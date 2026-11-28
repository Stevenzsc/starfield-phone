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
//   SpaceshipReference.GetGravJumpRange ID 115013  float (vm, stack, ref**): computed from the ship parts (the
//                                    SpaceshipGravJumpMaxDistance AV stays 0), in light years
//   XP needed for a level         FUN_140D83030 ID 69497  float (level)   (Game.GetXPForLevel)
//   GetFullName                   ID 47496   const char* (form)            ("" when the form has no name)

#include "pch.h"
#include "lang.h"
#include "stats.h"

#include <algorithm>
#include <cmath>
#include <chrono>
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
	constexpr REL::ID kGravJumpRange{ 115013 };
	constexpr REL::ID kGetFullName{ 47496 };

	// Starfield.esm AVIF form IDs
	constexpr std::uint32_t kAvHealth = 0x000002D4;
	constexpr std::uint32_t kAvOxygen = 0x000002D5;
	constexpr std::uint32_t kAvCarryWeight = 0x000002DC;
	constexpr std::uint32_t kAvExperience = 0x000002C9;
	constexpr std::uint32_t kAvShieldHealth = 0x0024A05F;
	constexpr std::uint32_t kAvShieldMaxHealth = 0x0005BFA8;
	constexpr std::uint32_t kAvGravJumpFuel = 0x0000854F;
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

	// the native takes the ship as a ref** (SpaceshipReference self argument)
	float JumpRange(void* a_ship)
	{
		if (!a_ship) {
			return NAN;
		}
		using fn_t = float (*)(void*, std::uint32_t, void**);
		return reinterpret_cast<fn_t>(kGravJumpRange.address())(nullptr, 0, &a_ship);
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
		return loc + Tr("，") + planet;
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
		rows.push_back({ Tr("姓名"), name.empty() ? "—" : name });
		rows.push_back({ Tr("等级"), std::to_string(level) });
		// Experience may be the running total (>= the level's threshold) or the progress inside the level
		Ui::InfoRow xpRow{ Tr("经验") };
		if (std::isfinite(xp) && xpNext > xpThis) {
			const float into = xp >= xpThis ? xp - xpThis : xp;
			xpRow.value = Num(into) + " / " + Num(xpNext - xpThis);
			xpRow.fraction = std::clamp(into / (xpNext - xpThis), 0.0f, 1.0f);
		} else {
			xpRow.value = Num(xp);
		}
		rows.push_back(xpRow);
		rows.push_back({ Tr("信用点"), std::to_string(credits) });
		rows.push_back(Header(Tr("状态")));
		rows.push_back(Gauge(Tr("生命"), Value(p, kAvHealth), Value(p, kAvHealth, true)));
		rows.push_back(Gauge(Tr("氧气"), Value(p, kAvOxygen), Value(p, kAvOxygen, true)));
		rows.push_back({ Tr("负重上限"), Num(Value(p, kAvCarryWeight)) + " kg" });

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
		rows.push_back({ Tr("名称"), name.empty() ? "—" : name });
		rows.push_back({ Tr("位置"), Place(s) });
		rows.push_back(Header(Tr("状态")));
		rows.push_back(Gauge(Tr("船体"), Value(s, kAvHealth), Value(s, kAvHealth, true)));
		const float shieldMax = Value(s, kAvShieldMaxHealth);
		rows.push_back(Gauge(Tr("护盾"), Value(s, kAvShieldHealth), shieldMax));
		rows.push_back(Gauge(Tr("跃迁燃料"), Value(s, kAvGravJumpFuel), Value(s, kAvGravJumpFuel, true)));
		rows.push_back(Header(Tr("性能")));
		rows.push_back({ Tr("跃迁距离"), Num(JumpRange(s)) + Tr(" 光年") });
		rows.push_back({ Tr("货舱容量"), Num(Value(s, kAvCarryWeight)) + " kg" });
		rows.push_back({ Tr("反应堆"), Num(Value(s, kAvMaxPower)) });
		rows.push_back({ Tr("船员"), Num(Value(s, kAvCrew)) });
		rows.push_back({ Tr("乘客舱位"), Num(Value(s, kAvPassengerSlots)) });

		static bool logged = false;
		if (!logged) {
			logged = true;
			REX::INFO("stats: ship {:08X} '{}' hull {}/{} shield {}/{} fuel {}/{} jump {} cargo {} power {} crew {}",
				reinterpret_cast<RE::TESForm*>(s)->GetFormID(), name, Value(s, kAvHealth), Value(s, kAvHealth, true),
				Value(s, kAvShieldHealth), shieldMax, Value(s, kAvGravJumpFuel), Value(s, kAvGravJumpFuel, true),
				JumpRange(s), Value(s, kAvCarryWeight), Value(s, kAvMaxPower), Value(s, kAvCrew));
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
		const auto geo = CurrentGeo();
		rows.push_back(Header(Tr("当前位置")));
		rows.push_back({ Tr("星系"), geo.system.empty() ? "—" : geo.system });
		rows.push_back({ Tr("星球"), planet.empty() ? Tr("太空") : planet });
		rows.push_back({ Tr("地点"), loc.empty() ? "—" : loc });
		rows.push_back({ Tr("状态"), aboard ? Tr("在飞船上") : (planet.empty() ? Tr("太空中") : Tr("地面")) });
		rows.push_back({ "GPS", geo.gps ? Gallery::GpsText(geo) : Tr("无信号") });
		rows.push_back(Header(Tr("家园飞船")));
		{
			HomeShip ship;
			if (ship.ref) {
				const auto name = RefName(ship.ref);
				rows.push_back({ Tr("飞船"), name.empty() ? "—" : name });
				rows.push_back({ Tr("停靠"), Place(ship.ref) });
			} else {
				rows.push_back({ Tr("飞船"), "—" });
			}
		}
		static bool logged = false;
		if (!logged) {
			logged = true;
			REX::INFO("stats: player planet '{}' location '{}' system '{}' aboard {} gps {} {} {}", planet, loc, geo.system, aboard,
				geo.gps, geo.lat, geo.lon);
		}
		return rows;
	}
}

// ---- location / GPS, crew, scanner, outposts ----
//   BGSPlanet::Manager            ID 945580 (static object): +0x80 planet form ID, +0x98 lon, +0xA0 lat (radians,
//                                 landing-zone centre), +0x100 radius km; local positions are metres from that centre,
//                                 +X east, +Y north (outpost-backup project, verified in game)
//   BGSLocation                   +0xC0 parent location (Papyrus Location.GetParentLocations, FUN_141FE9F40)
//   TESObjectREFR                 +0x88 heading (radians, 0 = north, clockwise), +0x8C position, +0xB0 parent cell,
//                                 +0xC8 extra data list
//   TESObjectCELL                 +0x48 bit 0 interior (Papyrus Cell.IsInterior, ID 118114; CommonLibSF says +0x40)
//   ProcessLists                  ID 937584 (pointer): +0x60 u32 count, +0x68 u32* high-process actor handles
//   handle -> reference           ID 35638 (ref** out, const u32* handle), reference released with ID 38742
//   FindExtra                     ID 37878 (extra list, type); type 0xE2 on a ship = its crew: +0x18 u32 count,
//                                 +0x20 u32* actor handles (ObjectReference.GetAllCrewMembers, FUN_14206F3B0)
//   Actor.IsDead                  ID 116726 bool (vm, stack, actor)
//   Actor.IsHostileToActor        ID 116842 bool (vm, stack, actor, target)
//   Debug.ExecuteConsole          ID 117306 void (vm, stack, unused, BSFixedString*) -> runs a console line
//   Debug.Notification            ID 117311 void (vm, stack, unused, BSFixedString*)
//   Outpost beacons: references whose base is OutpostBeaconActivator (ACTI 002C60E9)

namespace
{
	constexpr REL::ID kPlanetManager{ 945580 };
	constexpr REL::ID kProcessLists{ 937584 };
	constexpr REL::ID kLookupHandle{ 35638 };
	constexpr REL::ID kFindExtra{ 37878 };
	constexpr REL::ID kIsDead{ 116726 };
	constexpr REL::ID kIsHostile{ 116842 };
	constexpr REL::ID kExecuteConsole{ 117306 };
	constexpr REL::ID kNotification{ 117311 };
	constexpr REL::ID kPlayMenuSound{ 114814 };   // WwiseEvent.PlayMenuSound(string) global, Starfield.exe+0x1F0AB40
	constexpr REL::ID kShakeController{ 117449 }; // Game.ShakeController(float, float, float), Starfield.exe+0x200FCA0
	constexpr REL::ID kAllForms{ 883341 };
	// Outpost beacon data (ExtraData type 0xBC; MatchOutpostName FUN_142077350 writes the name at +0x70, a
	// BSFixedString). FUN_140F51350(ref, create): with create = false a plain lookup, nullptr when not a beacon or none.
	constexpr REL::ID kOutpostData{ 73983 };

	constexpr std::uint32_t kBeaconBase = 0x002C60E9;
	constexpr std::uint8_t  kTypeREFR = 0x4A;
	constexpr std::uint8_t  kTypeACHR = 0x4B;

	template <class T>
	T At(const void* a_base, std::ptrdiff_t a_off)
	{
		return *reinterpret_cast<const T*>(reinterpret_cast<std::uintptr_t>(a_base) + a_off);
	}

	struct Pos
	{
		float x, y, z;
	};
	Pos PosOf(const void* a_ref) { return { At<float>(a_ref, 0x8C), At<float>(a_ref, 0x90), At<float>(a_ref, 0x94) }; }

	std::uint32_t FormID(const void* a_form) { return a_form ? reinterpret_cast<const RE::TESForm*>(a_form)->GetFormID() : 0; }

	bool Interior(const void* a_ref)
	{
		const void* cell = At<const void*>(a_ref, 0xB0);
		return cell && (At<std::uint8_t>(cell, 0x48) & 1);
	}

	void* PlanetOf(void* a_ref)
	{
		using fn_t = void* (*)(void*, std::uint32_t, void*);
		return a_ref ? reinterpret_cast<fn_t>(kGetCurrentPlanet.address())(nullptr, 0, a_ref) : nullptr;
	}

	// holds a reference resolved from a handle and releases it
	struct RefPtr
	{
		void* ref = nullptr;
		explicit RefPtr(std::uint32_t a_handle)
		{
			using fn_t = void** (*)(void**, const std::uint32_t*);
			reinterpret_cast<fn_t>(kLookupHandle.address())(&ref, &a_handle);
		}
		~RefPtr()
		{
			if (ref) {
				using fn_t = void (*)(void*);
				reinterpret_cast<fn_t>(kReleaseRef.address())(ref);
			}
		}
		RefPtr(const RefPtr&) = delete;
		RefPtr& operator=(const RefPtr&) = delete;
	};

	// high-process (loaded, near) actor handles
	std::vector<std::uint32_t> HighActors()
	{
		std::vector<std::uint32_t> out;
		const auto* pl = *reinterpret_cast<const std::uint8_t* const*>(kProcessLists.address());
		if (!pl) return out;
		const auto  n = At<std::uint32_t>(pl, 0x60);
		const auto* h = At<const std::uint32_t*>(pl, 0x68);
		for (std::uint32_t i = 0; h && i < n && i < 4096; ++i) out.push_back(h[i]);
		return out;
	}

	// crew handles of a ship (extra data 0xE2)
	std::vector<std::uint32_t> CrewHandles(void* a_ship)
	{
		std::vector<std::uint32_t> out;
		void* list = a_ship ? At<void*>(a_ship, 0xC8) : nullptr;
		if (!list) return out;
		using fn_t = void* (*)(void*, std::uint8_t);
		void* extra = reinterpret_cast<fn_t>(kFindExtra.address())(list, 0xE2);
		if (!extra) return out;
		const auto  n = At<std::uint32_t>(extra, 0x18);
		const auto* h = At<const std::uint32_t*>(extra, 0x20);
		for (std::uint32_t i = 0; h && i < n && i < 256; ++i) out.push_back(h[i]);
		return out;
	}

	bool IsDead(void* a_actor)
	{
		using fn_t = bool (*)(void*, std::uint32_t, void*);
		return reinterpret_cast<fn_t>(kIsDead.address())(nullptr, 0, a_actor);
	}

	// Actor.IsPlayerTeammate (native ID 116853): actor +0x208 bit 26 (companions following the player)
	bool IsTeammate(const void* a_actor)
	{
		return At<std::uint8_t>(a_actor, 0x2E) == kTypeACHR && ((At<std::uint32_t>(a_actor, 0x208) >> 26) & 1);
	}

	bool IsHostile(void* a_actor, void* a_target)
	{
		using fn_t = bool (*)(void*, std::uint32_t, void*, void*);
		return reinterpret_cast<fn_t>(kIsHostile.address())(nullptr, 0, a_actor, a_target);
	}

	void RunNative(const REL::ID& a_id, const std::string& a_text)
	{
		RE::BSFixedString text{ a_text.c_str() };
		using fn_t = void (*)(void*, std::uint32_t, void*, const RE::BSFixedString*);
		reinterpret_cast<fn_t>(a_id.address())(nullptr, 0, nullptr, &text);
	}

	float Distance(const Pos& a, const Pos& b)
	{
		const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
		return std::sqrt(dx * dx + dy * dy + dz * dz);
	}

	std::string Meters(float a_m)
	{
		char buf[32];
		if (a_m >= 1000.0f) std::snprintf(buf, sizeof(buf), "%.1f km", a_m / 1000.0f);
		else std::snprintf(buf, sizeof(buf), "%.0f m", a_m);
		return buf;
	}

	// location names from the ref's location up to the root
	std::vector<std::string> LocationChain(void* a_ref)
	{
		std::vector<std::string> out;
		using fn_t = void* (*)(void*);
		void* loc = a_ref ? reinterpret_cast<fn_t>(kGetCurrentLocation.address())(a_ref) : nullptr;
		for (int i = 0; loc && i < 12; ++i, loc = At<void*>(loc, 0xC0)) {
			out.push_back(Name(loc));
		}
		return out;
	}

	// outpost beacon references (form IDs), from the all-forms map
	std::vector<std::uint32_t> ScanBeacons()
	{
		std::vector<std::uint32_t> out;
		auto*                      base = RE::TESForm::LookupByID(kBeaconBase);
		auto*                      map = *reinterpret_cast<std::uint8_t**>(kAllForms.address());
		if (!base || !map) return out;
		auto* lock = reinterpret_cast<RE::BSReadWriteLock*>(map + 0x30);
		lock->LockRead();
		const auto* entries = *reinterpret_cast<const std::uint8_t* const*>(map + 0x10);
		const auto  capacity = *reinterpret_cast<const std::uint64_t*>(map + 0x18);
		for (std::uint64_t i = 0; entries && i < capacity && i < (1ull << 26); ++i) {
			const auto* e = entries + i * 0x18;
			const auto* form = *reinterpret_cast<const RE::TESForm* const*>(e + 8);
			if (!form || *reinterpret_cast<const std::uint32_t*>(e) != form->GetFormID()) continue;
			if (At<std::uint8_t>(form, 0x2E) == kTypeREFR && At<const void*>(form, 0x98) == base) {
				out.push_back(form->GetFormID());
			}
		}
		lock->UnlockRead();
		return out;
	}
}

namespace Stats
{
	// a_pos: position for lat / lon (the photo mode camera), else the player's
	Gallery::Geo GeoWith(const Pos* a_pos)
	{
		Gallery::Geo g;
		auto*        p = Player();
		if (!p || !At<void*>(p, 0xB0)) return g;  // no parent cell: no save loaded yet
		void* planet = PlanetOf(p);
		g.planet = Name(planet);
		const auto chain = LocationChain(p);
		if (!chain.empty() && chain[0] != g.planet) g.place = chain[0];
		// the star system: the first parent above the planet's location(s) with another name (the chain can hold the
		// planet name twice, e.g. 着陆区域 -> 霍金I -> 霍金I -> 霍金)
		bool seenPlanet = false;
		for (const auto& n : chain) {
			if (!g.planet.empty() && n == g.planet) {
				seenPlanet = true;
			} else if (seenPlanet && !n.empty()) {
				g.system = n;
				break;
			}
		}
		const auto* pm = reinterpret_cast<const std::uint8_t*>(kPlanetManager.address());
		const float radiusKm = At<float>(pm, 0x100);
		if (planet && At<std::uint32_t>(pm, 0x80) == FormID(planet) && radiusKm > 1.0f && !Interior(p)) {
			constexpr double toDeg = 180.0 / 3.14159265358979323846;
			const double     lon0 = At<double>(pm, 0x98) * toDeg, lat0 = At<double>(pm, 0xA0) * toDeg;
			const double     R = double(radiusKm) * 1000.0;
			const auto       pos = a_pos ? *a_pos : PosOf(p);
			g.lat = std::clamp(lat0 + pos.y / R * toDeg, -90.0, 90.0);
			g.lon = lon0 + pos.x / (R * std::max(0.01, std::cos(lat0 / toDeg))) * toDeg;
			while (g.lon > 180) g.lon -= 360;
			while (g.lon < -180) g.lon += 360;
			g.gps = std::isfinite(g.lat) && std::isfinite(g.lon);
		}
		if (auto* cal = RE::Calendar::GetSingleton()) {
			const float    h = cal->GetHour();
			const unsigned y = cal->GetYear() < 1000 ? cal->GetYear() + 2000 : cal->GetYear();
			char           buf[64];
			std::snprintf(buf, sizeof(buf), " %02d:%02d", int(h) % 24, int((h - std::floor(h)) * 60.0f) % 60);
			g.date = Lang::Date(y, unsigned(cal->GetMonth()), unsigned(cal->GetDay())) + buf;
		}
		return g;
	}

	Gallery::Geo CurrentGeo() { return GeoWith(nullptr); }

	std::vector<Ui::Contact> Contacts()
	{
		std::vector<Ui::Contact> out;
		auto*                    p = Player();
		if (!p) return out;
		HomeShip   ship;
		const auto me = PosOf(p);
		const auto high = HighActors();
		void*      myPlanet = PlanetOf(p);
		// ship crew (extra data 0xE2) plus companions following the player (loaded teammates)
		auto handles = CrewHandles(ship.ref);
		for (const auto h : high) {
			RefPtr a(h);
			if (a.ref && a.ref != p && IsTeammate(a.ref) && std::find(handles.begin(), handles.end(), h) == handles.end()) {
				handles.push_back(h);
			}
		}
		for (const auto h : handles) {
			RefPtr a(h);
			if (!a.ref || a.ref == p) continue;
			Ui::Contact c;
			c.formID = FormID(a.ref);
			c.name = RefName(a.ref);
			if (c.name.empty()) c.name = Tr("船员");
			c.dead = IsDead(a.ref);
			const bool loaded = std::find(high.begin(), high.end(), h) != high.end();
			if (loaded && PlanetOf(a.ref) == myPlanet) {
				c.distance = Distance(PosOf(a.ref), me);
				c.where = Tr("附近 ") + Meters(c.distance);
			} else {
				c.where = Place(a.ref);
			}
			out.push_back(std::move(c));
		}
		std::stable_sort(out.begin(), out.end(), [](const Ui::Contact& a, const Ui::Contact& b) {
			if ((a.distance >= 0) != (b.distance >= 0)) return a.distance >= 0;
			return a.name < b.name;
		});
		static bool logged = false;
		if (!logged) {
			logged = true;
			REX::INFO("stats: {} contacts (ship crew + following companions)", out.size());
			for (const auto& c : out) REX::INFO("stats:   {:08X} '{}' {} dead {}", c.formID, c.name, c.where, c.dead);
		}
		return out;
	}

	bool Summon(std::uint32_t a_formID)
	{
		if (!a_formID) return false;
		char cmd[48];
		std::snprintf(cmd, sizeof(cmd), "%08X.moveto player", a_formID);
		REX::INFO("contacts: console '{}'", cmd);
		RunNative(kExecuteConsole, cmd);
		return true;
	}

	std::vector<Ui::Blip> Radar(float a_range)
	{
		std::vector<Ui::Blip> out;
		auto*                 p = Player();
		if (!p) return out;
		const auto  me = PosOf(p);
		const float heading = At<float>(p, 0x88);
		const float s = std::sin(heading), c = std::cos(heading);
		HomeShip    ship;
		const auto  crew = CrewHandles(ship.ref);
		for (const auto h : HighActors()) {
			RefPtr a(h);
			if (!a.ref || a.ref == p || At<std::uint8_t>(a.ref, 0x2E) != kTypeACHR) continue;
			const auto  pos = PosOf(a.ref);
			const float dx = pos.x - me.x, dy = pos.y - me.y;
			const float d = Distance(pos, me);
			if (!(d <= a_range)) continue;
			Ui::Blip b;
			b.name = RefName(a.ref);
			if (b.name.empty()) b.name = Tr("未知");
			b.right = dx * c - dy * s;
			b.forward = dx * s + dy * c;
			b.distance = d;
			if (IsDead(a.ref)) b.kind = 3;
			else if (IsTeammate(a.ref) || std::find(crew.begin(), crew.end(), h) != crew.end()) b.kind = 2;
			else if (IsHostile(a.ref, p)) b.kind = 1;
			out.push_back(std::move(b));
		}
		std::sort(out.begin(), out.end(), [](const Ui::Blip& a, const Ui::Blip& b) { return a.distance < b.distance; });
		if (out.size() > 60) out.resize(60);
		return out;
	}

	// the outpost's own name (what the player named it in build mode); "" if it has none
	std::string OutpostName(void* a_beacon)
	{
		using fn_t = void* (*)(void*, bool);
		const auto* data = static_cast<const std::uint8_t*>(reinterpret_cast<fn_t>(kOutpostData.address())(a_beacon, false));
		if (!data || !At<const void*>(data, 0x70)) return {};
		const char* s = reinterpret_cast<const RE::BSFixedString*>(data + 0x70)->c_str();
		return s ? s : "";
	}

	std::vector<Ui::InfoRow> Outposts()
	{
		static std::vector<std::uint32_t>            beacons;
		static std::chrono::steady_clock::time_point scanned{};
		const auto                                   now = std::chrono::steady_clock::now();
		bool                                         logNames = false;
		if (scanned == std::chrono::steady_clock::time_point{} || now - scanned > std::chrono::seconds(30)) {
			beacons = ScanBeacons();
			scanned = now;
			logNames = true;
			REX::INFO("stats: {} outpost beacons", beacons.size());
		}
		std::vector<Ui::InfoRow> rows;
		auto*                    p = Player();
		void*                    myPlanet = p ? PlanetOf(p) : nullptr;
		int                      n = 0;
		for (const auto id : beacons) {
			auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(id);
			if (!ref) continue;
			// the outpost's name lives in its beacon data; the reference's own name is the beacon object's
			const auto  outpost = OutpostName(ref);
			// the player's outposts are references made in play (FF......) and always get a name; beacons placed by the
			// game or a mod (quest / fleet locations, templates) have neither: not the player's
			if ((id >> 24) != 0xFF && outpost.empty()) {
				if (logNames) REX::INFO("outpost {:08X}: skipped (placed by a plugin, no outpost name)", id);
				continue;
			}
			const char* dn = ref->GetDisplayFullName();
			const auto  refName = dn && *dn ? std::string(dn) : RefName(ref);
			std::string name = !outpost.empty() ? outpost : refName;
			++n;
			if (name.empty()) name = Tr("前哨站 ") + std::to_string(n);
			void*      planet = PlanetOf(ref);
			const auto planetName = Name(planet);
			const auto chain = LocationChain(ref);
			if (logNames) {
				REX::INFO("outpost {:08X}: name '{}' ref name '{}' location '{}'", id, outpost, refName, chain.empty() ? "" : chain[0]);
			}
			rows.push_back({ name, "", -1.0f, true });
			rows.push_back({ Tr("星球"), planetName.empty() ? "—" : planetName });
			if (!chain.empty() && !chain[0].empty() && chain[0] != planetName) rows.push_back({ Tr("地点"), chain[0] });
			if (p && planet && planet == myPlanet && !Interior(p)) {
				rows.push_back({ Tr("距离"), Meters(Distance(PosOf(ref), PosOf(p))) });
			}
		}
		return rows;
	}

	void HudNotify(const std::string& a_utf8) { RunNative(kNotification, a_utf8); }

	// The native returns early while the audio system is not up, and only touches the VM argument to log an empty
	// name, so callers must pass a non-empty ID.
	void PlayUiSound(const char* a_id)
	{
		if (a_id && *a_id) RunNative(kPlayMenuSound, a_id);
	}

	// the native clamps both strengths to 0..1 and starts both motors (small = high frequency)
	void Rumble(float a_small, float a_big, float a_seconds)
	{
		using fn_t = void (*)(void*, std::uint32_t, void*, float, float, float);
		reinterpret_cast<fn_t>(kShakeController.address())(nullptr, 0, nullptr, a_small, a_big, a_seconds);
	}
}

// ---- photo mode shutter ----
// PhotoModeMenu handles the UI event PhotoMode_TakeSnapshot in its BSTEventSink<PhotoMode_TakeSnapshot> part (menu
// +0x160, RTTI). That sub-vtable is ID 443439 (Starfield.exe+0x4C85BC8, COL offset 0x160); slot 1 = ProcessEvent
// FUN_1416000D0, which sets the menu's "take snapshot" flag. The slot is replaced to record the location first.
// The photo mode camera: world holder (ID 936470) +0x80 = "World Camera" NiCamera, world translate at +0xB0.
namespace
{
	constexpr REL::ID kSnapshotSinkVtbl{ 443439 };
	constexpr REL::ID kWorldHolder{ 936470 };

	using ProcessEvent_t = std::uint32_t (*)(void*, const void*, void*);
	ProcessEvent_t g_origSnapshot = nullptr;

	bool CameraPos(Pos& a_out)
	{
		const auto holder = *reinterpret_cast<const std::uint8_t* const*>(kWorldHolder.address());
		const auto cam = holder ? At<const std::uint8_t*>(holder, 0x80) : nullptr;
		if (!cam) return false;
		a_out = { At<float>(cam, 0xB0), At<float>(cam, 0xB4), At<float>(cam, 0xB8) };
		return std::isfinite(a_out.x) && std::isfinite(a_out.y);
	}

	std::uint32_t OnSnapshot(void* a_sink, const void* a_event, void* a_source)
	{
		auto* p = Player();
		if (p && At<void*>(p, 0xB0)) {
			const auto me = PosOf(p);
			Pos        cam{};
			// the photo mode camera can fly ~100 m from the player: use it when it is plausibly in the same frame
			const bool useCam = CameraPos(cam) && Distance(cam, me) < 150.0f;
			const auto geo = Stats::GeoWith(useCam ? &cam : nullptr);
			Gallery::QueueShotGeo(geo);
			REX::INFO("photomode: shutter at '{}' gps {} {:.5f} {:.5f} (camera {} {:.1f} {:.1f} {:.1f}, player {:.1f} {:.1f} {:.1f})",
				Gallery::PlaceTitle(geo), geo.gps, geo.lat, geo.lon, useCam, cam.x, cam.y, cam.z, me.x, me.y, me.z);
		}
		return g_origSnapshot(a_sink, a_event, a_source);
	}
}

namespace Stats
{
	void InstallPhotoModeHook()
	{
		auto* slot = reinterpret_cast<std::uintptr_t*>(kSnapshotSinkVtbl.address() + 8);
		std::uint32_t old = 0;
		if (!REX::W32::VirtualProtect(slot, sizeof(*slot), REX::W32::PAGE_READWRITE, &old)) {
			REX::ERROR("photomode: VirtualProtect failed");
			return;
		}
		g_origSnapshot = reinterpret_cast<ProcessEvent_t>(*slot);
		*slot = reinterpret_cast<std::uintptr_t>(&OnSnapshot);
		REX::W32::VirtualProtect(slot, sizeof(*slot), old, &old);
		REX::INFO("photomode: shutter hook installed (original {:X})", reinterpret_cast<std::uintptr_t>(g_origSnapshot));
	}
}
