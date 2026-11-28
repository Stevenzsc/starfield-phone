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
//   F11 cycle the model of AnimObjectDataSlate (ANIO 0005AFB8) through the model= lines of PhoneTest.ini and re-open
//       twice so the new model is loaded (preload); the next F7
//       shows a different object. The .afx annotation picks the ANIO by editor ID, so a truly separate ANIO needs
//       a new 1st-person graph branch; swapping the model path is the P0 shortcut.
//   The screen shows N white squares at the top for model N of PhoneTest.ini (1 = first line).
//   Numpad: live placement tuner while open (tuner.cpp; it captures the object in the engine's attach call).
// (F9 is the game's quickload key; F5 quicksave. Don't bind those.)
// Results go to Documents\My Games\Starfield\SFSE\Logs\PhoneTest.log.

#include "pch.h"
#include "camera.h"
#include "screen.h"
#include "tuner.h"
#include "ui.h"
#include "quests.h"
#include "stats.h"
#include "viewprobe.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

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
	// Screen test texture (tools/make_test_assets.py): Textures\PhoneTest\PhoneScreenTest.dds
	constexpr std::uint32_t kScreenWidth = 756;
	constexpr std::uint32_t kScreenHeight = 1332;

	// Model list for F11: Data\SFSE\Plugins\PhoneTest.ini, "model=<path under Meshes>" lines, re-read on every press
	// (tools/make_test_assets.py writes it). Falls back to the vanilla slate.
	constexpr const char* kIniPath = "Data\\SFSE\\Plugins\\PhoneTest.ini";

	bool g_devKeys = false;  // dev_keys=1 in the ini: F11 model cycle + numpad tuner

	// screen_rotate=0|90|180|270, screen_mirror=0|1 and screen_aspect=<visible height / width> in PhoneTest.ini:
	// how the upright UI maps onto the texture.
	// Re-read on every open, so they can be changed while the game runs.
	void LoadScreenOptions()
	{
		std::ifstream in(kIniPath);
		std::string   line;
		int           rotate = 0;
		bool          mirror = false;
		float         aspect = 1.0f;
		float         inset[4] = { 0, 0, 0, 0 };  // left, top, right, bottom
		constexpr const char* insetKeys[4] = { "screen_inset_left=", "screen_inset_top=", "screen_inset_right=", "screen_inset_bottom=" };
		while (std::getline(in, line)) {
			if (!line.empty() && line.back() == '\r') {
				line.pop_back();
			}
			if (line.rfind("screen_rotate=", 0) == 0) {
				rotate = std::atoi(line.c_str() + 14);
			} else if (line.rfind("screen_mirror=", 0) == 0) {
				mirror = std::atoi(line.c_str() + 14) != 0;
			} else if (line.rfind("screen_aspect=", 0) == 0) {
				aspect = static_cast<float>(std::atof(line.c_str() + 14));
			} else {
				for (int k = 0; k < 4; ++k) {
					const std::size_t n = std::strlen(insetKeys[k]);
					if (line.rfind(insetKeys[k], 0) == 0) {
						inset[k] = static_cast<float>(std::atof(line.c_str() + n));
					}
				}
			}
		}
		Ui::SetOrientation(rotate, mirror, aspect);
		Ui::SetInsets(inset[0], inset[1], inset[2], inset[3]);
		REX::INFO("screen: rotate {} mirror {} aspect {:.3f}", rotate, mirror, aspect);
	}

	// brightness=<30..100> and open_key=<index into Ui's open-key choices> in PhoneTest.ini (Settings app)
	void LoadSettings()
	{
		std::ifstream in(kIniPath);
		std::string   line;
		Ui::Settings  st;
		while (std::getline(in, line)) {
			if (!line.empty() && line.back() == '\r') {
				line.pop_back();
			}
			if (line.rfind("brightness=", 0) == 0) {
				st.brightness = std::atoi(line.c_str() + 11);
			} else if (line.rfind("open_key=", 0) == 0) {
				st.openKey = std::atoi(line.c_str() + 9);
			} else if (line.rfind("dev_keys=", 0) == 0) {
				g_devKeys = std::atoi(line.c_str() + 9) != 0;
			}
		}
		Ui::SetSettings(st);
		const auto now = Ui::GetSettings();
		REX::INFO("settings: brightness {}% open key {}", now.brightness, Ui::OpenKeyName(now.openKey));
	}

	// rewrite the settings lines of the ini, keeping every other line
	void SaveSettings()
	{
		const auto               st = Ui::GetSettings();
		std::vector<std::string> lines;
		{
			std::ifstream in(kIniPath);
			std::string   line;
			while (std::getline(in, line)) {
				if (!line.empty() && line.back() == '\r') {
					line.pop_back();
				}
				if (line.rfind("brightness=", 0) != 0 && line.rfind("open_key=", 0) != 0) {
					lines.push_back(line);
				}
			}
		}
		lines.push_back("brightness=" + std::to_string(st.brightness));
		lines.push_back("open_key=" + std::to_string(st.openKey));
		std::ofstream out(kIniPath, std::ios::trunc);
		for (const auto& l : lines) {
			out << l << '\n';
		}
		REX::INFO("settings saved: brightness {}% open key {}", st.brightness, Ui::OpenKeyName(st.openKey));
	}

	std::vector<std::string> LoadModels()
	{
		std::vector<std::string> models;
		std::ifstream            in(kIniPath);
		std::string              line;
		while (std::getline(in, line)) {
			if (!line.empty() && line.back() == '\r') {
				line.pop_back();
			}
			if (line.rfind("model=", 0) == 0 && line.size() > 6) {
				models.push_back(line.substr(6));
			}
		}
		if (models.empty()) {
			models.emplace_back("AnimObjects\\AnimObjectDataSlate.nif");
		}
		return models;
	}

	std::array<bool, kBindings.size()> g_down{};
	bool                               g_cycleDown = false;
	std::size_t                        g_model = 0;
	std::atomic_bool                   g_ready{ false };
	bool                               g_open = false;
	bool                               g_preloaded = false;  // current model shown once (see QueuePreloadOpen)
	bool                               g_openKeyDown = false;
	std::array<bool, 6>                g_navDown{};
	constexpr std::size_t              kStepPhotoMode = 99;  // Step::binding value: open the game's photo mode

	// Timed follow-up actions (preload: a model that is not loaded yet is missing on its first open, so after a
	// model change we open, close and open again).
	struct Step
	{
		std::chrono::steady_clock::time_point at;
		std::size_t                           binding;
	};
	std::deque<Step> g_steps;

	// last successful open / close; the next one waits until its animation is over
	constexpr auto                        kActionGap = std::chrono::milliseconds(2500);
	std::chrono::steady_clock::time_point g_lastAction{};


	void Perform(const Binding& a_b);

	void QueuePreloadOpen()
	{
		using namespace std::chrono_literals;
		// The equip/unequip animations must finish first: at 700 ms spacing most actions returned false (in game).
		constexpr auto gap = 2500ms;
		const auto     now = std::chrono::steady_clock::now();
		g_steps.clear();
		if (g_open) {
			g_steps.push_back({ now, 1 });            // close
			g_steps.push_back({ now + gap, 0 });      // open (loads the model)
			g_steps.push_back({ now + 2 * gap, 1 });  // close
			g_steps.push_back({ now + 3 * gap, 0 });  // open (shows it)
		} else {
			g_steps.push_back({ now, 0 });
			g_steps.push_back({ now + gap, 1 });
			g_steps.push_back({ now + 2 * gap, 0 });
		}
	}

	void CycleModel()
	{
		auto* anio = RE::TESForm::LookupByID<RE::TESObjectANIO>(kSlateANIO);
		if (!anio) {
			REX::WARN("cycle: ANIO {:08X} not found", kSlateANIO);
			return;
		}
		const auto models = LoadModels();
		g_model = (g_model + 1) % models.size();
		const std::string before{ anio->model.c_str() ? anio->model.c_str() : "" };
		anio->model = models[g_model].c_str();
		REX::INFO("cycle: model {}/{} '{}' -> '{}'", g_model + 1, models.size(), before, anio->model.c_str());
		Screen::SetMarker(static_cast<std::uint32_t>(g_model + 1));
		QueuePreloadOpen();
		g_preloaded = true;
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
		if (ok) {
			g_lastAction = std::chrono::steady_clock::now();
			g_open = a_b.formID == 0x0001901F;
			if (g_open) {
				LoadScreenOptions();
				Ui::OnOpen();
				ViewProbe::Run();  // second-view research: log the renderer's view / output tables
			}
			Screen::SetActive(g_open);
			Camera::SetActive(g_open);
		}
	}

	void OpenPhotoMode()
	{
		auto* q = RE::UIMessageQueue::GetSingleton();
		if (!q) {
			REX::WARN("camera: no UIMessageQueue");
			return;
		}
		const auto r = q->AddMessage(RE::BSFixedString{ "PhotoModeMenu" }, RE::UI_MESSAGE_TYPE::kShow);
		REX::INFO("camera: PhotoModeMenu show -> {}", r);
	}

	// start with the first phone model of the ini (line 2; line 1 is the vanilla slate)
	// Preload a model into the engine's model cache, so the AnimObject shows on its FIRST open (otherwise its async
	// load finishes after the animation's AnimObjDraw and the hand is empty). Same calls as FUN_14242CB10 /
	// FUN_142428F00 in Starfield.exe 1.16.244:
	//   NormalizePath  (ID 137147, FUN_1427DB100)(char buf[0x108], path, path, "meshes\\") -> normalized path
	//   MakeResourceID (ID 137049, FUN_1427D7580)(u32 id[3], normalized)                   -> BSResource ID
	//   DemandModel    (ID 127164, FUN_142428730)(id, &handle, &args{u32 0, u8 0x2D, u8 0}, 5)
	//                  -> 0 loaded, 6 queued (async); the handle keeps the entry alive, so we never release it.
	bool PreloadModel(const std::string& a_path)
	{
		using normalize_t = const char* (*)(char*, const char*, const char*, const char*);
		using makeId_t = void (*)(std::uint32_t*, const char*);
		using demand_t = int (*)(const std::uint32_t*, void**, void*, std::uint32_t);
		static const REL::Relocation<normalize_t> normalize{ REL::ID(137147) };
		static const REL::Relocation<makeId_t>    makeId{ REL::ID(137049) };
		static const REL::Relocation<demand_t>    demand{ REL::ID(127164) };

		alignas(16) char buf[0x110]{};
		const char*      norm = normalize(buf, a_path.c_str(), a_path.c_str(), "meshes\\");
		if (!norm) {
			REX::WARN("preload: could not normalize '{}'", a_path);
			return false;
		}
		alignas(8) std::uint32_t id[4]{};
		makeId(id, norm);
		struct Args
		{
			std::uint32_t a = 0;
			std::uint8_t  b = 0x2D;
			std::uint8_t  c = 0;
			std::uint16_t pad = 0;
		} args;
		static void* handle = nullptr;  // kept for the session
		const int    r = demand(id, &handle, &args, 5);
		REX::INFO("preload: '{}' ({}) -> {} handle {}", a_path, norm, r, handle);
		return (r == 0 || r == 6) && handle != nullptr;
	}

	void UseDefaultModel()
	{
		const auto models = LoadModels();
		if (models.size() < 2) {
			return;
		}
		if (auto* anio = RE::TESForm::LookupByID<RE::TESObjectANIO>(kSlateANIO)) {
			g_model = 1;
			anio->model = models[1].c_str();
			Screen::SetMarker(2);
			REX::INFO("model: using '{}'", models[1]);
			// cached already: skip the open/close/open preload sequence on the first open
			g_preloaded = PreloadModel(models[1]);
		}
	}

	bool KeyEdge(int a_vk, bool& a_wasDown)
	{
		const bool down = (GetAsyncKeyState(a_vk) & 0x8000) != 0;
		const bool edge = down && !a_wasDown;
		a_wasDown = down;
		return edge;
	}

	// Runs on the game's task queue: poll the hotkeys, act on key-down edges.
	void Tick()
	{
		if (!g_ready.load()) {
			return;
		}
		// open key (Settings app; default F7) toggles the phone; F8 also puts it away
		if (KeyEdge(Ui::OpenKeyVk(Ui::GetSettings().openKey), g_openKeyDown) && g_steps.empty()) {
			const auto ready = g_lastAction + kActionGap;
			if (std::chrono::steady_clock::now() < ready) {
				// the raise / lower animation is still playing: an action now freezes the view (seen in game) -> queue it
				g_steps.push_back({ ready, std::size_t(g_open ? 1 : 0) });
			} else if (g_open) {
				Perform(kBindings[1]);
			} else if (!g_preloaded) {
				QueuePreloadOpen();  // first open of this model shows nothing; open, close, open
				g_preloaded = true;
			} else {
				Perform(kBindings[0]);
			}
		}
		if (KeyEdge(kBindings[1].vkey, g_down[1]) && g_open) {
			Perform(kBindings[1]);
		}

		// phone navigation: arrows, Enter, Backspace (unbound in the game by default)
		if (g_open && g_steps.empty()) {
			constexpr std::pair<int, Ui::Key> nav[] = {
				{ 0x26, Ui::Key::kUp }, { 0x28, Ui::Key::kDown }, { 0x25, Ui::Key::kLeft },
				{ 0x27, Ui::Key::kRight }, { 0x0D, Ui::Key::kEnter }, { 0x08, Ui::Key::kBack },
			};
			for (std::size_t i = 0; i < std::size(nav); ++i) {
				if (!KeyEdge(nav[i].first, g_navDown[i])) {
					continue;
				}
				switch (Ui::Input(nav[i].second)) {
				case Ui::Action::kSettingsChanged:
					SaveSettings();
					break;
				case Ui::Action::kToggleTrack: {
					const auto sel = Ui::SelectedQuest();
					if (sel.formID) {
						Quests::SetTracked(sel.formID, !sel.tracked);
						Ui::SetQuests(Quests::CollectActive(40));  // show the new state right away
					}
					break;
				}
				case Ui::Action::kTakePhoto:
					// the photo comes from the same source as the viewfinder (the second view once it renders)
					Camera::RequestPhoto();
					break;
				case Ui::Action::kOpenPhotoMode:
					// put the phone away, then open photo mode once the hand is down
					Perform(kBindings[1]);
					g_steps.push_back({ std::chrono::steady_clock::now() + std::chrono::milliseconds(1500), kStepPhotoMode });
					break;
				default:
					break;
				}
			}
		} else {
			for (auto& d : g_navDown) {
				d = false;
			}
		}
		const bool cycle = (GetAsyncKeyState(kCycleKey) & 0x8000) != 0;
		if (cycle && !g_cycleDown && g_devKeys) {
			CycleModel();
		}
		g_cycleDown = cycle;

		if (!g_steps.empty() && std::chrono::steady_clock::now() >= g_steps.front().at) {
			const auto b = g_steps.front().binding;
			g_steps.pop_front();
			if (b == kStepPhotoMode) {
				OpenPhotoMode();
			} else {
				Perform(kBindings[b]);
			}
		}

		// second view: our own camera + output rendered into a texture while the Camera page is shown
		{
			int cw = 0, ch = 0;
			ViewProbe::Tick(g_open && Ui::CameraTarget(cw, ch));
		}
		Tuner::Tick(g_open && g_devKeys);

		// Quests app: refresh every 2 s while its page is open
		static auto nextQuests = std::chrono::steady_clock::now();
		if (g_open && Ui::WantsQuests() && std::chrono::steady_clock::now() >= nextQuests) {
			nextQuests = std::chrono::steady_clock::now() + std::chrono::seconds(2);
			auto list = Quests::CollectActive(40);
			REX::INFO("quests: {} active with displayed objectives", list.size());
			Ui::SetQuests(std::move(list));
		} else if (!Ui::WantsQuests()) {
			nextQuests = std::chrono::steady_clock::now();
		}

		// Character / Ship / Star map apps: refresh every second while their page is open
		static auto nextInfo = std::chrono::steady_clock::now();
		if (const auto info = Ui::WantsInfo(); g_open && info != Ui::Info::kNone) {
			if (std::chrono::steady_clock::now() >= nextInfo) {
				nextInfo = std::chrono::steady_clock::now() + std::chrono::seconds(1);
				switch (info) {
				case Ui::Info::kCharacter: Ui::SetInfo(info, Stats::Character()); break;
				case Ui::Info::kShip: Ui::SetInfo(info, Stats::Ship()); break;
				case Ui::Info::kMap: Ui::SetInfo(info, Stats::Location()); break;
				default: break;
				}
			}
		} else {
			nextInfo = std::chrono::steady_clock::now();
		}

		// in-game calendar (home screen, calendar app, message time stamps), twice a second; also while the phone
		// is put away, so messages get the right time
		static auto   nextTime = std::chrono::steady_clock::now();
		static double gameHours = -1;  // game time at the last message poll, in hours
		if (std::chrono::steady_clock::now() >= nextTime) {
			nextTime = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
			if (auto* cal = RE::Calendar::GetSingleton()) {
				Ui::SetGameTime(cal->GetYear(), cal->GetMonth(), cal->GetDay(), cal->GetHour());
			}
		}

		// Messages app: objective changes and level-ups, once a second
		static auto nextPoll = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		if (std::chrono::steady_clock::now() >= nextPoll) {
			nextPoll = std::chrono::steady_clock::now() + std::chrono::seconds(1);
			double hours = gameHours;
			if (auto* cal = RE::Calendar::GetSingleton()) {
				hours = ((double(cal->GetYear()) * 12 + cal->GetMonth()) * 31 + cal->GetDay()) * 24 + cal->GetHour();
			}
			// a save load (or fast travel, sleep) moves the game clock: record the new state without messages
			const bool silent = gameHours < 0 || hours < gameHours || hours - gameHours > 2.0;
			gameHours = hours;
			for (auto& ch : Quests::PollChanges(silent)) {
				REX::INFO("message: {} | {}", ch.title, ch.text);
				Ui::PushMessage(std::move(ch.title), std::move(ch.text));
			}
			static int lastLevel = 0;
			const int  level = Stats::PlayerLevel();
			if (!silent && lastLevel > 0 && level == lastLevel + 1) {
				Ui::PushMessage("角色", "升级到 " + std::to_string(level) + " 级");
			}
			lastLevel = level;
		}
	}

	void OnMessage(SFSE::MessagingInterface::Message* a_msg)
	{
		if (!a_msg) {
			return;
		}
		// SFSE for Starfield has no load-game message; press the keys only after a save is loaded.
		if (a_msg->type == SFSE::MessagingInterface::kPostDataLoad) {
			if (const auto* tasks = SFSE::GetTaskInterface()) {
				Tuner::Load();
				LoadSettings();
				UseDefaultModel();
				tasks->AddPermanentTask([]() { Tick(); });
				g_ready = true;
				REX::INFO("hotkey task installed (open key {} toggles the phone, F8 puts it away, F11 cycles the model)",
					Ui::OpenKeyName(Ui::GetSettings().openKey));
			} else {
				REX::ERROR("no task interface; hotkeys disabled");
			}
		}
	}
}

SFSE_PLUGIN_LOAD(const SFSE::LoadInterface* a_sfse)
{
	SFSE::Init(a_sfse, { .trampoline = true, .trampolineSize = 64 });
	REX::INFO("PhoneTest loaded; PerformAction ptr at Starfield.exe+0x{:X}", kPerformActionPtr.offset());
	Ui::Init([](const char* a_msg) { REX::INFO("{}", a_msg); });
	Screen::Install(kScreenWidth, kScreenHeight, [](const char* a_msg) { REX::INFO("{}", a_msg); });
	Screen::WatchViewTarget(ViewProbe::kTargetWidth, ViewProbe::kTargetHeight);
	Camera::Install([](const char* a_msg) { REX::INFO("{}", a_msg); });
	Tuner::Install();
	if (const auto* msg = SFSE::GetMessagingInterface(); !msg || !msg->RegisterListener(OnMessage)) {
		REX::ERROR("could not register for SFSE messages");
	}
	return true;
}
