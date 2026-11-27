// PhoneTest - P0 spike for the hand-held phone mod.
//
// Question this answers: can an SFSE plugin make the player hold an AnimObject in FIRST person, the way the
// game shows the digipick while lock picking?
//
// How the game does it (Starfield.exe 1.16.244, see docs/p0-findings.md):
//   SecurityMenu (Address Library ID 92372) reads the default object SecurityMenuInitiateAction_DO
//   (= AACT ActionDigipickRaise 003A15E3), builds a BGSActionData on the stack {source = player, action = AACT}
//   and calls the global PerformAction function pointer (ID 936442). The idle under that action
//   (DigipickRaise 003A1975, graph Human_1stPersonRoot.agx) sends IdleDigiPicEquip to the 1st-person graph,
//   and the animation's AnimObjLoad annotation puts AnimObjectDigiPic in the hands.
//
// This plugin does the same from hotkeys:
//   F7  ActionDataSlateOpen  (0001901F)    F8  ActionDataSlateClose (000222CD)
//   F11 cycle the model of AnimObjectDataSlate (ANIO 0005AFB8) through other hand-held meshes, so the next F7
//       shows a different object. The .afx annotation picks the ANIO by editor ID, so a truly separate ANIO needs
//       a new 1st-person graph branch; swapping the model path is the P0 shortcut.
// (F9 is the game's quickload key; F5 quicksave. Don't bind those.)
// Results go to Documents\My Games\Starfield\SFSE\Logs\PhoneTest.log.

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>

extern "C" __declspec(dllimport) short __stdcall GetAsyncKeyState(int a_vkey);

namespace
{
	// Address Library IDs, game 1.16.244
	constexpr REL::ID kPerformActionPtr{ 936442 };    // Starfield.exe+0x5FAFE70, holds fn 0x19AA9A0 (ID 101974)
	constexpr REL::ID kActionDataVtblInput{ 393262 };  // BGSActionData::vftable (ActionInput part)  +0x4AFAE18
	constexpr REL::ID kActionDataVtblOutput{ 393260 }; // BGSActionData::vftable (ActionOutput part) +0x4AFAE08

	constexpr std::uint8_t kFormTypeAACT = 7;  // TESForm+0x2E, the game checks this before performing

	// Layout copied from the stack object in SecurityMenu (ID 92372), offsets verified in the disassembly.
	struct ActionData
	{
		void*         vtblInput;   // 00
		void*         source;      // 08 actor
		void*         target;      // 10
		RE::TESForm*  action;      // 18 AACT
		std::uint32_t unk20;       // 20
		std::uint32_t pad24;
		void*         vtblOutput;  // 28
		void*         str30;       // 30 BSFixedString (empty)
		void*         str38;       // 38 BSFixedString (empty)
		std::uint32_t unk40;       // 40
		std::uint32_t pad44;
		std::uint64_t unk48;       // 48
		std::uint64_t unk50;       // 50
		std::uint32_t unk58;       // 58
		std::uint32_t unk5C;       // 5C
		std::uint32_t unk60;       // 60
		std::uint8_t  spare[0x1C]; // extra zeroed room in case the real object is larger
	};
	static_assert(offsetof(ActionData, vtblOutput) == 0x28);
	static_assert(offsetof(ActionData, unk60) == 0x60);

	using PerformAction_t = bool (*)(ActionData*);

	struct Binding
	{
		int           vkey;
		std::uint32_t formID;
		const char*   name;
	};
	constexpr std::array<Binding, 2> kBindings{ {
		{ 0x76, 0x0001901F, "ActionDataSlateOpen" },   // F7
		{ 0x77, 0x000222CD, "ActionDataSlateClose" },  // F8
	} };

	constexpr int           kCycleKey = 0x7A;  // F11
	constexpr std::uint32_t kSlateANIO = 0x0005AFB8;  // AnimObjectDataSlate
	constexpr std::array<const char*, 5> kModels{ {
		"AnimObjects\\AnimObjectDataSlate.nif",  // vanilla
		"AnimObjects\\AnimObjectVideoGrendel.nif",
		"AnimObjects\\AnimObjectTechReader01.nif",
		"AnimObjects\\AnimObjectIDCard.nif",
		"AnimObjects\\AnimObjectTablet.nif",
	} };

	std::array<bool, kBindings.size()> g_down{};
	bool                               g_cycleDown = false;
	std::size_t                        g_model = 0;
	std::atomic_bool                   g_ready{ false };

	void CycleModel()
	{
		auto* anio = RE::TESForm::LookupByID<RE::TESObjectANIO>(kSlateANIO);
		if (!anio) {
			REX::WARN("cycle: ANIO {:08X} not found", kSlateANIO);
			return;
		}
		g_model = (g_model + 1) % kModels.size();
		const std::string before{ anio->model.c_str() ? anio->model.c_str() : "" };
		anio->model = kModels[g_model];
		REX::INFO("cycle: AnimObjectDataSlate model '{}' -> '{}' (press F8 then F7 to see it)", before, anio->model.c_str());
	}

	void Perform(const Binding& a_b)
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		auto* form = RE::TESForm::LookupByID(a_b.formID);
		const auto fn = *reinterpret_cast<PerformAction_t*>(kPerformActionPtr.address());

		if (!player || !form || !fn) {
			REX::WARN("{}: missing player={} form={} fn={}", a_b.name, (void*)player, (void*)form, (void*)fn);
			return;
		}
		const auto type = *reinterpret_cast<const std::uint8_t*>(reinterpret_cast<std::uintptr_t>(form) + 0x2E);
		if (type != kFormTypeAACT) {
			REX::WARN("{}: form {:08X} has type {} (expected AACT = {})", a_b.name, a_b.formID, type, kFormTypeAACT);
			return;
		}

		ActionData data;
		std::memset(&data, 0, sizeof(data));
		data.vtblInput = reinterpret_cast<void*>(kActionDataVtblInput.address());
		data.vtblOutput = reinterpret_cast<void*>(kActionDataVtblOutput.address());
		data.source = player;
		data.action = form;

		const bool ok = fn(&data);
		REX::INFO("{} ({:08X}) -> PerformAction returned {}", a_b.name, a_b.formID, ok);
	}

	// Runs on the game's task queue: poll the hotkeys, act on key-down edges.
	void Tick()
	{
		if (!g_ready.load()) {
			return;
		}
		for (std::size_t i = 0; i < kBindings.size(); ++i) {
			const bool down = (GetAsyncKeyState(kBindings[i].vkey) & 0x8000) != 0;
			if (down && !g_down[i]) {
				Perform(kBindings[i]);
			}
			g_down[i] = down;
		}
		const bool cycle = (GetAsyncKeyState(kCycleKey) & 0x8000) != 0;
		if (cycle && !g_cycleDown) {
			CycleModel();
		}
		g_cycleDown = cycle;
	}

	void OnMessage(SFSE::MessagingInterface::Message* a_msg)
	{
		if (!a_msg) {
			return;
		}
		// SFSE for Starfield has no load-game message; press the keys only after a save is loaded.
		if (a_msg->type == SFSE::MessagingInterface::kPostDataLoad) {
			if (const auto* tasks = SFSE::GetTaskInterface()) {
				tasks->AddPermanentTask([]() { Tick(); });
				g_ready = true;
				REX::INFO("hotkey task installed (F7 slate open, F8 slate close, F11 cycle slate model)");
			} else {
				REX::ERROR("no task interface; hotkeys disabled");
			}
		}
	}
}

SFSE_PLUGIN_LOAD(const SFSE::LoadInterface* a_sfse)
{
	SFSE::Init(a_sfse);
	REX::INFO("PhoneTest loaded; PerformAction ptr at Starfield.exe+0x{:X}", kPerformActionPtr.offset());
	if (const auto* msg = SFSE::GetMessagingInterface(); !msg || !msg->RegisterListener(OnMessage)) {
		REX::ERROR("could not register for SFSE messages");
	}
	return true;
}
