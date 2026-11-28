// Second render view, step 1: read-only probe of CreationRenderer's view / output tables in game.
// Every read goes through SafeRead (SEH), so a wrong offset logs "bad" instead of crashing.
// Layouts and addresses: docs/second-view-research.md.

#include "pch.h"
#include "viewprobe.h"
#include "screen.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <format>
#include <new>
#include <string>

namespace
{
	constexpr REL::ID kWorldHolder{ 936470 };  // DAT_145FB08E8: world holder (views +0x3D0/+0x3D4, outputs +0x3D8/+0x3DC)
	constexpr REL::ID kViewTable{ 837401 };    // DAT_145975168
	constexpr REL::ID kOutputTable{ 837405 };  // DAT_145975188

	bool SafeRead(std::uintptr_t a_addr, void* a_out, std::size_t a_size)
	{
		if (a_addr < 0x10000) {
			return false;
		}
		__try {
			std::memcpy(a_out, reinterpret_cast<const void*>(a_addr), a_size);
			return true;
		} __except (1) {
			return false;
		}
	}

	template <class T>
	bool Rd(std::uintptr_t a_addr, T& a_out)
	{
		return SafeRead(a_addr, &a_out, sizeof(T));
	}

	std::uintptr_t Ptr(std::uintptr_t a_addr)
	{
		std::uintptr_t v = 0;
		return Rd(a_addr, v) ? v : 0;
	}

	std::uint32_t U32(std::uintptr_t a_addr)
	{
		std::uint32_t v = 0xDEADBEEF;
		Rd(a_addr, v);
		return v;
	}

	// row of a table column for an entity index: mapping array at +a_map (u32 per entity), rows at +0x3C8
	bool Row(std::uintptr_t a_col, std::uint32_t a_index, std::uint32_t a_map, std::size_t a_stride, std::uintptr_t& a_row)
	{
		const auto map = Ptr(a_col + a_map);
		const auto rows = Ptr(a_col + 0x3C8);
		std::uint32_t r = 0;
		if (!map || !rows || !Rd(map + std::uintptr_t(a_index) * 4, r)) {
			return false;
		}
		a_row = rows + std::uintptr_t(r) * a_stride;
		return true;
	}

	std::string Hex(std::uintptr_t a_addr, std::size_t a_n)
	{
		std::string s;
		char        b[4];
		for (std::size_t i = 0; i < a_n; ++i) {
			std::uint8_t v = 0;
			if (!Rd(a_addr + i, v)) {
				return s + " (bad)";
			}
			std::snprintf(b, sizeof(b), "%02X", v);
			s += b;
			if (i % 4 == 3) s += ' ';
		}
		return s;
	}

	void LogViewList(std::uintptr_t a_outTable, std::uint32_t a_out, const char* a_label)
	{
		const auto col = Ptr(a_outTable + 0x120);
		const auto idx = a_out & 0xFFFFFF;
		for (std::uint32_t map : { 0x40u, 0x2C8u }) {
			std::uintptr_t row = 0;
			if (!col || !Row(col, idx, map, 0x20, row)) {
				REX::INFO("probe: {} view list (map +{:X}): unreadable", a_label, map);
				continue;
			}
			const auto count = U32(row);
			const auto cap = U32(row + 8);
			std::uintptr_t data = row + 0x10;
			if (static_cast<std::int32_t>(cap) >= 0) {
				data = Ptr(row + 0x10);
			}
			std::string views;
			for (std::uint32_t i = 0; i < count && i < 8; ++i) {
				views += std::format(" {:08X}", U32(data + i * 4));
			}
			REX::INFO("probe: {} view list (map +{:X}): count {} cap {:08X} [{}] raw {}", a_label, map, count, cap, views,
				Hex(row, 0x20));
		}
	}
}

namespace ViewProbe
{
	void Run()
	{
		const auto holder = Ptr(kWorldHolder.address());
		const auto views = Ptr(kViewTable.address());
		const auto outs = Ptr(kOutputTable.address());
		REX::INFO("probe: holder {:X} viewTable {:X} outputTable {:X}", holder, views, outs);
		if (!holder || !views || !outs) {
			return;
		}
		const auto v0 = U32(holder + 0x3D0), v1 = U32(holder + 0x3D4);
		const auto o0 = U32(holder + 0x3D8), o1 = U32(holder + 0x3DC);
		std::uint8_t active = 0xFF, valid = 0xFF;
		Rd(holder + 0x4E0, active);
		Rd(holder + 0x4E1, valid);
		REX::INFO("probe: views {:08X} {:08X} outputs {:08X} {:08X} env +0x64 {:08X} active {} valid {}", v0, v1, o0, o1,
			U32(holder + 0x64), active, valid);

		const auto scene = Ptr(holder + 0x70), cam = Ptr(holder + 0x80);
		const auto scene2 = Ptr(holder + 0x90), cam2 = Ptr(holder + 0xA8);
		REX::INFO("probe: scene {:X} handle {:08X} camera {:X} handle {:08X} | scene2 {:X} handle {:08X} camera2 {:X} handle {:08X}",
			scene, scene ? U32(scene + 0x150) : 0, cam, cam ? U32(cam + 0x130) : 0, scene2, scene2 ? U32(scene2 + 0x150) : 0,
			cam2, cam2 ? U32(cam2 + 0x130) : 0);

		// live views: generation bytes at viewTable+0x10, scene/camera column +0x120 (0x1C per row)
		const auto gens = Ptr(views + 0x10);
		const auto bind = Ptr(views + 0x120);
		for (std::uint32_t i = 0; i < 64; ++i) {
			std::uint8_t g = 0;
			if (!gens || !Rd(gens + i, g) || g == 0) {
				continue;
			}
			std::uintptr_t row = 0;
			const bool ok = bind && Row(bind, i, 0x2C8, 0x1C, row);
			REX::INFO("probe: view #{} gen {} bind {}", i, g, ok ? Hex(row, 0x1C) : std::string("unreadable"));
		}
		LogViewList(outs, o0, "output0");
		LogViewList(outs, o1, "output1");

		// output columns (settings +0xE8 = 0x30 bytes, target +0xF0, ? +0xF8/+0x100, flag +0x108, +0x110 = 0x38,
		// type/name +0x128 = 0x10): raw rows, to find the resolution fields
		struct Col
		{
			std::uint32_t off;
			std::size_t   stride;
		};
		static constexpr Col cols[] = { { 0xE8, 0x30 }, { 0xF0, 4 }, { 0xF8, 0x20 }, { 0x100, 0x20 }, { 0x108, 1 }, { 0x110, 0x38 },
			{ 0x128, 0x10 } };
		for (const auto o : { o0, o1 }) {
			for (const auto& c : cols) {
				const auto     col = Ptr(outs + c.off);
				std::uintptr_t row = 0;
				const bool     ok = col && Row(col, o & 0xFFFFFF, 0x2C8, c.stride, row);
				REX::INFO("probe: output {:08X} col +{:X}: {}", o, c.off, ok ? Hex(row, c.stride) : std::string("unreadable"));
			}
		}
		const auto desc0 = holder + 0x3E0;
		REX::INFO("probe: output0 desc {}", Hex(desc0, 0x80));
	}
}

// ---- step 2: our own camera + view in the world scene, rendered through the idle second main output ----
namespace
{
	constexpr REL::ID kCreateCameraView{ 130594 };  // FUN_142550860(obj, &sceneObj, envHandle, fov, exposure)
	constexpr REL::ID kSetViewList{ 140277 };       // FUN_142914b90(column, index, BSTSmallArray<u32>*)
	constexpr REL::ID kSetViewBinding{ 54152 };     // FUN_140841310(&view, binding[0x1C])

	using CreateCameraView_t = void (*)(void* a_obj, void* a_sceneObjSlot, std::uint32_t a_env, float a_fov, float a_exposure);
	using SetViewList_t = void (*)(std::uintptr_t a_column, std::uint32_t a_index, void* a_array);
	using SetViewBinding_t = void (*)(std::uint32_t* a_view, void* a_binding);
	using AttachChild_t = void (*)(void* a_node, void* a_child, bool a_firstAvailable);

	// The object FUN_142550860 fills: +0x00 BSFixedString name, +0x18 NiCamera*, +0x28 view handle, +0x2C env handle
	alignas(16) std::uint8_t g_obj[0x40]{};
	bool                     g_created = false;
	bool                     g_failed = false;
	bool                     g_enabled = false;
	std::uint32_t            g_view = 0xFFFFFF;

	// engine calls under SEH: a fault is logged and the feature is switched off instead of crashing the game
	bool CallCreate(void* a_fn, void* a_obj, void* a_slot, std::uint32_t a_env, float a_fov, float a_exp)
	{
		__try {
			reinterpret_cast<CreateCameraView_t>(a_fn)(a_obj, a_slot, a_env, a_fov, a_exp);
			return true;
		} __except (1) {
			return false;
		}
	}

	bool CallAttach(void* a_fn, void* a_node, void* a_child)
	{
		__try {
			reinterpret_cast<AttachChild_t>(a_fn)(a_node, a_child, true);
			return true;
		} __except (1) {
			return false;
		}
	}

	bool CallSetList(void* a_fn, std::uintptr_t a_col, std::uint32_t a_index, void* a_array)
	{
		__try {
			reinterpret_cast<SetViewList_t>(a_fn)(a_col, a_index, a_array);
			return true;
		} __except (1) {
			return false;
		}
	}

	bool CallSetBinding(void* a_fn, std::uint32_t* a_view, void* a_binding)
	{
		__try {
			reinterpret_cast<SetViewBinding_t>(a_fn)(a_view, a_binding);
			return true;
		} __except (1) {
			return false;
		}
	}

	// PhoneTest.ini viewfinder_near=<float> (0 = keep the engine's near plane)
	float IniNear()
	{
		std::FILE* f = nullptr;
		if (fopen_s(&f, "Data\\SFSE\\Plugins\\PhoneTest.ini", "r") != 0 || !f) {
			return 0.0f;
		}
		char  line[256];
		float v = 0.0f;
		while (std::fgets(line, sizeof(line), f)) {
			if (std::strncmp(line, "viewfinder_near=", 16) == 0) {
				v = static_cast<float>(std::atof(line + 16));
			}
		}
		std::fclose(f);
		return v;
	}

	bool Create()
	{
		if (g_created || g_failed) {
			return g_created;
		}
		g_failed = true;  // cleared on success
		const auto holder = Ptr(kWorldHolder.address());
		if (!holder) {
			REX::WARN("view2: no world holder");
			return false;
		}
		const std::uint32_t env = U32(holder + 0x64);
		std::memset(g_obj, 0, sizeof(g_obj));
		new (g_obj) RE::BSFixedString("PhoneCameraView");
		*reinterpret_cast<std::uint32_t*>(g_obj + 0x28) = 0xFFFFFF;
		*reinterpret_cast<std::uint32_t*>(g_obj + 0x2C) = 0xFFFFFF;
		REX::INFO("view2: creating camera + view (scene slot {:X}, env {:08X})", holder + 0x70, env);
		if (!CallCreate(reinterpret_cast<void*>(kCreateCameraView.address()), g_obj, reinterpret_cast<void*>(holder + 0x70), env,
				70.0f, 1.0f)) {
			REX::ERROR("view2: FUN_142550860 faulted");
			return false;
		}
		const auto cam = *reinterpret_cast<std::uintptr_t*>(g_obj + 0x18);
		g_view = *reinterpret_cast<std::uint32_t*>(g_obj + 0x28);
		REX::INFO("view2: camera {:X} handle {:08X} local translate {} view {:08X} env {:08X}", cam, cam ? U32(cam + 0x130) : 0,
			cam ? Hex(cam + 0x70, 12) : std::string("-"), g_view, U32(reinterpret_cast<std::uintptr_t>(g_obj) + 0x2C));
		if (!cam || (g_view & 0xFFFFFF) >= 64) {
			return false;
		}

		// follow the eye: hang the camera under "World Camera Root", like the world camera itself
		const auto root = Ptr(holder + 0x78);
		const auto vtbl = root ? Ptr(root) : 0;
		const auto attach = vtbl ? Ptr(vtbl + 0x2A0) : 0;
		if (!attach || !CallAttach(reinterpret_cast<void*>(attach), reinterpret_cast<void*>(root), reinterpret_cast<void*>(cam))) {
			REX::ERROR("view2: attaching the camera to the world camera root failed");
			return false;
		}

		// binding row: { scene, camera, flags, fov, near, far, u32 } -> optional near plane from the ini
		const auto views = Ptr(kViewTable.address());
		const auto bind = views ? Ptr(views + 0x120) : 0;
		std::uintptr_t row = 0;
		if (bind && Row(bind, g_view & 0xFFFFFF, 0x2C8, 0x1C, row)) {
			std::uint8_t b[0x1C]{};
			if (SafeRead(row, b, sizeof(b))) {
				REX::INFO("view2: binding {}", Hex(row, 0x1C));
			}
		}
		g_created = true;
		g_failed = false;
		return true;
	}

	// BSTSmallArray<u32> as the engine lays it out: count, pad, capacity | 0x80000000 (inline), pad, inline data
	struct ViewList
	{
		std::uint32_t count = 0;
		std::uint32_t pad0 = 0;
		std::uint32_t cap = 0x80000004;
		std::uint32_t pad1 = 0;
		std::uint32_t data[4]{};
	};

	// ---- step 3: own output + render texture, queued for rendering every frame ----
	constexpr REL::ID kCreateTexture{ 140022 };    // FUN_1428FC910(&BSFixedString name, desc[0x48], &texture) -> 0 / 6 ok
	constexpr REL::ID kBindTexture{ 140237 };      // FUN_142912210(desc, slot hash, &texture)
	constexpr REL::ID kQueueOutput{ 54145 };       // FUN_140840B40(desc): render this output descriptor
	constexpr REL::ID kCreateOutput{ 52406 };      // FUN_1407CA380(&handle)
	constexpr REL::ID kSetOutputType{ 37829 };     // FUN_140342CE0(&output, { u8 type, BSFixedString name })
	constexpr REL::ID kSetOutputSettings{ 54153 }; // FUN_140841410(&output, settings[0x30]) (tone-map parameters)
	constexpr REL::ID kSetOutputFlag{ 54154 };     // FUN_140841500(&output, &u8)
	constexpr REL::ID kSetFrustum{ 147392 };       // FUN_142BE2F60(camera, float[4] left right top bottom)
	constexpr std::uint32_t kMainImageSlot = 0x09681900;  // final image slot of the main output (descriptor +0x10)

	using Fn4 = std::uintptr_t (*)(std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t);

	bool CallRaw(std::uintptr_t a_fn, std::uintptr_t a_a, std::uintptr_t a_b, std::uintptr_t a_c, std::uintptr_t a_d,
		std::uintptr_t* a_ret)
	{
		const auto fn = reinterpret_cast<Fn4>(a_fn);
		__try {
			const auto r = fn(a_a, a_b, a_c, a_d);
			if (a_ret) {
				*a_ret = r;
			}
			return true;
		} __except (1) {
			return false;
		}
	}

	bool Call(REL::ID a_id, std::uintptr_t a_a, std::uintptr_t a_b = 0, std::uintptr_t a_c = 0, std::uintptr_t a_d = 0,
		std::uintptr_t* a_ret = nullptr)
	{
		return CallRaw(a_id.address(), a_a, a_b, a_c, a_d, a_ret);
	}

	template <class T>
	std::uintptr_t A(T* a_p)
	{
		return reinterpret_cast<std::uintptr_t>(a_p);
	}

	enum class Stage
	{
		kIdle,
		kTexturePending,
		kReady,
		kFailed,
	};
	Stage                    g_stage = Stage::kIdle;
	std::uintptr_t           g_texture = 0;        // engine texture object (refcount at +0x14)
	std::uint32_t            g_output = 0xFFFFFF;
	alignas(16) std::uint8_t g_desc[0x80]{};       // output descriptor
	std::uint32_t            g_queued = 0;
	std::chrono::steady_clock::time_point g_nextQueue{};

	bool TextureReady(std::uintptr_t a_tex)
	{
		const auto s = U32(a_tex + 0x14) & 0x70000000;
		return s == 0x30000000 || s == 0x40000000;  // same test as the starmap scanner
	}

	bool StartTexture()
	{
		struct Name
		{
			RE::BSFixedString s{ "PhoneCameraTarget" };
		} name;
		std::uint32_t d[0x12]{};  // 0x48 bytes, as the starmap scanner fills it
		d[0] = 0x80;
		d[1] = ViewProbe::kTargetWidth;
		d[2] = ViewProbe::kTargetHeight;
		d[3] = 1;
		d[4] = 2;
		d[5] = 1;
		d[6] = 0x2401;
		d[11] = 0x101006;
		d[12] = 0x05040302;
		d[14] = 1;
		std::uintptr_t r = 0;
		Screen::ArmViewTarget();
		if (!Call(kCreateTexture, A(&name.s), A(d), A(&g_texture), 0, &r)) {
			REX::ERROR("view2: texture creation faulted");
			return false;
		}
		REX::INFO("view2: texture {:X} created ({}x{}) -> {} state {:08X}", g_texture, ViewProbe::kTargetWidth,
			ViewProbe::kTargetHeight, static_cast<int>(r), g_texture ? U32(g_texture + 0x14) : 0);
		return g_texture != 0;
	}

	// Descriptor: inline binding array (4 x 0x18), +0x70 target object (none), +0x78 output. Queueing MOVES it
	// (FUN_14083CCC0 empties the bindings and swaps in output 0xFFFFFF), so it is rebuilt before every queue;
	// re-queueing the emptied one crashed the render graph.
	bool FillDesc()
	{
		std::memset(g_desc, 0, sizeof(g_desc));
		*reinterpret_cast<std::uint32_t*>(g_desc + 8) = 0x80000004;
		*reinterpret_cast<std::uint32_t*>(g_desc + 0x78) = g_output;
		if (!Call(kBindTexture, A(g_desc), kMainImageSlot, A(&g_texture))) {  // adds a texture reference
			REX::ERROR("view2: binding the texture faulted");
			return false;
		}
		return true;
	}

	// Our camera hangs under "World Camera Root" like the world camera, but the view direction lives in the world
	// camera's own transform (identity under the root rendered the world sideways and static): copy its local
	// (+0x40) and world (+0x80) NiTransform (0x40 bytes each) every frame.
	void FollowMainCamera()
	{
		const auto holder = Ptr(kWorldHolder.address());
		const auto mainCam = holder ? Ptr(holder + 0x80) : 0;
		const auto ourCam = *reinterpret_cast<std::uintptr_t*>(g_obj + 0x18);
		if (!mainCam || !ourCam) {
			return;
		}
		std::uint8_t xf[0x80];
		if (SafeRead(mainCam + 0x40, xf, sizeof(xf))) {
			std::memcpy(reinterpret_cast<void*>(ourCam + 0x40), xf, sizeof(xf));
		}
		static bool logged = false;
		if (!logged) {
			logged = true;
			REX::INFO("view2: main camera local {} world {}", Hex(mainCam + 0x40, 0x40), Hex(mainCam + 0x80, 0x40));
		}
	}

	// Near plane of our view: geometry closer than this (the hand and the phone) is not drawn. Default 0.6; the ini
	// key viewfinder_near overrides it and is re-read every second, so it can be tuned while the game runs.
	constexpr float kDefaultNear = 0.6f;
	float           g_nearApplied = -1.0f;
	std::chrono::steady_clock::time_point g_nextNearCheck{};

	void ApplyNear()
	{
		const auto now = std::chrono::steady_clock::now();
		if (now < g_nextNearCheck) {
			return;
		}
		g_nextNearCheck = now + std::chrono::seconds(1);
		const float ini = IniNear();
		const float want = ini > 0.0f ? ini : kDefaultNear;
		if (want == g_nearApplied) {
			return;
		}
		const auto     views = Ptr(kViewTable.address());
		const auto     bind = views ? Ptr(views + 0x120) : 0;
		std::uintptr_t row = 0;
		std::uint8_t   b[0x1C]{};
		if (!bind || !Row(bind, g_view & 0xFFFFFF, 0x2C8, 0x1C, row) || !SafeRead(row, b, sizeof(b))) {
			return;
		}
		std::memcpy(b + 0x10, &want, 4);  // { scene, camera, flags, fov, near, far, u32 }
		if (CallSetBinding(reinterpret_cast<void*>(kSetViewBinding.address()), &g_view, b)) {
			g_nearApplied = want;
			REX::INFO("view2: near plane {} (binding {})", want, Hex(row, 0x1C));
		}
	}

	// Feature flags of a view: 0x210 bytes in view column +0x108 (FUN_14083FC30 copies that much); the first u64 is
	// the feature bits. FUN_142550860 fills ours from the global settings with bits 22, 34, 38 and 43 cleared: that
	// view drew no characters or ships; copying all four from the main view made the output black. The ini key
	// viewfinder_bits (hex) picks which of the main view's bits to copy; it is re-read every second.
	constexpr REL::ID       kSetViewFlags{ 128330 };  // FUN_14248D200(&view, flags[0x210])
	constexpr std::size_t   kFlagsSize = 0x210;
	constexpr std::uint64_t kCandidateBits = (1ull << 22) | (1ull << 34) | (1ull << 38) | (1ull << 43);
	constexpr std::uint64_t kDefaultBits = (1ull << 34) | (1ull << 38) | (1ull << 43);
	alignas(16) std::uint8_t g_baseFlags[kFlagsSize]{};  // ours as the helper made them
	bool                     g_haveBase = false;
	std::uint64_t            g_bitsApplied = ~0ull;

	std::uint64_t IniBits()
	{
		std::FILE* f = nullptr;
		if (fopen_s(&f, "Data\\SFSE\\Plugins\\PhoneTest.ini", "r") != 0 || !f) {
			return kDefaultBits;
		}
		char          line[256];
		std::uint64_t v = kDefaultBits;
		while (std::fgets(line, sizeof(line), f)) {
			if (std::strncmp(line, "viewfinder_bits=", 16) == 0) {
				v = std::strtoull(line + 16, nullptr, 16);
			}
		}
		std::fclose(f);
		return v & kCandidateBits;
	}

	void ApplyViewBits()
	{
		const auto holder = Ptr(kWorldHolder.address());
		const auto views = Ptr(kViewTable.address());
		const auto col = views ? Ptr(views + 0x108) : 0;
		if (!holder || !col) {
			return;
		}
		std::uintptr_t row = 0;
		if (!g_haveBase) {
			if (!Row(col, g_view & 0xFFFFFF, 0x2C8, kFlagsSize, row) || !SafeRead(row, g_baseFlags, kFlagsSize)) {
				return;
			}
			g_haveBase = true;
		}
		const std::uint64_t bits = IniBits();
		if (bits == g_bitsApplied) {
			return;
		}
		std::uint64_t mainBits = 0;
		if (!Row(col, U32(holder + 0x3D0) & 0xFFFFFF, 0x2C8, kFlagsSize, row) || !Rd(row, mainBits)) {
			return;
		}
		alignas(16) std::uint8_t flags[kFlagsSize];
		std::memcpy(flags, g_baseFlags, kFlagsSize);
		std::uint64_t ours = 0;
		std::memcpy(&ours, flags, 8);
		ours = (ours & ~bits) | (mainBits & bits);
		std::memcpy(flags, &ours, 8);
		if (!Call(kSetViewFlags, A(&g_view), A(flags))) {
			REX::ERROR("view2: setting the view flags faulted");
			return;
		}
		g_bitsApplied = bits;
		REX::INFO("view2: view bits from main {:X} -> ours {:016X} (main {:016X})", bits, ours, mainBits);
	}

	bool BuildOutput()
	{
		const auto holder = Ptr(kWorldHolder.address());
		const auto outs = Ptr(kOutputTable.address());
		if (!holder || !outs) {
			return false;
		}
		if (!Call(kCreateOutput, A(&g_output)) || (g_output & 0xFFFFFF) == 0xFFFFFF) {
			REX::ERROR("view2: output creation failed");
			return false;
		}
		struct TypeName
		{
			std::uint8_t      type = 0x55;  // main world pipeline
			std::uint8_t      pad[7]{};
			RE::BSFixedString name{ "PhoneCamera" };
		} tn;
		std::uint8_t settings[0x30]{};
		std::uintptr_t setRow = 0;
		const auto     setCol = Ptr(outs + 0xE8);
		const std::uint32_t main = U32(holder + 0x3D8);
		const bool haveSettings = setCol && Row(setCol, main & 0xFFFFFF, 0x2C8, 0x30, setRow) && SafeRead(setRow, settings, 0x30);
		std::uint8_t flag = 0;
		ViewList     list;
		list.count = 1;
		list.data[0] = g_view;
		const auto listCol = Ptr(outs + 0x120);
		const bool ok = Call(kSetOutputType, A(&g_output), A(&tn)) && (!haveSettings || Call(kSetOutputSettings, A(&g_output), A(settings))) &&
		                Call(kSetOutputFlag, A(&g_output), A(&flag)) && listCol &&
		                CallSetList(reinterpret_cast<void*>(kSetViewList.address()), listCol, g_output & 0xFFFFFF, &list);
		if (!ok) {
			REX::ERROR("view2: output setup faulted");
			return false;
		}
		if (!FillDesc()) {
			return false;
		}
		REX::INFO("view2: output {:08X} ready (settings {}), desc {}", g_output, haveSettings, Hex(A(g_desc), 0x80));
		LogViewList(outs, g_output, "phone output");
		return true;
	}
}

namespace ViewProbe
{
	void SetSecondView(bool) {}

	void Tick(bool a_on)
	{
		if (!a_on || g_stage == Stage::kFailed) {
			return;
		}
		switch (g_stage) {
		case Stage::kIdle:
			if (!Create() || !StartTexture()) {
				g_stage = Stage::kFailed;
				return;
			}
			{
				// frustum matching the target's aspect: { left, right, top, bottom }, 60 degrees vertical
				const auto  cam = *reinterpret_cast<std::uintptr_t*>(g_obj + 0x18);
				const float t = 0.57735f;  // tan(30 deg)
				const float x = t * float(ViewProbe::kTargetWidth) / float(ViewProbe::kTargetHeight);
				float       f[4] = { -x, x, t, -t };
				if (!Call(kSetFrustum, cam, A(f))) {
					REX::WARN("view2: setting the frustum faulted");
				}
			}
			g_stage = Stage::kTexturePending;
			return;
		case Stage::kTexturePending:
			if (!TextureReady(g_texture)) {
				return;
			}
			REX::INFO("view2: texture ready, state {:08X}", U32(g_texture + 0x14));
			if (!BuildOutput()) {
				g_stage = Stage::kFailed;
				return;
			}
			g_stage = Stage::kReady;
			// the render thread sees new table rows a frame or more later: queueing the output in the frame it was
			// created crashed in the render graph (FUN_142AA3790 indexing the per-output array)
			g_nextQueue = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
			return;
		case Stage::kReady: {
			FollowMainCamera();
			ApplyNear();
			{
				static auto next = std::chrono::steady_clock::now();
				if (std::chrono::steady_clock::now() >= next) {
					next = std::chrono::steady_clock::now() + std::chrono::seconds(1);
					ApplyViewBits();
				}
			}
			const auto now = std::chrono::steady_clock::now();
			if (now < g_nextQueue) {
				return;
			}
			g_nextQueue = now + std::chrono::milliseconds(33);  // ~30 renders per second
			if (g_queued > 0 && !FillDesc()) {
				g_stage = Stage::kFailed;
				return;
			}
			if (!Call(kQueueOutput, A(g_desc))) {
				REX::ERROR("view2: queueing the output faulted");
				g_stage = Stage::kFailed;
				return;
			}
			if (++g_queued == 1 || g_queued % 300 == 0) {
				REX::INFO("view2: output queued {} times", g_queued);
			}
			return;
		}
		default:
			return;
		}
	}
}
