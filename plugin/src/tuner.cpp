// Live placement tuner. While the hand-held object is open, numpad keys move and turn it relative to the camera:
//   4 / 6  left / right        8 / 2  up / down          7 / 9  closer / farther
//   1 / 3  roll (about the view direction)   / and *  yaw (about up)   - and +  pitch (about right)
//   .      toggle fine / coarse steps (2 mm, 1 deg / 1 cm, 5 deg)
//   0      reset to the NIF's own placement           5      save + log the offset
// The offset D is kept in the object's root space and applied to every geometry child: child.local = D * original.
// That is the same "child' = V * child" the asset generator uses, so a saved offset can be baked into the NIF.
// (The engine overwrites the AnimObject ROOT transform when it attaches it, so the root itself can't be moved.)

#include "pch.h"
#include "tuner.h"

#include <array>
#include <chrono>
#include <cmath>
#include <fstream>
#include <numbers>
#include <vector>

extern "C" __declspec(dllimport) short __stdcall GetAsyncKeyState(int a_vkey);

namespace
{
	constexpr const char* kRootName = "PhoneAnimObjectRoot";  // tools/make_test_assets.py ROOT_NAME
	constexpr const char* kSavePath = "Data\\SFSE\\Plugins\\PhoneTest_offset.txt";

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

	Xf                                  g_d;
	bool                                g_fine = true;
	RE::NiPointer<RE::NiAVObject>       g_root;
	std::vector<RE::NiAVObject*>        g_children;  // owned by g_root
	std::vector<RE::NiTransform>        g_orig;
	std::chrono::steady_clock::time_point g_nextSearch{};
	std::chrono::steady_clock::time_point g_nextRepeat{};
	std::array<bool, 16>                g_wasDown{};

	void Save()
	{
		std::ofstream out(kSavePath, std::ios::trunc);
		out.imbue(std::locale::classic());
		out << "rot";
		for (auto& row : g_d.rot)
			for (float v : row)
				out << ' ' << v;
		out << "\ntrans " << g_d.trans[0] << ' ' << g_d.trans[1] << ' ' << g_d.trans[2] << '\n';
		REX::INFO("tuner: saved offset to {}: rot [{:.4f} {:.4f} {:.4f} / {:.4f} {:.4f} {:.4f} / {:.4f} {:.4f} {:.4f}] trans [{:.4f} {:.4f} {:.4f}]",
			kSavePath, g_d.rot[0][0], g_d.rot[0][1], g_d.rot[0][2], g_d.rot[1][0], g_d.rot[1][1], g_d.rot[1][2],
			g_d.rot[2][0], g_d.rot[2][1], g_d.rot[2][2], g_d.trans[0], g_d.trans[1], g_d.trans[2]);
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

	RE::NiAVObject* SceneTop()
	{
		RE::NiAVObject* top = CameraRoot();
		for (int depth = 0; top && top->parent && depth < 64; ++depth) {
			top = top->parent;
		}
		return top;
	}

	void Find()
	{
		const RE::BSFixedString name{ kRootName };
		RE::NiAVObject*         found = nullptr;
		const char*             via = "";
		if (auto* top = SceneTop()) {
			found = top->GetObjectByName(name);
			via = "scene top";
		}
		if (!found) {
			if (auto* root = CameraRoot()) {
				found = root->GetObjectByName(name);
				via = "camera root";
			}
		}
		if (!found) {
			return;
		}
		auto* node = reinterpret_cast<RE::NiNode*>(found);
		g_root = RE::NiPointer<RE::NiAVObject>(found);
		g_children.clear();
		g_orig.clear();
		for (std::uint32_t i = 0; i < node->children.size(); ++i) {
			if (auto* c = node->children[i].get()) {
				g_children.push_back(c);
				g_orig.push_back(c->local);
			}
		}
		REX::INFO("tuner: found '{}' via {} with {} children", kRootName, via, g_children.size());
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

	void Step(int a_key)
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

	constexpr std::array<int, 16> kKeys{ 0x60, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6A, 0x6B, 0x6D, 0x6E, 0x6F, 0 };
}

namespace Tuner
{
	void Load()
	{
		std::ifstream in(kSavePath);
		if (!in) {
			REX::INFO("tuner: no saved offset ({}), starting from the NIF placement", kSavePath);
			return;
		}
		in.imbue(std::locale::classic());
		std::string tag;
		Xf d;
		if ((in >> tag) && tag == "rot") {
			for (auto& row : d.rot)
				for (float& v : row) in >> v;
		}
		if ((in >> tag) && tag == "trans") {
			in >> d.trans[0] >> d.trans[1] >> d.trans[2];
		}
		if (in) {
			g_d = d;
			REX::INFO("tuner: loaded saved offset, trans [{:.4f} {:.4f} {:.4f}]", d.trans[0], d.trans[1], d.trans[2]);
		} else {
			REX::WARN("tuner: could not parse {}", kSavePath);
		}
	}

	void Tick(bool a_isOpen)
	{
		const auto now = std::chrono::steady_clock::now();
		if (!a_isOpen) {
			if (g_root) {
				Release();
			}
			return;
		}
		// the object is re-created on every open; drop it once it is detached
		if (g_root && !g_root->parent) {
			Release();
		}
		if (!g_root && now >= g_nextSearch) {
			g_nextSearch = now + std::chrono::milliseconds(250);
			Find();
		}

		// keys: act on press, then repeat every 60 ms while held
		bool any = false;
		for (std::size_t i = 0; i + 1 < kKeys.size(); ++i) {
			const int  vk = kKeys[i];
			const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
			const bool pressed = down && !g_wasDown[i];
			g_wasDown[i] = down;
			if (!down) continue;
			any = true;
			if (vk == 0x60) {  // 0 reset
				if (pressed) { g_d = Xf{}; REX::INFO("tuner: reset"); }
			} else if (vk == 0x65) {  // 5 save
				if (pressed) Save();
			} else if (vk == 0x6E) {  // . fine / coarse
				if (pressed) { g_fine = !g_fine; REX::INFO("tuner: {} steps", g_fine ? "fine" : "coarse"); }
			} else if (pressed || now >= g_nextRepeat) {
				Step(vk);
			}
		}
		if (any && now >= g_nextRepeat) {
			g_nextRepeat = now + std::chrono::milliseconds(60);
		}

		if (g_root) {
			for (std::size_t i = 0; i < g_children.size(); ++i) {
				Apply(g_d, g_orig[i], g_children[i]->local);
			}
		}
	}
}
