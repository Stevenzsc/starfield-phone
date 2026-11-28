// Player / ship / location data, Starfield.exe 1.16.244 (Ghidra; Papyrus natives found by their registration
// strings, see tools/re/ghidra/NativeFind.java). Every native below ignores its VM and stack-ID arguments.
//   ObjectReference.GetValue      ID 118329  float (vm, stack, ref, AVIF)  -> ActorValueOwner (ref+0x70) slot 1
//   ObjectReference.GetBaseValue  ID 118296  float (vm, stack, ref, AVIF)  -> slot 3
//   ObjectReference.GetCurrentPlanet ID 118491  PNDT* (vm, stack, ref)     (form type 0xBA)
//   Actor.GetLevel                ID 116823  u16 (vm, stack, actor)         (base NPC +0xE8)
//   Actor.GetGoldAmount           ID 116821  int (vm, stack, actor)         (credits)
//   ObjectReference.GetCurrentLocation -> TESObjectREFR::GetCurrentLocation ID 63412  BGSLocation* (ref)
//   Game.GetPlayerHomeSpaceShip   -> FUN_141A6BD50 ID 102957 (player, ref** out): out holds a reference,
//                                    released with FUN_140384600 ID 38742
//   TESObjectREFR::GetSpaceship   ID 119881  ref* (ref, true): the ship the ref is in (no reference added)
//   XP needed for a level         FUN_140D83030 ID 69497  float (level)   (Game.GetXPForLevel)
//   GetFullName                   ID 47496   const char* (form)            ("" when the form has no name)

#include "pch.h"
#include "stats.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace
{
	constexpr REL::ID kGetValue{ 118329 };
	constexpr REL::ID kGetBaseValue{ 118296 };
	constexpr REL::ID kGetCurrentPlanet{ 118491 };
	constexpr REL::ID kGetLevel{ 116823 };
	constexpr REL::ID kGetGoldAmount{ 116821 };
	constexpr REL::ID kGetCurrentLocation{ 63412 };
	constexpr REL::ID kGetHomeShip{ 102957 };
	constexpr REL::ID kReleaseRef{ 38742 };
	constexpr REL::ID kGetSpaceship{ 119881 };
	constexpr REL::ID kXPForLevel{ 69497 };
	constexpr REL::ID kGetFullName{ 47496 };

	// Starfield.esm AVIF form IDs
	constexpr std::uint32_t kAvHealth = 0x000002D4;
	constexpr std::uint32_t kAvOxygen = 0x000002D5;
	constexpr std::uint32_t kAvCarryWeight = 0x000002DC;
	constexpr std::uint32_t kAvExperience = 0x000002C9;
	constexpr std::uint32_t kAvShieldHealth = 0x0024A05F;
	constexpr std::uint32_t kAvShieldMaxHealth = 0x0005BFA8;
	constexpr std::uint32_t kAvGravJumpFuel = 0x0000854F;
	constexpr std::uint32_t kAvGravJumpMaxDistance = 0x00008551;
	constexpr std::uint32_t kAvMaxPower = 0x00001018;
	constexpr std::uint32_t kAvCrew = 0x00040CE0;
	constexpr std::uint32_t kAvPassengerSlots = 0x0001E7DE;

	void* Player() { return RE::PlayerCharacter::GetSingleton(); }

	float Value(void* a_ref, std::uint32_t a_av, bool a_base = false)
	{
		auto* av = RE::TESForm::LookupByID(a_av);
		if (!a_ref || !av) {
			return NAN;
		}
		using fn_t = float (*)(void*, std::uint32_t, void*, RE::TESForm*);
		return reinterpret_cast<fn_t>((a_base ? kGetBaseValue : kGetValue).address())(nullptr, 0, a_ref, av);
	}

	std::string Name(const void* a_form)
	{
		if (!a_form) {
			return {};
		}
		using fn_t = const char* (*)(const void*);
		const char* s = reinterpret_cast<fn_t>(kGetFullName.address())(a_form);
		return s ? s : "";
	}

	// a reference's display name, else its base object's (+0x98)
	std::string RefName(const void* a_ref)
	{
		auto n = Name(a_ref);
		if (n.empty() && a_ref) {
			n = Name(*reinterpret_cast<void* const*>(reinterpret_cast<std::uintptr_t>(a_ref) + 0x98));
		}
		return n;
	}

	std::string PlanetName(void* a_ref)
	{
		using fn_t = void* (*)(void*, std::uint32_t, void*);
		return a_ref ? Name(reinterpret_cast<fn_t>(kGetCurrentPlanet.address())(nullptr, 0, a_ref)) : std::string{};
	}

	std::string LocationName(void* a_ref)
	{
		using fn_t = void* (*)(void*);
		return a_ref ? Name(reinterpret_cast<fn_t>(kGetCurrentLocation.address())(a_ref)) : std::string{};
	}

	std::string Fmt(const char* a_fmt, double a_v)
	{
		char buf[64];
		std::snprintf(buf, sizeof(buf), a_fmt, a_v);
		return buf;
	}

	std::string Num(float a_v) { return std::isfinite(a_v) ? Fmt("%.0f", std::round(a_v)) : "—"; }

	// "cur / max" with a bar
	Ui::InfoRow Gauge(const char* a_label, float a_cur, float a_max)
	{
		Ui::InfoRow r{ a_label, Num(a_cur) + " / " + Num(a_max) };
		if (std::isfinite(a_cur) && std::isfinite(a_max) && a_max > 0) {
			r.fraction = std::clamp(a_cur / a_max, 0.0f, 1.0f);
		}
		return r;
	}

	Ui::InfoRow Header(const char* a_label) { return { a_label, "", -1.0f, true }; }

	// holds the home ship for one call and releases it
	struct HomeShip
	{
		void* ref = nullptr;
		HomeShip()
		{
			if (auto* p = Player()) {
				using fn_t = void** (*)(void*, void**);
				reinterpret_cast<fn_t>(kGetHomeShip.address())(p, &ref);
			}
		}
		~HomeShip()
		{
			if (ref) {
				using fn_t = void (*)(void*);
				reinterpret_cast<fn_t>(kReleaseRef.address())(ref);
			}
		}
		HomeShip(const HomeShip&) = delete;
		HomeShip& operator=(const HomeShip&) = delete;
	};

	std::string Place(void* a_ref)
	{
		const auto planet = PlanetName(a_ref);
		const auto loc = LocationName(a_ref);
		if (planet.empty()) return loc.empty() ? "—" : loc;
		if (loc.empty() || loc == planet) return planet;
		return loc + "，" + planet;
	}
}

namespace Stats
{
	int PlayerLevel()
	{
		auto* p = Player();
		// the native reads the base NPC (+0x98) without a check; it can be missing before a save is loaded
		if (!p || !*reinterpret_cast<void* const*>(reinterpret_cast<std::uintptr_t>(p) + 0x98)) {
			return 0;
		}
		using fn_t = std::uint16_t (*)(void*, std::uint32_t, void*);
		return reinterpret_cast<fn_t>(kGetLevel.address())(nullptr, 0, p);
	}

	std::vector<Ui::InfoRow> Character()
	{
		std::vector<Ui::InfoRow> rows;
		auto*                    p = Player();
		if (!p) {
			return rows;
		}
		const int level = PlayerLevel();
		using gold_t = int (*)(void*, std::uint32_t, void*);
		const int credits = reinterpret_cast<gold_t>(kGetGoldAmount.address())(nullptr, 0, p);
		using xp_t = float (*)(std::uint32_t);
		const auto  xpFor = reinterpret_cast<xp_t>(kXPForLevel.address());
		const float xp = Value(p, kAvExperience);
		const float xpThis = xpFor(std::uint32_t(level));
		const float xpNext = xpFor(std::uint32_t(level + 1));

		const auto name = RefName(p);
		rows.push_back({ "姓名", name.empty() ? "—" : name });
		rows.push_back({ "等级", std::to_string(level) });
		// Experience may be the running total (>= the level's threshold) or the progress inside the level
		Ui::InfoRow xpRow{ "经验" };
		if (std::isfinite(xp) && xpNext > xpThis) {
			const float into = xp >= xpThis ? xp - xpThis : xp;
			xpRow.value = Num(into) + " / " + Num(xpNext - xpThis);
			xpRow.fraction = std::clamp(into / (xpNext - xpThis), 0.0f, 1.0f);
		} else {
			xpRow.value = Num(xp);
		}
		rows.push_back(xpRow);
		rows.push_back({ "信用点", std::to_string(credits) });
		rows.push_back(Header("状态"));
		rows.push_back(Gauge("生命", Value(p, kAvHealth), Value(p, kAvHealth, true)));
		rows.push_back(Gauge("氧气", Value(p, kAvOxygen), Value(p, kAvOxygen, true)));
		rows.push_back({ "负重上限", Num(Value(p, kAvCarryWeight)) + " kg" });

		static bool logged = false;
		if (!logged) {
			logged = true;
			REX::INFO("stats: player '{}' level {} xp {} (level {} at {}, {} at {}) credits {} health {}/{} o2 {}/{} carry {}",
				name, level, xp, level, xpThis, level + 1, xpNext, credits, Value(p, kAvHealth), Value(p, kAvHealth, true),
				Value(p, kAvOxygen), Value(p, kAvOxygen, true), Value(p, kAvCarryWeight));
		}
		return rows;
	}

	std::vector<Ui::InfoRow> Ship()
	{
		std::vector<Ui::InfoRow> rows;
		HomeShip                 ship;
		if (!ship.ref) {
			return rows;
		}
		void*      s = ship.ref;
		const auto name = RefName(s);
		rows.push_back({ "名称", name.empty() ? "—" : name });
		rows.push_back({ "位置", Place(s) });
		rows.push_back(Header("状态"));
		rows.push_back(Gauge("船体", Value(s, kAvHealth), Value(s, kAvHealth, true)));
		const float shieldMax = Value(s, kAvShieldMaxHealth);
		rows.push_back(Gauge("护盾", Value(s, kAvShieldHealth), shieldMax));
		rows.push_back(Gauge("跃迁燃料", Value(s, kAvGravJumpFuel), Value(s, kAvGravJumpFuel, true)));
		rows.push_back(Header("性能"));
		rows.push_back({ "跃迁距离", Num(Value(s, kAvGravJumpMaxDistance)) + " 光年" });
		rows.push_back({ "货舱容量", Num(Value(s, kAvCarryWeight)) + " kg" });
		rows.push_back({ "反应堆", Num(Value(s, kAvMaxPower)) });
		rows.push_back({ "船员", Num(Value(s, kAvCrew)) });
		rows.push_back({ "乘客舱位", Num(Value(s, kAvPassengerSlots)) });

		static bool logged = false;
		if (!logged) {
			logged = true;
			REX::INFO("stats: ship {:08X} '{}' hull {}/{} shield {}/{} fuel {}/{} jump {} cargo {} power {} crew {}",
				reinterpret_cast<RE::TESForm*>(s)->GetFormID(), name, Value(s, kAvHealth), Value(s, kAvHealth, true),
				Value(s, kAvShieldHealth), shieldMax, Value(s, kAvGravJumpFuel), Value(s, kAvGravJumpFuel, true),
				Value(s, kAvGravJumpMaxDistance), Value(s, kAvCarryWeight), Value(s, kAvMaxPower), Value(s, kAvCrew));
		}
		return rows;
	}

	std::vector<Ui::InfoRow> Location()
	{
		std::vector<Ui::InfoRow> rows;
		auto*                    p = Player();
		if (!p) {
			return rows;
		}
		const auto planet = PlanetName(p);
		const auto loc = LocationName(p);
		using ship_t = void* (*)(void*, bool);
		void* aboard = reinterpret_cast<ship_t>(kGetSpaceship.address())(p, true);
		rows.push_back(Header("当前位置"));
		rows.push_back({ "星球", planet.empty() ? "太空" : planet });
		rows.push_back({ "地点", loc.empty() ? "—" : loc });
		rows.push_back({ "状态", aboard ? "在飞船上" : (planet.empty() ? "太空中" : "地面") });
		rows.push_back(Header("家园飞船"));
		{
			HomeShip ship;
			if (ship.ref) {
				const auto name = RefName(ship.ref);
				rows.push_back({ "飞船", name.empty() ? "—" : name });
				rows.push_back({ "停靠", Place(ship.ref) });
			} else {
				rows.push_back({ "飞船", "—" });
			}
		}
		static bool logged = false;
		if (!logged) {
			logged = true;
			REX::INFO("stats: player planet '{}' location '{}' aboard {}", planet, loc, aboard);
		}
		return rows;
	}
}
