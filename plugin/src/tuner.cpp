// Live placement tuner. While the hand-held object is open, numpad keys move and turn it relative to the camera:
//   4 / 6  left / right        8 / 2  up / down          7 / 9  closer / farther
//   1 / 3  roll (about the view direction)   / and *  yaw (about up)   - and +  pitch (about right)
//   .      toggle fine / coarse steps (2 mm, 1 deg / 1 cm, 5 deg)
//   0      reset to the NIF's own placement           5      save + log the offset
// The offset D is kept in the object's root space and applied to every geometry child: child.local = D * original.
// That is the same "child' = V * child" the asset generator uses, so a saved offset can be baked into the NIF.
// (The engine overwrites the AnimObject ROOT transform when it attaches it, so the root itself can't be moved.)

#include "pch.h"
#include "lang.h"
#include "tuner.h"
#include "ui.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <string>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <numbers>
#include <vector>

extern "C" __declspec(dllimport) short __stdcall GetAsyncKeyState(int a_vkey);

namespace
{
	constexpr const char* kRootName = "PhoneAnimObjectRoot";  // tools/make_test_assets.py ROOT_NAME
	constexpr const char* kSavePath = "Data\\SFSE\\Plugins\\PhoneTest_offset.txt";
	// pose while the Camera page is shown: the phone moves out of the part of the screen the camera crops
	constexpr const char* kCameraSavePath = "Data\\SFSE\\Plugins\\PhoneTest_camera_offset.txt";

	using Vec = std::array<float, 3>;
	using Mat = std::array<std::array<float, 3>, 3>;  // row-major, v_world = M * v_local

	struct Xf
	{
		Mat rot{ { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } } };
		Vec trans{ 0, 0, 0 };
	};

	Mat Mul(const Mat& a, const Mat& b)
	{
		Mat r{};
		for (int i = 0; i < 3; ++i)
			for (int j = 0; j < 3; ++j)
				r[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j];
		return r;
	}
	Vec Mul(const Mat& a, const Vec& v)
	{
		return { a[0][0] * v[0] + a[0][1] * v[1] + a[0][2] * v[2],
			a[1][0] * v[0] + a[1][1] * v[1] + a[1][2] * v[2],
			a[2][0] * v[0] + a[2][1] * v[1] + a[2][2] * v[2] };
	}
	Mat Transpose(const Mat& a)
	{
		Mat r{};
		for (int i = 0; i < 3; ++i)
			for (int j = 0; j < 3; ++j)
				r[i][j] = a[j][i];
		return r;
	}
	float Dot(const Vec& a, const Vec& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
	Vec   Cross(const Vec& a, const Vec& b) { return { a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0] }; }
	Vec   Scale(const Vec& a, float s) { return { a[0] * s, a[1] * s, a[2] * s }; }
	Vec   Add(const Vec& a, const Vec& b) { return { a[0] + b[0], a[1] + b[1], a[2] + b[2] }; }
	Vec   Sub(const Vec& a, const Vec& b) { return { a[0] - b[0], a[1] - b[1], a[2] - b[2] }; }
	Vec   Norm(const Vec& a)
	{
		const float l = std::sqrt(Dot(a, a));
		return l > 1e-6F ? Scale(a, 1.0F / l) : Vec{ 0, 0, 0 };
	}
	Mat AxisAngle(const Vec& n, float a_rad)
	{
		const float c = std::cos(a_rad), s = std::sin(a_rad), t = 1 - c;
		const float x = n[0], y = n[1], z = n[2];
		return { { { t * x * x + c, t * x * y - s * z, t * x * z + s * y },
			{ t * x * y + s * z, t * y * y + c, t * y * z - s * x },
			{ t * x * z - s * y, t * y * z + s * x, t * z * z + c } } };
	}

	Mat FromNi(const RE::NiMatrix3& m)
	{
		return { { { m.entry[0].x, m.entry[0].y, m.entry[0].z },
			{ m.entry[1].x, m.entry[1].y, m.entry[1].z },
			{ m.entry[2].x, m.entry[2].y, m.entry[2].z } } };
	}
	Vec FromNi(const RE::NiPoint3& p) { return { p.x, p.y, p.z }; }

	// D (root space) applied to a child's NIF transform: child' = D * child
	void Apply(const Xf& a_d, const RE::NiTransform& a_orig, RE::NiTransform& a_out)
	{
		const Mat r = Mul(a_d.rot, FromNi(a_orig.rotate));
		const Vec t = Add(Mul(a_d.rot, FromNi(a_orig.translate)), a_d.trans);
		for (int i = 0; i < 3; ++i) {
			a_out.rotate.entry[i].x = r[i][0];
			a_out.rotate.entry[i].y = r[i][1];
			a_out.rotate.entry[i].z = r[i][2];
		}
		a_out.translate = RE::NiPoint3{ t[0], t[1], t[2] };
		a_out.scale = a_orig.scale;
	}

	Xf                                  g_d;    // normal pose offset (dev keys only)
	Xf                                  g_cam;  // Camera page pose offset
	bool                                g_camReady = false;
	enum class Mode
	{
		kNone,    // NIF placement as baked
		kNormal,  // g_d (dev keys)
		kCamera,  // g_cam (Camera page)
	};
	Mode                                g_applied = Mode::kNone;
	bool                                g_fine = true;
	RE::NiPointer<RE::NiAVObject>       g_root;
	std::vector<RE::NiAVObject*>        g_children;  // owned by g_root
	std::vector<RE::NiTransform>        g_orig;
	std::chrono::steady_clock::time_point g_nextRepeat{};
	std::array<bool, 16>                g_wasDown{};

	void Save(const Xf& a_d, const char* a_path)
	{
		std::ofstream out(a_path, std::ios::trunc);
		out.imbue(std::locale::classic());
		out << "rot";
		for (auto& row : a_d.rot)
			for (float v : row)
				out << ' ' << v;
		out << "\ntrans " << a_d.trans[0] << ' ' << a_d.trans[1] << ' ' << a_d.trans[2] << '\n';
		REX::INFO("tuner: saved offset to {}: rot [{:.4f} {:.4f} {:.4f} / {:.4f} {:.4f} {:.4f} / {:.4f} {:.4f} {:.4f}] trans [{:.4f} {:.4f} {:.4f}]",
			a_path, a_d.rot[0][0], a_d.rot[0][1], a_d.rot[0][2], a_d.rot[1][0], a_d.rot[1][1], a_d.rot[1][2],
			a_d.rot[2][0], a_d.rot[2][1], a_d.rot[2][2], a_d.trans[0], a_d.trans[1], a_d.trans[2]);
	}

	bool LoadXf(const char* a_path, Xf& a_out)
	{
		std::ifstream in(a_path);
		if (!in) {
			return false;
		}
		in.imbue(std::locale::classic());
		std::string tag;
		Xf          d;
		if ((in >> tag) && tag == "rot") {
			for (auto& row : d.rot)
				for (float& v : row) in >> v;
		}
		if ((in >> tag) && tag == "trans") {
			in >> d.trans[0] >> d.trans[1] >> d.trans[2];
		}
		if (!in) {
			REX::WARN("tuner: could not parse {}", a_path);
			return false;
		}
		a_out = d;
		return true;
	}

	void Release()
	{
		g_root.reset();
		g_children.clear();
		g_orig.clear();
	}

	// NOTE: don't use RE::Main::GetWorldRoot(): its Address Library ID is 0 in this CommonLibSF for 1.16.244,
	// and resolving it raised SFSE's fatal "Invalid ID: 0" box (the game hung on the first F11). PlayerCamera's
	// singleton ID (937788) is valid.
	RE::NiAVObject* CameraRoot()
	{
		auto* cam = RE::PlayerCamera::GetSingleton();
		return cam && cam->cameraRoot ? cam->cameraRoot.get() : nullptr;
	}

	// ---- capture: hook the engine's AnimObject attach ----
	// FUN_14034D400 (Address Library ID 37954) attaches a loaded model to a skeleton: it looks up the bone named by
	// the model root's "Prn" extra data, zeroes the root's local translation and attaches it to that bone. Its only
	// direct caller is ID 37953 +0x6B0 (E8 call); rdx = pointer to the model root. Searching the scene by name never
	// found the 1st-person object, so take it here instead.
	// Layout as the engine itself uses it there: children array data at +0x138, uint16 count at +0x142;
	// local transform +0x40 (translation +0x70), world +0x80, name +0x10, parent +0x38, refcount +0x08.
	constexpr REL::ID          kAttachCaller{ 37953 };
	constexpr std::ptrdiff_t   kAttachCallOffset = 0x6B0;
	using Attach_t = bool (*)(void* a_ctx, void** a_model);
	Attach_t                      g_attachOrig = nullptr;
	std::mutex                    g_captureLock;
	RE::NiPointer<RE::NiAVObject> g_captured;  // newest attached object with our root name

	std::atomic_uint32_t g_attachCalls{ 0 };

	bool Attach_Hook(void* a_ctx, void** a_model)
	{
		const bool result = g_attachOrig(a_ctx, a_model);
		++g_attachCalls;
		if (a_model && *a_model) {
			auto*       obj = static_cast<RE::NiAVObject*>(*a_model);
			const char* n = obj->name.c_str();
			if (n && std::strcmp(n, kRootName) == 0) {
				std::lock_guard lk(g_captureLock);
				g_captured = RE::NiPointer<RE::NiAVObject>(obj);
			}
		}
		return result;
	}

	// ---- engine-layout helpers (CommonLibSF's NiNode::children and GetObjectByName slot are wrong for 1.16.244:
	// a scene dump through NiNode::children crashed, and searching through RE::NiAVObject::GetObjectByName found
	// nothing). The engine calls GetObjectByName through vtable +0x1F0 (FUN_14219B940, FUN_14034D400). ----
	RE::NiAVObject* const* Children(RE::NiAVObject* a_obj, std::uint16_t& a_count)
	{
		const auto base = reinterpret_cast<std::uintptr_t>(a_obj);
		a_count = *reinterpret_cast<const std::uint16_t*>(base + 0x142);
		return *reinterpret_cast<RE::NiAVObject* const* const*>(base + 0x138);
	}

	RE::NiAVObject* FindByName(RE::NiAVObject* a_root, const RE::BSFixedString& a_name)
	{
		using fn_t = RE::NiAVObject* (*)(RE::NiAVObject*, const RE::BSFixedString*);
		auto** vtbl = *reinterpret_cast<void***>(a_root);
		return reinterpret_cast<fn_t>(vtbl[0x1F0 / 8])(a_root, &a_name);
	}

	RE::NiAVObject* TopOf(RE::NiAVObject* a_obj)
	{
		for (int depth = 0; a_obj && a_obj->parent && depth < 64; ++depth) {
			a_obj = a_obj->parent;
		}
		return a_obj;
	}

	RE::NiAVObject* Player3D()
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player) {
			return nullptr;
		}
		const auto  guard = player->loadedData.LockRead();
		const auto* data = *guard;
		return data && data->data3D ? data->data3D.get() : nullptr;
	}

	const char* NameOf(RE::NiAVObject* a_obj)
	{
		const char* n = a_obj ? a_obj->name.c_str() : nullptr;
		return n && *n ? n : "(unnamed)";
	}

	void LogTree(const char* a_label, RE::NiAVObject* a_top)
	{
		if (!a_top) {
			REX::INFO("tuner: {}: none", a_label);
			return;
		}
		std::uint16_t count = 0;
		auto* const*  kids = Children(a_top, count);
		std::string   names;
		for (std::uint16_t i = 0; kids && i < count && i < 40; ++i) {
			names += ' ';
			names += NameOf(kids[i]);
		}
		REX::INFO("tuner: {}: top '{}' children[{}]:{}", a_label, NameOf(a_top), count, names);
	}

	std::chrono::steady_clock::time_point g_nextSearch{};
	bool                                  g_loggedTrees = false;

	RE::NiPointer<RE::NiAVObject> g_bone;  // the bone the object hangs on (1st-person L_AnimObject1), kept across opens

	// newest child of the bone with our root name: every open attaches a NEW object, and the old one can stay
	// under the bone, so the first match of a name search is not necessarily the visible one
	RE::NiAVObject* NewestOnBone()
	{
		if (!g_bone) {
			return nullptr;
		}
		std::uint16_t   count = 0;
		auto* const*    kids = Children(g_bone.get(), count);
		RE::NiAVObject* newest = nullptr;
		for (std::uint16_t i = 0; kids && i < count && i < 256; ++i) {
			const char* n = kids[i] ? kids[i]->name.c_str() : nullptr;
			if (n && std::strcmp(n, kRootName) == 0) {
				newest = kids[i];
			}
		}
		return newest;
	}

	// object to tune: newest on the remembered bone; else the attach hook's capture; else a scene search by name
	RE::NiAVObject* Locate(const char*& a_via)
	{
		if (auto* o = NewestOnBone()) {
			a_via = "bone children";
			return o;
		}
		{
			std::lock_guard lk(g_captureLock);
			if (g_captured && g_captured->parent) {
				a_via = "attach hook";
				return g_captured.get();
			}
		}
		const auto now = std::chrono::steady_clock::now();
		if (now < g_nextSearch) {
			return nullptr;
		}
		g_nextSearch = now + std::chrono::milliseconds(500);
		const RE::BSFixedString name{ kRootName };
		auto* p3d = Player3D();
		auto* cam = CameraRoot();
		struct Start
		{
			const char*     label;
			RE::NiAVObject* obj;
		};
		const Start starts[] = { { "player 3D scene top", TopOf(p3d) }, { "camera root scene top", TopOf(cam) } };
		for (const auto& st : starts) {
			if (st.obj) {
				if (auto* f = FindByName(st.obj, name)) {
					a_via = st.label;
					if (f->parent) {
						g_bone = RE::NiPointer<RE::NiAVObject>(f->parent);
						REX::INFO("tuner: object hangs on '{}'; following that bone from now on", NameOf(f->parent));
						if (auto* o = NewestOnBone()) {
							return o;
						}
					}
					return f;
				}
			}
		}
		if (!g_loggedTrees) {
			g_loggedTrees = true;
			REX::INFO("tuner: '{}' not found (attach hook calls so far: {}); scene roots:", kRootName, g_attachCalls.load());
			LogTree("player 3D scene top", TopOf(p3d));
			LogTree("camera root scene top", TopOf(cam));
		}
		return nullptr;
	}

	// adopt a new object and remember its children's NIF transforms
	void Adopt()
	{
		const char* via = "";
		auto*       obj = Locate(via);
		if (!obj || obj == g_root.get()) {
			return;
		}
		Release();
		std::uint16_t count = 0;
		auto* const*  kids = Children(obj, count);
		if (!kids || count == 0 || count > 64) {
			REX::WARN("tuner: found '{}' via {} but its children look wrong (count {})", kRootName, via, count);
			return;
		}
		g_root = RE::NiPointer<RE::NiAVObject>(obj);
		for (std::uint16_t i = 0; i < count; ++i) {
			if (auto* c = kids[i]) {
				g_children.push_back(c);
				g_orig.push_back(c->local);
			}
		}
		const auto& t = g_root->world.translate;
		REX::INFO("tuner: found '{}' via {} with {} children, world pos ({:.2f}, {:.2f}, {:.2f})", kRootName, via, g_children.size(), t.x, t.y, t.z);
	}

	// Camera-relative axes (right, up, forward) in world space, picked from the camera's own axes so they don't
	// depend on its matrix convention: forward = camera axis closest to the camera->object direction,
	// up = remaining axis closest to world +Z.
	bool CameraAxes(const Vec& a_objPos, Vec& a_right, Vec& a_up, Vec& a_fwd)
	{
		auto* camRoot = CameraRoot();
		if (!camRoot) {
			return false;
		}
		const auto& cw = camRoot->world;
		const Mat   cr = FromNi(cw.rotate);
		const Vec   dir = Norm(Sub(a_objPos, FromNi(cw.translate)));
		std::array<Vec, 3> axes{ Vec{ cr[0][0], cr[1][0], cr[2][0] }, Vec{ cr[0][1], cr[1][1], cr[2][1] }, Vec{ cr[0][2], cr[1][2], cr[2][2] } };
		int   fi = 0;
		float best = -2;
		for (int i = 0; i < 3; ++i) {
			const float d = std::abs(Dot(axes[i], dir));
			if (d > best) best = d, fi = i;
		}
		a_fwd = Dot(axes[fi], dir) >= 0 ? axes[fi] : Scale(axes[fi], -1);
		int ui = -1;
		best = -2;
		for (int i = 0; i < 3; ++i) {
			if (i == fi) continue;
			const float d = std::abs(axes[i][2]);
			if (d > best) best = d, ui = i;
		}
		a_up = axes[ui][2] >= 0 ? axes[ui] : Scale(axes[ui], -1);
		a_right = Cross(a_fwd, a_up);
		return true;
	}

	void Step(int a_key, Xf& g_d)  // the parameter shadows the global: tunes whichever pose is active
	{
		if (!g_root) {
			REX::INFO("tuner: object not found yet (open it first)");
			return;
		}
		const float move = g_fine ? 0.002F : 0.01F;
		const float turn = (g_fine ? 1.0F : 5.0F) * std::numbers::pi_v<float> / 180.0F;

		const Mat   rootRot = FromNi(g_root->world.rotate);
		const float rootScale = g_root->world.scale > 1e-6F ? g_root->world.scale : 1.0F;
		const Mat   toLocal = Transpose(rootRot);
		Vec right, up, fwd;
		if (!CameraAxes(FromNi(g_root->world.translate), right, up, fwd)) {
			return;
		}

		Vec worldMove{ 0, 0, 0 };
		Vec worldAxis{ 0, 0, 0 };
		float angle = 0;
		switch (a_key) {
		case 0x64: worldMove = Scale(right, -move); break;  // 4
		case 0x66: worldMove = Scale(right, move); break;   // 6
		case 0x68: worldMove = Scale(up, move); break;      // 8
		case 0x62: worldMove = Scale(up, -move); break;     // 2
		case 0x67: worldMove = Scale(fwd, -move); break;    // 7 closer
		case 0x69: worldMove = Scale(fwd, move); break;     // 9 farther
		case 0x61: worldAxis = fwd, angle = turn; break;    // 1 roll
		case 0x63: worldAxis = fwd, angle = -turn; break;   // 3 roll
		case 0x6F: worldAxis = up, angle = turn; break;     // / yaw
		case 0x6A: worldAxis = up, angle = -turn; break;    // * yaw
		case 0x6D: worldAxis = right, angle = turn; break;  // - pitch
		case 0x6B: worldAxis = right, angle = -turn; break; // + pitch
		default: return;
		}

		if (angle == 0) {
			g_d.trans = Add(g_d.trans, Scale(Mul(toLocal, worldMove), 1.0F / rootScale));
			return;
		}
		// rotate about the object's centre (mean of the children's positions), not the root origin
		Vec c0{ 0, 0, 0 };
		for (auto& o : g_orig) c0 = Add(c0, FromNi(o.translate));
		if (!g_orig.empty()) c0 = Scale(c0, 1.0F / g_orig.size());
		const Vec c = Add(Mul(g_d.rot, c0), g_d.trans);
		const Mat rl = Mul(toLocal, Mul(AxisAngle(Norm(worldAxis), angle), rootRot));
		g_d.rot = Mul(rl, g_d.rot);
		g_d.trans = Add(Mul(rl, Sub(g_d.trans, c)), c);
	}

	// a + (b - a) * w for both parts; the rotation is re-orthonormalised (Gram-Schmidt on the rows)
	Xf Blend(const Xf& a_a, const Xf& a_b, float a_w)
	{
		Xf r;
		for (int i = 0; i < 3; ++i) {
			r.trans[i] = a_a.trans[i] + (a_b.trans[i] - a_a.trans[i]) * a_w;
			for (int j = 0; j < 3; ++j) {
				r.rot[i][j] = a_a.rot[i][j] + (a_b.rot[i][j] - a_a.rot[i][j]) * a_w;
			}
		}
		Vec x{ r.rot[0][0], r.rot[0][1], r.rot[0][2] }, y{ r.rot[1][0], r.rot[1][1], r.rot[1][2] };
		x = Norm(x);
		y = Norm(Sub(y, Scale(x, Dot(x, y))));
		const Vec z = Cross(x, y);
		r.rot = { { { x[0], x[1], x[2] }, { y[0], y[1], y[2] }, { z[0], z[1], z[2] } } };
		return r;
	}

	// move a pose by a camera-relative offset (right, up, forward), the way the numpad does
	void MoveCameraSpace(Xf& a_d, float a_right, float a_up, float a_fwd)
	{
		if (!g_root) {
			return;
		}
		const Mat   toLocal = Transpose(FromNi(g_root->world.rotate));
		const float rootScale = g_root->world.scale > 1e-6F ? g_root->world.scale : 1.0F;
		Vec right, up, fwd;
		if (!CameraAxes(FromNi(g_root->world.translate), right, up, fwd)) {
			return;
		}
		const Vec world = Add(Add(Scale(right, a_right), Scale(up, a_up)), Scale(fwd, a_fwd));
		a_d.trans = Add(a_d.trans, Scale(Mul(toLocal, world), 1.0F / rootScale));
	}


	// ---- arm pose: rotation offsets for the 1st-person left arm bones while the Camera page is shown ----
	// Bones are found from the phone up (PhoneAnimObjectRoot < L_AnimObject1 < L_Wrist < L_Forearm < L_Biceps <
	// L_Clavicle), so the 3rd-person skeleton with the same names is never touched. Every tick: take each bone's local
	// transform as the animation left it (or the copy kept from the last tick when nothing rewrote it), multiply in
	// its offset (blended by the camera-pose weight), then recompute the world transforms of the arm subtree (arm,
	// hand, phone) with the engine's convention, detected from the untouched transforms.
	constexpr const char* kArmSavePath = "Data\\SFSE\\Plugins\\PhoneTest_arm_pose.txt";
	constexpr int         kBones = 4;  // 0 clavicle, 1 upper arm, 2 forearm, 3 wrist
	constexpr const char* kBoneLabels[kBones] = { "锁骨", "上臂", "前臂", "手腕" };

	struct Bone
	{
		RE::NiAVObject* node = nullptr;
		RE::NiTransform animLocal{};  // the animation's local transform (before our offset)
		RE::NiTransform written{};    // what we wrote last tick
		bool            haveWritten = false;
	};
	struct ArmState
	{
		Bone bones[kBones];
		int  conv = -1;  // 0: world = parentRot * localRot, 1: localRot * parentRot (1 measured in game)
		int  ticks = 0, rewritten = 0;
		std::chrono::steady_clock::time_point nextLog{};
	} g_arm;

	constexpr Mat kIdentity{ { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } } };
	Mat  g_boneRot[kBones] = { kIdentity, kIdentity, kIdentity, kIdentity };  // tuned offsets
	bool g_armLoaded = false;
	int  g_target = 0;  // numpad target on the Camera page: 0 phone, 1..4 bones

	bool SameXf(const RE::NiTransform& a, const RE::NiTransform& b)
	{
		return std::memcmp(&a, &b, sizeof(RE::NiTransform)) == 0;
	}

	Mat MatOf(const RE::NiTransform& a_x) { return FromNi(a_x.rotate); }
	void SetMat(RE::NiMatrix3& a_out, const Mat& a_m)
	{
		for (int i = 0; i < 3; ++i) {
			a_out.entry[i].x = a_m[i][0];
			a_out.entry[i].y = a_m[i][1];
			a_out.entry[i].z = a_m[i][2];
		}
	}

	// world of a child from its parent's world and its local transform, in convention a_conv
	void ComposeWorld(const RE::NiTransform& a_parentWorld, const RE::NiTransform& a_local, int a_conv, RE::NiTransform& a_out)
	{
		const Mat   pr = MatOf(a_parentWorld), lr = MatOf(a_local);
		const float ps = a_parentWorld.scale;
		const Vec   lt = Scale(FromNi(a_local.translate), ps);
		Mat         r;
		Vec         tr;
		if (a_conv == 0) {
			r = Mul(pr, lr);
			tr = Add(FromNi(a_parentWorld.translate), Mul(pr, lt));
		} else {
			r = Mul(lr, pr);
			tr = Add(FromNi(a_parentWorld.translate), Mul(Transpose(pr), lt));
		}
		SetMat(a_out.rotate, r);
		a_out.translate = RE::NiPoint3{ tr[0], tr[1], tr[2] };
		a_out.scale = ps * a_local.scale;
	}

	float XfError(const RE::NiTransform& a, const RE::NiTransform& b)
	{
		float e = 0;
		const Mat ma = MatOf(a), mb = MatOf(b);
		for (int i = 0; i < 3; ++i)
			for (int j = 0; j < 3; ++j)
				e += std::abs(ma[i][j] - mb[i][j]);
		e += std::abs(a.translate.x - b.translate.x) + std::abs(a.translate.y - b.translate.y) + std::abs(a.translate.z - b.translate.z);
		return e;
	}

	// Recompute world transforms below a bone. Only objects of the bone's own class (NiNode, same vtable) are
	// descended into; the phone root's children (geometry) only get their own world. Descending into geometry as if
	// it had a children array crashed the game.
	void UpdateSubtreeWorld(RE::NiAVObject* a_obj, int a_conv, const void* a_nodeVtbl, int a_depth = 0)
	{
		if (!a_obj || !a_obj->parent || a_depth > 24) {
			return;
		}
		ComposeWorld(a_obj->parent->world, a_obj->local, a_conv, a_obj->world);
		const void* vtbl = *reinterpret_cast<void* const*>(a_obj);
		const bool  isPhone = a_obj == g_root.get();
		if (vtbl != a_nodeVtbl && !isPhone) {
			return;
		}
		if (isPhone) {
			for (auto* c : g_children) {
				if (c) ComposeWorld(a_obj->world, c->local, a_conv, c->world);
			}
			return;
		}
		std::uint16_t count = 0;
		auto* const*  kids = Children(a_obj, count);
		for (std::uint16_t i = 0; kids && i < count && i < 64; ++i) {
			UpdateSubtreeWorld(kids[i], a_conv, a_nodeVtbl, a_depth + 1);
		}
	}

	void SaveArm()
	{
		std::ofstream out(kArmSavePath, std::ios::trunc);
		out.imbue(std::locale::classic());
		for (int b = 0; b < kBones; ++b) {
			out << "bone " << b;
			for (auto& row : g_boneRot[b])
				for (float v : row)
					out << ' ' << v;
			out << '\n';
		}
		REX::INFO("arm: pose saved to {}", kArmSavePath);
	}

	// the photo pose tuned in game (repo defaults/PhoneTest_arm_pose.txt): used when no file is installed
	constexpr Mat kDefaultBoneRot[kBones] = {
		{ { { 0.990331F, 0.122923F, -0.0643091F }, { -0.122129F, 0.992384F, 0.0161445F }, { 0.0658037F, -0.00813428F, 0.997801F } } },
		{ { { 0.998979F, 0.00428132F, -0.0449942F }, { 0.00353155F, 0.985067F, 0.172141F }, { 0.0450592F, -0.172124F, 0.984046F } } },
		{ { { 0.997566F, -0.0039548F, 0.0696264F }, { -0.000913388F, 0.997565F, 0.0697486F }, { -0.0697326F, -0.0696423F, 0.995132F } } },
		{ { { 0.771979F, -0.210935F, 0.599631F }, { 0.0849723F, 0.969114F, 0.231515F }, { -0.629945F, -0.127772F, 0.766059F } } },
	};

	void LoadArm()
	{
		std::ifstream in(kArmSavePath);
		if (!in) {
			for (int b = 0; b < kBones; ++b) g_boneRot[b] = kDefaultBoneRot[b];
			REX::INFO("arm: no saved pose ({}), using the built-in photo pose", kArmSavePath);
			return;
		}
		in.imbue(std::locale::classic());
		std::string tag;
		int         b = 0;
		while (in >> tag >> b) {
			Mat m{};
			for (auto& row : m)
				for (float& v : row) in >> v;
			if (tag == "bone" && b >= 0 && b < kBones && in) {
				g_boneRot[b] = m;
			}
		}
		REX::INFO("arm: pose loaded from {}", kArmSavePath);
	}

	bool FindArm()
	{
		if (!g_root || !g_root->parent || !g_root->parent->parent) {
			return false;
		}
		RE::NiAVObject* chain[kBones]{};
		chain[3] = g_root->parent->parent;  // L_AnimObject1 -> L_Wrist
		for (int b = 2; b >= 0; --b) {
			chain[b] = chain[b + 1] ? chain[b + 1]->parent : nullptr;
		}
		if (!chain[0] || !chain[0]->parent) {
			return false;
		}
		if (chain[1] != g_arm.bones[1].node) {
			g_arm = {};
			for (int b = 0; b < kBones; ++b) {
				g_arm.bones[b].node = chain[b];
			}
			REX::INFO("arm: chain {} < {} < {} < {} < {}", NameOf(chain[3]), NameOf(chain[2]), NameOf(chain[1]), NameOf(chain[0]),
				NameOf(chain[0]->parent));
			RE::NiTransform w0{}, w1{};
			ComposeWorld(chain[1]->parent->world, chain[1]->local, 0, w0);
			ComposeWorld(chain[1]->parent->world, chain[1]->local, 1, w1);
			const float e0 = XfError(w0, chain[1]->world), e1 = XfError(w1, chain[1]->world);
			g_arm.conv = e0 <= e1 ? 0 : 1;
			REX::INFO("arm: world convention {} (error {:.5f} vs {:.5f})", g_arm.conv, e0, e1);
		}
		return true;
	}

	// ---- applying the offsets (see Update3D_Hook below for where) ----
	std::atomic<RE::NiAVObject*> g_hookBones[kBones]{};
	std::atomic<float>           g_armWeight{ 0.0F };
	std::mutex                   g_armLock;  // g_arm.bones[] state touched from the update job threads

	void ApplyBoneOffset(int a_bone)
	{
		std::lock_guard lk(g_armLock);
		auto&           bone = g_arm.bones[a_bone];
		if (!bone.node) {
			return;
		}
		const float w = g_armWeight.load();
		if (a_bone == 1) ++g_arm.ticks;
		const bool  fresh = !bone.haveWritten || !SameXf(bone.node->local, bone.written);
		if (fresh) {
			bone.animLocal = bone.node->local;  // the animation's value for this frame
			if (a_bone == 1) ++g_arm.rewritten;
		}
		if (w <= 0.0F) {
			if (!fresh) {
				bone.node->local = bone.animLocal;  // take our offset back out
			}
			bone.haveWritten = false;
			return;
		}
		Xf off;
		off.rot = g_boneRot[a_bone];
		const Mat       d = w >= 1.0F ? g_boneRot[a_bone] : Blend(Xf{}, off, w).rot;
		RE::NiTransform out = bone.animLocal;
		SetMat(out.rotate, Mul(MatOf(bone.animLocal), d));
		bone.node->local = out;
		bone.written = out;
		bone.haveWritten = true;
	}

	// ---- the skeleton's pose arrays ----
	// FUN_142BE93F0 (the writer of the bones' world transforms) does not read the NiNode locals: it works on the
	// pose component at node +0x180 of a skeleton node, whose +0x10 block holds arrays of 0x40-byte transforms
	// (+0x08 -> +0x10: locals, +0x10 -> +0x10: model space; u16 bone count at component +0x78). The NiNode locals are
	// copies (FUN_141A3DC50). So the offsets go into the local array: the component is found by walking up from
	// the clavicle, the bones' indices by matching their NiNode locals against the array entries.
	bool SafeCopy(const void* a_src, void* a_dst, std::size_t a_size)
	{
		__try {
			std::memcpy(a_dst, a_src, a_size);
			return true;
		} __except (1) {
			return false;
		}
	}

	std::uintptr_t ReadPtr(std::uintptr_t a_addr)
	{
		std::uintptr_t v = 0;
		return (a_addr > 0x10000 && SafeCopy(reinterpret_cast<const void*>(a_addr), &v, sizeof(v))) ? v : 0;
	}

	struct PoseRef
	{
		std::uintptr_t component = 0;
		std::uintptr_t locals = 0;  // array of 0x40-byte transforms
		std::uint16_t  count = 0;
		int            index[kBones] = { -1, -1, -1, -1 };
		bool           searched = false;
		bool           ok = false;
	} g_pose;

	float EntryDiff(std::uintptr_t a_entry, const RE::NiTransform& a_local)
	{
		float e[16];
		if (!SafeCopy(reinterpret_cast<const void*>(a_entry), e, sizeof(e))) {
			return 1e9F;
		}
		const float* l = reinterpret_cast<const float*>(&a_local);
		float        d = 0;
		for (int k = 0; k < 15; ++k) {  // rotation rows (with padding), translation; not the scale slot
			d += std::abs(e[k] - l[k]);
		}
		return d;
	}

	// (g_armLock held) find the pose component and our bones' indices once per arm chain
	void FindPose()
	{
		if (g_pose.searched || !g_arm.bones[0].node) {
			return;
		}
		g_pose = {};
		g_pose.searched = true;
		int depth = 0;
		for (auto* n = g_arm.bones[0].node->parent; n && depth < 16; n = n->parent, ++depth) {
			const auto comp = ReadPtr(reinterpret_cast<std::uintptr_t>(n) + 0x180);
			if (!comp) {
				continue;
			}
			const auto    block = ReadPtr(comp + 0x10);
			const auto    locals = block ? ReadPtr(ReadPtr(block + 0x08) + 0x10) : 0;
			std::uint16_t count = 0;
			SafeCopy(reinterpret_cast<const void*>(comp + 0x78), &count, sizeof(count));
			REX::INFO("arm: pose candidate on '{}' (depth {}): component {:X} block {:X} locals {:X} count {}", NameOf(n), depth, comp,
				block, locals, count);
			if (!locals || count < 8 || count > 1024) {
				continue;
			}
			bool all = true;
			for (int b = 0; b < kBones; ++b) {
				float best = 1e9F;
				int   bestI = -1;
				for (int i = 0; i < count; ++i) {
					const float d = EntryDiff(locals + std::uintptr_t(i) * 0x40, g_arm.bones[b].animLocal);  // the animation's value, before our offset
					if (d < best) best = d, bestI = i;
				}
				REX::INFO("arm: {} -> pose index {} (difference {:.5f})", NameOf(g_arm.bones[b].node), bestI, best);
				if (best < 0.01F) {
					g_pose.index[b] = bestI;
				} else {
					all = false;
				}
			}
			if (all) {
				g_pose.component = comp;
				g_pose.locals = locals;
				g_pose.count = count;
				g_pose.ok = true;
				REX::INFO("arm: posing through the local pose array of '{}'", NameOf(n));
				return;
			}
		}
		REX::WARN("arm: no pose array matched the arm bones");
	}

	// (g_armLock held) multiply the offsets into the pose array entries (re-read every frame: the animation refills it)
	void ApplyPoseOffsets(float a_w)
	{
		if (!g_pose.ok || a_w <= 0.0F) {
			return;
		}
		// the array may move when the skeleton is rebuilt: re-resolve it from the component
		const auto block = ReadPtr(g_pose.component + 0x10);
		const auto locals = block ? ReadPtr(ReadPtr(block + 0x08) + 0x10) : 0;
		if (locals != g_pose.locals) {
			g_pose = {};  // search again next time
			return;
		}
		for (int b = 0; b < kBones; ++b) {
			auto* e = reinterpret_cast<RE::NiTransform*>(locals + std::uintptr_t(g_pose.index[b]) * 0x40);
			Xf    off;
			off.rot = g_boneRot[b];
			const Mat d = a_w >= 1.0F ? g_boneRot[b] : Blend(Xf{}, off, a_w).rot;
			SetMat(e->rotate, Mul(MatOf(*e), d));
		}
	}

	// ---- the window that works: right after the skeleton's pose component update ----
	// Inside the 1st-person update, the skeleton root's Update (FUN_142419B00) calls its pose component's vtable
	// +0x10 (FUN_14064E750 -> FUN_142BE93F0), which writes every bone's world transform from the pose arrays, and
	// then FUN_14241AB80, which hands the transforms to the renderer (render-instance tables). Writes before the
	// component call are overwritten, writes after FUN_14241AB80 come a frame late; the in-game tuning session only
	// worked because the task happened to land in between. So: hook the component's +0x10 and override the arm
	// there, right after it returns: offsets into the locals, arm subtree worlds recomputed (convention 1).
	using PoseUpdate_t = std::uintptr_t (*)(void*, void*, void*, void*);
	struct CompHook
	{
		void**       vtbl = nullptr;
		PoseUpdate_t orig = nullptr;
	};
	CompHook                   g_compHooks[4];
	std::atomic<std::uintptr_t> g_poseComp{ 0 };
	std::atomic_uint32_t       g_poseUpdates{ 0 };

	// ---- the pose array itself ----
	// Overriding the NiNode transforms right after the component update changed nothing on screen (offsets applied
	// every frame, logged): the renderer takes the bones from the component's arrays, the nodes are copies. So the
	// offsets go into the component's local array before it computes. Bone indices: a table of node pointers in or
	// under the component whose entry for the clavicle points at our clavicle node.
	struct ArrState
	{
		std::uintptr_t comp = 0;
		std::uintptr_t table = 0;  // node pointer table (bone index -> NiNode*)
		int            index[kBones] = { -1, -1, -1, -1 };
		bool           searched = false;
		bool           ok = false;
		float          animRot[kBones][12]{};  // the animation's rotation rows of each entry (with padding)
		float          written[kBones][12]{};
		bool           haveWritten[kBones]{};
	} g_arr;

	// (g_armLock held)
	void FindBoneTable(std::uintptr_t a_comp, std::uint16_t a_count)
	{
		g_arr = {};
		g_arr.comp = a_comp;
		g_arr.searched = true;
		const auto clav = reinterpret_cast<std::uintptr_t>(g_arm.bones[0].node);
		// candidate tables: pointers stored in the component (0x200 bytes), in its +0x10 block (0x100 bytes), and
		// the +0x10 of each of those (arrays sit at +0x10 of a header, like the transform arrays)
		std::uintptr_t cands[256];
		int            n = 0;
		for (std::uintptr_t base : { a_comp, ReadPtr(a_comp + 0x10) }) {
			for (std::uintptr_t off = 0; base && off < (base == a_comp ? 0x200u : 0x100u) && n < 250; off += 8) {
				const auto v = ReadPtr(base + off);
				if (v > 0x10000) {
					cands[n++] = v;
					const auto inner = ReadPtr(v + 0x10);
					if (inner > 0x10000) cands[n++] = inner;
				}
			}
		}
		for (int c = 0; c < n; ++c) {
			const auto tbl = cands[c];
			for (int i = 0; i < a_count; ++i) {
				if (ReadPtr(tbl + std::uintptr_t(i) * 8) != clav) {
					continue;
				}
				// clavicle at i: look up the other bones in the same table
				int  idx[kBones] = { i, -1, -1, -1 };
				bool all = true;
				for (int b = 1; b < kBones; ++b) {
					const auto node = reinterpret_cast<std::uintptr_t>(g_arm.bones[b].node);
					for (int k = 0; k < a_count; ++k) {
						if (ReadPtr(tbl + std::uintptr_t(k) * 8) == node) {
							idx[b] = k;
							break;
						}
					}
					all = all && idx[b] >= 0;
				}
				REX::INFO("arm: node table {:X}: clavicle {}, upper arm {}, forearm {}, wrist {}", tbl, idx[0], idx[1], idx[2], idx[3]);
				if (all) {
					g_arr.table = tbl;
					for (int b = 0; b < kBones; ++b) g_arr.index[b] = idx[b];
					g_arr.ok = true;
					return;
				}
			}
		}
		REX::WARN("arm: no bone node table found in the pose component ({} candidates)", n);
	}

	// (g_armLock held) before the component computes: offsets into the local array entries
	void ApplyArrayOffsets(std::uintptr_t a_comp)
	{
		const auto    block = ReadPtr(a_comp + 0x10);
		const auto    locals = block ? ReadPtr(ReadPtr(block + 0x08) + 0x10) : 0;
		std::uint16_t count = 0;
		SafeCopy(reinterpret_cast<const void*>(a_comp + 0x78), &count, sizeof(count));
		if (!locals || count < 8 || count > 1024) {
			return;
		}
		if (!g_arr.searched || g_arr.comp != a_comp) {
			FindBoneTable(a_comp, count);
		}
		if (!g_arr.ok) {
			return;
		}
		const float w = g_armWeight.load();
		for (int b = 0; b < kBones; ++b) {
			auto* e = reinterpret_cast<float*>(locals + std::uintptr_t(g_arr.index[b]) * 0x40);
			const bool fresh = !g_arr.haveWritten[b] || std::memcmp(e, g_arr.written[b], sizeof(g_arr.written[b])) != 0;
			if (fresh) {
				std::memcpy(g_arr.animRot[b], e, sizeof(g_arr.animRot[b]));
			}
			if (w <= 0.0F) {
				if (!fresh) std::memcpy(e, g_arr.animRot[b], sizeof(g_arr.animRot[b]));
				g_arr.haveWritten[b] = false;
				continue;
			}
			RE::NiTransform anim{};
			std::memcpy(&anim, g_arr.animRot[b], sizeof(g_arr.animRot[b]));
			Xf off;
			off.rot = g_boneRot[b];
			const Mat d = w >= 1.0F ? g_boneRot[b] : Blend(Xf{}, off, w).rot;
			RE::NiTransform out = anim;
			SetMat(out.rotate, Mul(MatOf(anim), d));
			std::memcpy(e, &out, sizeof(g_arr.written[b]));
			std::memcpy(g_arr.written[b], e, sizeof(g_arr.written[b]));
			g_arr.haveWritten[b] = true;
		}
	}

	// (g_armLock held) after the component computed: recompute the model-space array (block +0x10 -> +0x10) for the
	// clavicle's subtree from the locals with our offsets. FUN_142BE93F0 builds model[i] = local[i] (x) model[parent[i]]
	// (convention 1; parent indices: u16 table at block +0x28), but it can also take the pose from a controller
	// (+0x70) without reading the locals, which is why writing the locals before the call was not enough.
	void ApplyModelArray(std::uintptr_t a_comp)
	{
		if (!g_arr.ok || g_armWeight.load() <= 0.0F) {
			return;
		}
		const auto    block = ReadPtr(a_comp + 0x10);
		const auto    locals = block ? ReadPtr(ReadPtr(block + 0x08) + 0x10) : 0;
		const auto    model = block ? ReadPtr(ReadPtr(block + 0x10) + 0x10) : 0;
		const auto    parents = block ? ReadPtr(block + 0x28) : 0;
		std::uint16_t count = 0;
		SafeCopy(reinterpret_cast<const void*>(a_comp + 0x78), &count, sizeof(count));
		if (!locals || !model || !parents || count < 8 || count > 1024) {
			return;
		}
		static std::uint16_t par[1024];
		if (!SafeCopy(reinterpret_cast<const void*>(parents), par, std::size_t(count) * 2)) {
			return;
		}
		static bool inSub[1024];
		const int   clav = g_arr.index[0];
		const float w = g_armWeight.load();
		int         n = 0;
		for (int i = 0; i < count; ++i) {
			inSub[i] = i == clav || (par[i] < count && par[i] < i && inSub[par[i]]);
			if (!inSub[i] || i == clav && par[i] >= count) {
				continue;
			}
			++n;
			auto* L = reinterpret_cast<RE::NiTransform*>(locals + std::uintptr_t(i) * 0x40);
			auto* M = reinterpret_cast<RE::NiTransform*>(model + std::uintptr_t(i) * 0x40);
			const auto* P = reinterpret_cast<const RE::NiTransform*>(model + std::uintptr_t(par[i]) * 0x40);
			RE::NiTransform local = *L;
			for (int b = 0; b < kBones; ++b) {
				if (g_arr.index[b] != i) continue;
				// the local already carries our offset when nothing rewrote it since ApplyArrayOffsets
				const bool ours = g_arr.haveWritten[b] && std::memcmp(L, g_arr.written[b], sizeof(g_arr.written[b])) == 0;
				if (!ours) {
					Xf off;
					off.rot = g_boneRot[b];
					const Mat d = w >= 1.0F ? g_boneRot[b] : Blend(Xf{}, off, w).rot;
					SetMat(local.rotate, Mul(MatOf(local), d));
				}
			}
			ComposeWorld(*P, local, 1, *M);
		}
		static bool logged = false;
		if (!logged) {
			logged = true;
			REX::INFO("arm: model array {:X}, parents {:X}: {} bones recomputed below the clavicle (index {}, parent {})", model, parents, n,
				clav, par[clav]);
		}
	}

	std::uintptr_t PoseUpdate_Hook(void* a_comp, void* a_2, void* a_3, void* a_4)
	{
		PoseUpdate_t orig = nullptr;
		void** const vtbl = *reinterpret_cast<void***>(a_comp);
		for (const auto& h : g_compHooks) {
			if (h.vtbl == vtbl) {
				orig = h.orig;
				break;
			}
		}
		if (reinterpret_cast<std::uintptr_t>(a_comp) == g_poseComp.load() && g_hookBones[0].load()) {
			std::lock_guard lk(g_armLock);
			ApplyArrayOffsets(reinterpret_cast<std::uintptr_t>(a_comp));
		}
		const std::uintptr_t r = orig ? orig(a_comp, a_2, a_3, a_4) : 0;
		if (reinterpret_cast<std::uintptr_t>(a_comp) == g_poseComp.load() && g_hookBones[0].load()) {
			{
				std::lock_guard lk(g_armLock);
				ApplyModelArray(reinterpret_cast<std::uintptr_t>(a_comp));
			}
			++g_poseUpdates;
			for (int b = 0; b < kBones; ++b) {
				ApplyBoneOffset(b);
			}
			if (g_armWeight.load() > 0.0F || g_arm.bones[0].haveWritten) {
				auto* top = g_hookBones[0].load();
				UpdateSubtreeWorld(top, 1, *reinterpret_cast<void* const*>(top));
			}
		}
		return r;
	}

	// (task) the pose component of the arm's skeleton: the first ancestor of the clavicle whose +0x180 object has a
	// plausible pose block; hook its class's +0x10 once
	bool g_poseSearched = false;  // reset with the arm chain

	void FindPoseComponent()
	{
		if (g_poseComp.load() || g_poseSearched || !g_arm.bones[0].node) {
			return;
		}
		g_poseSearched = true;
		int depth = 0;
		for (auto* n = g_arm.bones[0].node->parent; n && depth < 16; n = n->parent, ++depth) {
			const auto    comp = ReadPtr(reinterpret_cast<std::uintptr_t>(n) + 0x180);
			const auto    block = comp ? ReadPtr(comp + 0x10) : 0;
			const auto    locals = block ? ReadPtr(ReadPtr(block + 0x08) + 0x10) : 0;
			std::uint16_t count = 0;
			if (comp) SafeCopy(reinterpret_cast<const void*>(comp + 0x78), &count, sizeof(count));
			if (!locals || count < 8 || count > 1024) {
				continue;
			}
			void** const vtbl = *reinterpret_cast<void***>(comp);
			bool         hooked = false;
			for (auto& h : g_compHooks) {
				if (h.vtbl == vtbl) {
					hooked = true;
					break;
				}
			}
			for (auto& h : g_compHooks) {
				if (hooked) break;
				if (!h.vtbl) {
					std::uint32_t old = 0;
					if (!REX::W32::VirtualProtect(&vtbl[2], sizeof(void*), REX::W32::PAGE_EXECUTE_READWRITE, &old)) {
						REX::ERROR("arm: VirtualProtect failed");
						return;
					}
					h.orig = reinterpret_cast<PoseUpdate_t>(vtbl[2]);
					h.vtbl = vtbl;
					vtbl[2] = reinterpret_cast<void*>(&PoseUpdate_Hook);
					REX::W32::VirtualProtect(&vtbl[2], sizeof(void*), old, &old);
					hooked = true;
				}
			}
			g_poseComp = comp;
			REX::INFO("arm: pose component {:X} on '{}' ({} bones), vtable {:X} +0x10 hooked", comp, NameOf(n), count,
				reinterpret_cast<std::uintptr_t>(vtbl));
			return;
		}
		REX::INFO("arm: no pose component above the clavicle: the arm is driven by its NiNodes (task writes)");
	}

	// ---- the reliable point: the player's 1st-person 3D update ----
	// A hardware-watchpoint trace (bonetrace.cpp) showed the animation job writing the bones' locals and the
	// player's 3D update FUN_141A3D8C0 (ID 102725) computing their world transforms through
	// FUN_141A1CB20(actor, dt, root, camera, firstPerson, ...) (ID 102618); both run on job threads, which is why
	// writes from the SFSE task and the NiNode::UpdateWorldData hook only worked in some sessions. The call at
	// +0x161 is the first-person one: put our offsets into the locals right before it.
	constexpr REL::ID        kPlayerUpdate3D{ 102725 };
	constexpr std::ptrdiff_t kFirstPersonUpdateCall = 0x161;
	using Update3D_t = void (*)(void*, float, void*, void*, char, char, std::uint8_t);
	Update3D_t           g_update3DOrig = nullptr;
	std::atomic_uint32_t g_update3DCalls{ 0 };

	void Update3D_Hook(void* a_actor, float a_dt, void* a_root, void* a_camera, char a_first, char a_flag, std::uint8_t a_p7)
	{
		if (a_first) {
			++g_update3DCalls;
		}
		g_update3DOrig(a_actor, a_dt, a_root, a_camera, a_first, a_flag, a_p7);
	}

	// task side: find the bones, publish them and the blend weight
	void ArmTick(float a_weight)
	{
		{
			std::lock_guard lk(g_armLock);
			if (!FindArm()) {
				return;
			}
		}
		for (int b = 0; b < kBones; ++b) {
			g_hookBones[b] = g_arm.bones[b].node;
		}
		g_armWeight = a_weight;
		FindPoseComponent();
		// Two skeleton states were seen. With a pose component above the clavicle (HumanExportRoot, 473 bones) the
		// component computes the bones and PoseUpdate_Hook does the work. Without one, the NiNode transforms are
		// consistent (world = localRot * parentRot, zero error) and the method of the in-game tuning session works:
		// offsets into the locals from the task, arm subtree worlds recomputed.
		if (!g_poseComp.load()) {
			for (int b = 0; b < kBones; ++b) {
				ApplyBoneOffset(b);
			}
			auto* top = g_arm.bones[0].node;
			UpdateSubtreeWorld(top, 1, *reinterpret_cast<void* const*>(top));
		}

		const auto now = std::chrono::steady_clock::now();
		std::lock_guard lk(g_armLock);
		if (now >= g_arm.nextLog) {
			g_arm.nextLog = now + std::chrono::seconds(5);
			REX::INFO("arm: weight {:.2f}, 1st-person 3D updates {}, pose component updates {}, offset applied {} times, animation writes {}",
				a_weight, g_update3DCalls.exchange(0), g_poseUpdates.exchange(0), g_arm.ticks, g_arm.rewritten);
			g_arm.rewritten = g_arm.ticks = 0;
		}
	}

	// weight 0: the hook takes the offsets out on the next update; then stop following the bones
	void ArmRelease()
	{
		g_armWeight = 0.0F;
	}

	void ArmForget()
	{
		for (auto& b : g_hookBones) {
			b = nullptr;
		}
		std::lock_guard lk(g_armLock);
		g_arm = {};
		g_pose = {};
		g_arr = {};
		g_poseComp = 0;
		g_poseSearched = false;
	}

	// numpad on a bone: rotate about the bone's own axes (pairs: 8/2 4/6 7/9 and 1/3 / * - +)
	void BoneStep(int a_key, int a_bone, bool a_coarse)
	{
		const float ang = (a_coarse ? 5.0F : 1.0F) * std::numbers::pi_v<float> / 180.0F;
		int         axis = 0;
		float       sign = 1;
		switch (a_key) {
		case 0x68: axis = 0, sign = 1; break;   // 8
		case 0x62: axis = 0, sign = -1; break;  // 2
		case 0x64: axis = 1, sign = 1; break;   // 4
		case 0x66: axis = 1, sign = -1; break;  // 6
		case 0x67: axis = 2, sign = 1; break;   // 7
		case 0x69: axis = 2, sign = -1; break;  // 9
		case 0x61: axis = 0, sign = 1; break;   // 1
		case 0x63: axis = 0, sign = -1; break;  // 3
		case 0x6F: axis = 1, sign = 1; break;   // /
		case 0x6A: axis = 1, sign = -1; break;  // *
		case 0x6D: axis = 2, sign = 1; break;   // -
		case 0x6B: axis = 2, sign = -1; break;  // +
		default: return;
		}
		Vec ax{ 0, 0, 0 };
		ax[axis] = 1;
		g_boneRot[a_bone] = Mul(g_boneRot[a_bone], AxisAngle(ax, sign * ang));
	}

	constexpr std::array<int, 16> kKeys{ 0x60, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6A, 0x6B, 0x6D, 0x6E, 0x6F, 0 };
}

namespace
{
	bool g_loggedHideChain = false;
}

namespace Tuner
{
	// Setting the cull bit (NiAVObject flags +0x118) on the 1st-person [Root] did not hide anything in game: the renderer keeps
	// its own instance tables. Use the engine's Papyrus native Game.ShowFirstPersonGeometry(bool) instead
	// (FUN_142009D70: sets player +0x112A bit 4 = hide 1st-person geometry); its VM arguments are unused.
	bool SetFirstPersonHidden(bool a_hide)
	{
		static REL::Relocation<void (*)(void*, std::uint32_t, void*, bool)> showFirstPersonGeometry{ REL::ID(117377) };
		static bool hidden = false;
		if (a_hide == hidden) {
			return true;
		}
		showFirstPersonGeometry(nullptr, 0, nullptr, !a_hide);
		hidden = a_hide;
		if (!g_loggedHideChain) {
			g_loggedHideChain = true;
			REX::INFO("photo: ShowFirstPersonGeometry({}) called", !a_hide);
		}
		return true;
	}

	void Install()
	{
		const auto site = kAttachCaller.address() + kAttachCallOffset;
		const auto op = *reinterpret_cast<const std::uint8_t*>(site);
		if (op != 0xE8) {
			REX::ERROR("tuner: expected a call at ID 37953 +0x{:X}, found 0x{:02X}; live tuning disabled", kAttachCallOffset, op);
			return;
		}
		g_attachOrig = reinterpret_cast<Attach_t>(REL::GetTrampoline().write_call<5>(site, Attach_Hook));

		const auto site3D = kPlayerUpdate3D.address() + kFirstPersonUpdateCall;
		if (*reinterpret_cast<const std::uint8_t*>(site3D) == 0xE8) {
			g_update3DOrig = reinterpret_cast<Update3D_t>(REL::GetTrampoline().write_call<5>(site3D, Update3D_Hook));
			REX::INFO("arm: hooked the player's 1st-person 3D update at Starfield.exe+0x{:X}", kPlayerUpdate3D.offset() + kFirstPersonUpdateCall);
		} else {
			REX::ERROR("arm: expected a call at ID 102725 +0x{:X}; arm pose disabled", kFirstPersonUpdateCall);
		}
		REX::INFO("tuner: hooked AnimObject attach at Starfield.exe+0x{:X}", kAttachCaller.offset() + kAttachCallOffset);
	}

	void Load()
	{
		if (LoadXf(kSavePath, g_d)) {
			REX::INFO("tuner: loaded saved offset, trans [{:.4f} {:.4f} {:.4f}]", g_d.trans[0], g_d.trans[1], g_d.trans[2]);
		} else {
			REX::INFO("tuner: no saved offset ({}), starting from the NIF placement", kSavePath);
		}
	}

	void Tick(bool a_isOpen, bool a_keys, bool a_camera)
	{
		const auto now = std::chrono::steady_clock::now();
		// weight of the camera pose: eases in / out over kBlendTime instead of jumping
		constexpr float kBlendTime = 0.35F;
		static float    weight = 0.0F;
		static auto     last = now;
		const float     dt = std::min(0.1F, std::chrono::duration<float>(now - last).count());
		last = now;
		if (!a_isOpen) {
			if (g_root) {
				Release();
			}
			g_applied = Mode::kNone;
			weight = 0.0F;
			ArmForget();
			g_target = 0;
			return;
		}
		weight = a_camera ? std::min(1.0F, weight + dt / kBlendTime) : std::max(0.0F, weight - dt / kBlendTime);
		const Mode mode = (a_camera || weight > 0.0F) ? Mode::kCamera : (a_keys ? Mode::kNormal : Mode::kNone);
		if (mode == Mode::kNone && g_applied == Mode::kNone) {
			return;  // baked placement, nothing to do
		}
		// the object is re-created on every open; drop it once it is detached
		if (g_root && !g_root->parent) {
			Release();
		}
		Adopt();
		if (!g_root) {
			return;
		}
		if (mode == Mode::kCamera && !g_camReady) {
			g_camReady = true;
			if (!g_armLoaded) {
				g_armLoaded = true;
				LoadArm();
			}
			if (LoadXf(kCameraSavePath, g_cam)) {
				REX::INFO("tuner: camera pose loaded from {}", kCameraSavePath);
			} else {
				// the pose tuned in game (repo defaults/PhoneTest_camera_offset.txt): the phone stays in the hand, the
				// arm pose moves both
				g_cam.rot = { { { 1.00002F, -0.000455752F, 0.00496641F }, { 0.000477783F, 1.00003F, -0.00484394F },
					{ -0.00497664F, 0.00484368F, 1.00001F } } };
				g_cam.trans = { -0.0168549F, -0.00363235F, 0.000672147F };
				REX::INFO("tuner: no camera pose file, using the built-in one (tune with the numpad on the Camera page, 5 saves)");
			}
		}
		// the numpad tunes the camera pose only while the Camera page is shown (fully blended in)
		Xf* active = a_camera ? &g_cam : a_keys ? &g_d : nullptr;

		// keys (dev_keys only): act on press, then repeat every 60 ms while held.
		// Camera page: '.' cycles the target (phone, clavicle, upper arm, forearm, wrist), 0 resets it, Ctrl = coarse
		// steps, 5 saves the phone pose and the arm pose. Elsewhere: the old tuner ('.' toggles fine / coarse).
		if (a_keys && active) {
			const bool coarse = (GetAsyncKeyState(0x11 /* VK_CONTROL */) & 0x8000) != 0;
			bool       any = false;
			for (std::size_t i = 0; i + 1 < kKeys.size(); ++i) {
				const int  vk = kKeys[i];
				const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
				const bool pressed = down && !g_wasDown[i];
				g_wasDown[i] = down;
				if (!down) continue;
				any = true;
				if (a_camera) {
					if (vk == 0x6E) {  // . next target
						if (pressed) {
							g_target = (g_target + 1) % (kBones + 1);
							const char* label = g_target == 0 ? Tr("手机") : Tr(kBoneLabels[g_target - 1]);
							Ui::SetCameraStatus(std::string(Tr("小键盘调整：")) + label);
							REX::INFO("tuner: target {}", g_target);
						}
					} else if (vk == 0x60) {  // 0 reset the target
						if (pressed) {
							if (g_target == 0) g_cam = Xf{};
							else g_boneRot[g_target - 1] = kIdentity;
							Ui::SetCameraStatus(Tr("已重置"));
						}
					} else if (vk == 0x65) {  // 5 save both
						if (pressed) {
							Save(g_cam, kCameraSavePath);
							SaveArm();
							Ui::SetCameraStatus(Tr("姿势已保存"));
						}
					} else if (pressed || now >= g_nextRepeat) {
						if (g_target == 0) {
							g_fine = !coarse;
							Step(vk, g_cam);
						} else {
							BoneStep(vk, g_target - 1, coarse);
						}
					}
					continue;
				}
				if (vk == 0x60) {  // 0 reset
					if (pressed) { *active = Xf{}; REX::INFO("tuner: reset"); }
				} else if (vk == 0x65) {  // 5 save
					if (pressed) Save(*active, kSavePath);
				} else if (vk == 0x6E) {  // . fine / coarse
					if (pressed) { g_fine = !g_fine; REX::INFO("tuner: {} steps", g_fine ? "fine" : "coarse"); }
				} else if (pressed || now >= g_nextRepeat) {
					Step(vk, *active);
				}
			}
			if (any && now >= g_nextRepeat) {
				g_nextRepeat = now + std::chrono::milliseconds(60);
			}
		}

		const Xf    base = a_keys ? g_d : Xf{};
		const float s = weight * weight * (3.0F - 2.0F * weight);  // smoothstep
		const Xf    use = s >= 1.0F ? g_cam : s <= 0.0F ? base : Blend(base, g_cam, s);
		for (std::size_t i = 0; i < g_children.size(); ++i) {
			Apply(use, g_orig[i], g_children[i]->local);
		}
		g_applied = mode;
		if (weight > 0.0F) {
			ArmTick(s);
		} else {
			ArmRelease();
		}
	}
}
