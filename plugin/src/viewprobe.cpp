// Second render view, step 1: read-only probe of CreationRenderer's view / output tables in game.
// Every read goes through SafeRead (SEH), so a wrong offset logs "bad" instead of crashing.
// Layouts and addresses: docs/second-view-research.md.

#include "pch.h"
#include "viewprobe.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
				const float nearPlane = IniNear();
				if (nearPlane > 0.0f) {
					std::memcpy(b + 0x10, &nearPlane, 4);
					CallSetBinding(reinterpret_cast<void*>(kSetViewBinding.address()), &g_view, b);
					REX::INFO("view2: near plane set to {} -> binding {}", nearPlane, Hex(row, 0x1C));
				}
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
}

namespace ViewProbe
{
	void SetSecondView(bool a_on)
	{
		if (a_on == g_enabled) {
			return;
		}
		if (a_on && !Create()) {
			return;
		}
		const auto holder = Ptr(kWorldHolder.address());
		const auto outs = Ptr(kOutputTable.address());
		const auto col = outs ? Ptr(outs + 0x120) : 0;
		if (!holder || !col) {
			return;
		}
		const std::uint32_t out = U32(holder + 0x3DC);  // second main output (type 0x5A), idle in normal play
		ViewList list;
		if (a_on) {
			list.count = 1;
			list.data[0] = g_view;
		}
		if (!CallSetList(reinterpret_cast<void*>(kSetViewList.address()), col, out & 0xFFFFFF, &list)) {
			REX::ERROR("view2: setting the view list of output {:08X} faulted", out);
			g_failed = true;
			return;
		}
		g_enabled = a_on;
		REX::INFO("view2: view {:08X} {} output {:08X}", g_view, a_on ? "added to" : "removed from", out);
		LogViewList(outs, out, "output1");
	}
}
