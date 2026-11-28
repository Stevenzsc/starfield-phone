// Second render view, step 1: read-only probe of CreationRenderer's view / output tables in game.
// Every read goes through SafeRead (SEH), so a wrong offset logs "bad" instead of crashing.
// Layouts and addresses: docs/second-view-research.md.

#include "pch.h"
#include "viewprobe.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <format>
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
	}
}
