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
#include "lang.h"
#include "camera.h"
#include "gamepad.h"
#include "gallery.h"
#include "music.h"
#include "screen.h"
#include "tuner.h"
#include "ui.h"
#include "quests.h"
#include "stats.h"
#include "viewprobe.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <iterator>
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

	bool g_devKeys = false;    // dev_keys=1 in the ini: F11 model cycle + numpad tuner
	bool g_cameraAnim = true;  // camera_anim=0 in the ini: Camera page arm pose from runtime bone offsets, not the graph

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
			} else if (line.rfind("sound=", 0) == 0) {
				st.sound = std::atoi(line.c_str() + 6) != 0;
			} else if (line.rfind("vibration=", 0) == 0) {
				st.vibration = std::atoi(line.c_str() + 10) != 0;
			} else if (line.rfind("music_volume=", 0) == 0) {
				st.musicVolume = std::atoi(line.c_str() + 13);
			} else if (line.rfind("language=", 0) == 0) {
				st.language = std::atoi(line.c_str() + 9);
			} else if (line.rfind("pad_open=", 0) == 0) {
				st.padOpen = std::atoi(line.c_str() + 9);
			} else if (line.rfind("dev_keys=", 0) == 0) {
				g_devKeys = std::atoi(line.c_str() + 9) != 0;
			} else if (line.rfind("camera_anim=", 0) == 0) {
				g_cameraAnim = std::atoi(line.c_str() + 12) != 0;
			}
		}
		Ui::SetSettings(st);
		const auto now = Ui::GetSettings();
		REX::INFO("settings: brightness {}% open key {}", now.brightness, Ui::OpenKeyName(now.openKey));
	}

	// "auto" language: the phone follows the language of the game's own text. The name of the Credits form (MISC
	// 0000000F, "信用点" / "Credits") read through GetFullName (ID 47496): Chinese if it holds a CJK character.
	void DetectGameLanguage()
	{
		const char* name = nullptr;
		if (auto* credits = RE::TESForm::LookupByID(0x0000000F)) {
			using fn_t = const char* (*)(const RE::TESForm*);
			name = reinterpret_cast<fn_t>(REL::ID(47496).address())(credits);
		}
		bool cjk = false;
		for (const auto* p = reinterpret_cast<const unsigned char*>(name ? name : ""); *p; ++p) {
			// UTF-8 lead bytes 0xE4..0xE9 = CJK ideographs (U+4000..U+9FFF); Japanese "クレジット" (kana) and Korean stay English
			if (*p >= 0xE4 && *p <= 0xE9) {
				cjk = true;
				break;
			}
		}
		Lang::SetGameEnglish(name && *name && !cjk);
		REX::INFO("language: game text '{}' -> {}", name ? name : "", cjk ? "Chinese" : "English");
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
				const bool ours = line.rfind("brightness=", 0) == 0 || line.rfind("open_key=", 0) == 0 || line.rfind("sound=", 0) == 0 ||
				                  line.rfind("vibration=", 0) == 0 || line.rfind("music_volume=", 0) == 0 || line.rfind("language=", 0) == 0 ||
				                  line.rfind("pad_open=", 0) == 0;
				if (!ours) {
					lines.push_back(line);
				}
			}
		}
		lines.push_back("brightness=" + std::to_string(st.brightness));
		lines.push_back("open_key=" + std::to_string(st.openKey));
		lines.push_back(std::string("sound=") + (st.sound ? "1" : "0"));
		lines.push_back(std::string("vibration=") + (st.vibration ? "1" : "0"));
		lines.push_back("music_volume=" + std::to_string(st.musicVolume));
		lines.push_back("language=" + std::to_string(st.language));
		lines.push_back("pad_open=" + std::to_string(st.padOpen));
		std::ofstream out(kIniPath, std::ios::trunc);
		for (const auto& l : lines) {
			out << l << '\n';
		}
		REX::INFO("settings saved: brightness {}% open key {}", st.brightness, Ui::OpenKeyName(st.openKey));
	}

	// While the phone is out, the game must not act on the buttons the phone uses (controller A / B / D-pad):
	// an input enable layer, the mechanism behind Papyrus InputEnableLayer.DisablePlayerControls (native
	// FUN_142024A90 calls EnableUserEvent ID 63121 / EnableOtherEvent ID 65852 on the layer; layer from
	// BSInputEnableManager::AllocateNewLayer ID 124211, manager ID 938036). Walking and looking stay enabled.
	RE::BSInputEnableLayer* g_inputLayer = nullptr;
	bool                    g_inputBlocked = false;

	void BlockGameInput(bool a_block)
	{
		if (a_block == g_inputBlocked) {
			return;
		}
		auto* mgr = RE::BSInputEnableManager::GetSingleton();
		if (!mgr) {
			return;
		}
		if (!g_inputLayer && !mgr->AllocateNewLayer(&g_inputLayer, "PhoneTest")) {
			REX::WARN("input: could not allocate an input enable layer");
			return;
		}
		using UF = RE::USER_EVENT_FLAG;
		using OF = RE::OTHER_EVENT_FLAG;
		const auto user = UF::Fighting | UF::Sneaking | UF::Jumping;
		const auto other = OF::Activate | OF::Favorites | OF::HandScanner | OF::VATS | OF::CamSwitch;
		g_inputLayer->EnableUserEvent(user, !a_block, RE::USER_EVENT_SENDER_ID::Script);
		g_inputLayer->EnableOtherEvent(other, !a_block, RE::USER_EVENT_SENDER_ID::Script);
		g_inputBlocked = a_block;
		REX::INFO("input: game controls {} (layer {})", a_block ? "blocked" : "restored", g_inputLayer->GetLayerID());
	}

	// controller rumble, if enabled in the Settings app
	void Buzz(float a_small, float a_big, float a_seconds)
	{
		if (Ui::GetSettings().vibration) {
			Stats::Rumble(a_small, a_big, a_seconds);
		}
	}

	// button sound (the game's own menu sounds) and a light tick on Enter
	void PlayFeedback(Ui::Feedback a_fb)
	{
		if (a_fb == Ui::Feedback::kNone) {
			return;
		}
		if (Ui::GetSettings().sound) {
			switch (a_fb) {
			case Ui::Feedback::kMove: Stats::PlayUiSound("UIMenuGeneralFocus"); break;
			case Ui::Feedback::kOpen:
			case Ui::Feedback::kAction: Stats::PlayUiSound("UIMenuGeneralOK"); break;
			case Ui::Feedback::kBack: Stats::PlayUiSound("UIMenuGeneralCancel"); break;
			default: break;
			}
		}
		if (a_fb == Ui::Feedback::kOpen || a_fb == Ui::Feedback::kAction) {
			Buzz(0.2f, 0.0f, 0.04f);
		}
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


	bool Perform(const Binding& a_b);

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

	// The engine's PerformAction for the player, no phone state changes. Returns true when the engine performed it.
	bool PerformRaw(const Binding& a_b)
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		auto* form = RE::TESForm::LookupByID(a_b.formID);
		const auto fn = *reinterpret_cast<PerformAction_t*>(kPerformActionPtr.address());

		if (!player || !form || !fn) {
			REX::WARN("{}: missing player={} form={} fn={}", a_b.name, (void*)player, (void*)form, (void*)fn);
			return false;
		}
		const auto type = *reinterpret_cast<const std::uint8_t*>(reinterpret_cast<std::uintptr_t>(form) + 0x2E);
		if (type != kFormTypeAACT) {
			REX::WARN("{}: form {:08X} has type {} (expected AACT = {})", a_b.name, a_b.formID, type, kFormTypeAACT);
			return false;
		}

		ActionData data;
		std::memset(&data, 0, sizeof(data));
		data.vtblInput = reinterpret_cast<void*>(kActionDataVtblInput.address());
		data.vtblOutput = reinterpret_cast<void*>(kActionDataVtblOutput.address());
		data.source = player;
		data.action = form;

		return fn(&data);
	}

	// Returns true when the engine performed the action.
	bool Perform(const Binding& a_b)
	{
		const bool ok = PerformRaw(a_b);
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
			BlockGameInput(g_open);
		} else if (g_open && a_b.formID != 0x0001901F) {
			// The engine refuses to close a slate the player is not holding. That happens after a save was
			// loaded while the phone was open: our state survives the load, the player's hands do not.
			// Drop the stale state, otherwise every key press would only retry the close.
			REX::INFO("phone state was stale (not held any more, e.g. after a load); reset to closed");
			g_open = false;
			Screen::SetActive(false);
			Camera::SetActive(false);
			BlockGameInput(false);
		}
		return ok;
	}

	// ---- Camera page hold animation ----
	// The phone's Camera page plays a 1st-person animation: the mod's Human_1stPersonRoot.agx adds a PhoneCamera
	// state to the dataslate state machine (Idle <-> PhoneCamera on events PhoneCameraIn / PhoneCameraOut, file
	// PhoneCamera_DataSlate.af; tools/make_camera_graph.py, tools/make_camera_anim.py).
	// Sending the event: PerformAction (FUN_1419AA9A0 -> FUN_1419AAAF0) picks the IDLE, writes its event name into the
	// action data (+0x30) and hands it to the actor's animation graph holder at actor +0x60, vtable slot 1 (+0x08):
	// fn(holder, BSFixedString* event, 0) -> bool. We call that directly. (Going through ActionDataSlateClose with the
	// IDLE's event swapped did reach the graph, but the action's own close handling put the phone away.)
	bool SendSlateEvent(bool a_in)
	{
		static const RE::BSFixedString in("PhoneCameraIn");
		static const RE::BSFixedString out("PhoneCameraOut");
		const RE::BSFixedString&       ev = a_in ? in : out;

		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player) {
			return false;
		}
		const auto holder = reinterpret_cast<std::uintptr_t>(player) + 0x60;
		using Notify_t = bool (*)(std::uintptr_t, const RE::BSFixedString*, std::uintptr_t);
		const auto fn = (*reinterpret_cast<Notify_t* const*>(holder))[1];
		const bool ok = fn(holder, &ev, 0);
		REX::INFO("camera hold: {} -> graph notify returned {}", ev.c_str(), ok);
		return ok;
	}

	// Camera page edges: enter / leave the PhoneCamera state. Without the animation (camera_anim=0, or the event did
	// not go through) the arm takes the runtime bone offsets as before. Closing the phone from the Camera page needs
	// nothing: the graph goes PhoneCamera -> Unequip on DataSlateClose.
	void CameraHoldTick(bool a_open, bool a_on)
	{
		static bool held = false;
		if (!a_open || !g_cameraAnim) {
			held = false;
			Tuner::SetArmOffsets(true);
			return;
		}
		if (a_on == held) {
			return;
		}
		held = a_on;
		const bool ok = SendSlateEvent(a_on);
		Tuner::SetArmOffsets(!(a_on && ok));
	}

	// PhoneTest.ini camera_crop=x0,y0,x1,y1 (fractions of the screen) and camera_hide_hud=0/1, re-read every second
	void CameraPageTick(bool a_on)
	{
		static bool hudHidden = false;
		static auto next = std::chrono::steady_clock::now();
		static bool hideHud = true;
		if (a_on && std::chrono::steady_clock::now() >= next) {
			next = std::chrono::steady_clock::now() + std::chrono::seconds(1);
			float         r[4] = { 0.40F, 0.04F, 0.98F, 0.72F };
			std::ifstream in(kIniPath);
			std::string   line;
			while (std::getline(in, line)) {
				if (!line.empty() && line.back() == '\r') {
					line.pop_back();
				}
				if (line.rfind("camera_crop=", 0) == 0) {
					float v[4];
					if (std::sscanf(line.c_str() + 12, "%f,%f,%f,%f", &v[0], &v[1], &v[2], &v[3]) == 4) {
						std::copy(std::begin(v), std::end(v), std::begin(r));
					}
				} else if (line.rfind("camera_hide_hud=", 0) == 0) {
					hideHud = line.size() > 16 && line[16] != '0';
				}
			}
			Camera::SetCropRegion(r[0], r[1], r[2], r[3]);
		}
		const bool wantHidden = a_on && hideHud;
		if (wantHidden != hudHidden) {
			if (auto* q = RE::UIMessageQueue::GetSingleton()) {
				const auto res = q->AddMessage(RE::BSFixedString{ "HUDMenu" }, wantHidden ? RE::UI_MESSAGE_TYPE::kHide : RE::UI_MESSAGE_TYPE::kShow);
				REX::INFO("camera: HUDMenu {} -> {}", wantHidden ? "hide" : "show", res);
			}
			hudHidden = wantHidden;
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

	bool g_padNavHold = false;  // controller buttons used to open / close are still held

	constexpr float kMusicDuck = 0.10f;  // music volume factor during dialogue

	// a key or mouse button went down since the last check (checked every 50 ms; the task runs far more often)
	bool KeyboardUsed()
	{
		static std::array<bool, 256>                 was{};
		static std::chrono::steady_clock::time_point next{};
		const auto                                   now = std::chrono::steady_clock::now();
		if (now < next) {
			return false;
		}
		next = now + std::chrono::milliseconds(50);
		bool used = false;
		for (int vk = 0x01; vk < 0xFF; ++vk) {
			if (vk == 0x03 || vk == 0x07 || (vk >= 0xC3 && vk <= 0xDA)) {
				continue;  // cancel, reserved, VK_GAMEPAD_* (the pad must not count as keyboard)
			}
			const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
			used = used || (down && !was[std::size_t(vk)]);
			was[std::size_t(vk)] = down;
		}
		return used;
	}

	// weapon out when the phone was asked for: holster it first, open once it is away (or after a timeout)
	std::chrono::steady_clock::time_point g_holsterUntil{};
	bool                                  g_holstering = false;

	// take the phone out or put it away (open key, controller gesture, B on the home screen)
	void TogglePhone(bool a_afterHolster = false)
	{
		if (!a_afterHolster && !g_open) {
			if (g_holstering) return;  // already putting the weapon away for the phone
			if (const int state = Stats::WeaponState(); state != 0) {
				REX::INFO("phone: weapon out (state {}), holstering first", state);
				Stats::SheatheWeapon();
				g_holstering = true;
				g_holsterUntil = std::chrono::steady_clock::now() + std::chrono::seconds(2);
				return;
			}
		}
		const auto ready = g_lastAction + kActionGap;
		if (std::chrono::steady_clock::now() < ready) {
			// the raise / lower animation is still playing: an action now freezes the view (seen in game) -> queue it
			g_steps.push_back({ ready, std::size_t(g_open ? 1 : 0) });
		} else if (g_open) {
			if (!Perform(kBindings[1]) && !g_open) {
				Perform(kBindings[0]);  // the close failed on a stale state: the user wants the phone, open it
			}
		} else if (!g_preloaded) {
			QueuePreloadOpen();  // first open of this model shows nothing; open, close, open
			g_preloaded = true;
		} else {
			Perform(kBindings[0]);
		}
	}

	// one navigation key for the phone UI, and what its action asks for
	void Navigate(Ui::Key a_key)
	{
		Ui::Feedback fb = Ui::Feedback::kNone;
		const auto   act = Ui::Input(a_key, &fb);
		PlayFeedback(fb);
		switch (act) {
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
		case Ui::Action::kSummonContact: {
			const auto who = Ui::SelectedContact();
			if (who.formID && Stats::Summon(who.formID)) {
				Ui::SetCallResult(Tr("“收到，马上到。”"));
				Buzz(0.3f, 0.3f, 0.2f);
			}
			break;
		}
		case Ui::Action::kTakePhoto:
			// the photo comes from the same source as the viewfinder (the second view once it renders)
			Camera::RequestPhoto();
			Buzz(0.6f, 0.0f, 0.08f);  // shutter click
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

	// Runs on the game's task queue: poll the hotkeys, act on key-down edges.
	void Tick()
	{
		if (!g_ready.load()) {
			return;
		}
		// open key (Settings app; default F7) toggles the phone; F8 also puts it away
		if (KeyEdge(Ui::OpenKeyVk(Ui::GetSettings().openKey), g_openKeyDown) && g_steps.empty()) {
			TogglePhone();
		}
		if (KeyEdge(kBindings[1].vkey, g_down[1]) && g_open) {
			Perform(kBindings[1]);
		}

		// controller: the chosen gesture toggles the phone; D-pad / A / B navigate while it is out
		Gamepad::Update();
		// button prompts follow the device in use: any pad button or stick -> A / B; any key or mouse button -> keys
		if (Gamepad::Active()) {
			Ui::SetPadPrompts(true);
		} else if (KeyboardUsed()) {
			Ui::SetPadPrompts(false);
		}
		if (Gamepad::OpenGesture(Gamepad::Open(Ui::GetSettings().padOpen)) && g_steps.empty()) {
			TogglePhone();
			g_padNavHold = true;  // the gesture's buttons are still down: no navigation until they are released
		}
		if (g_padNavHold && Gamepad::Any() == 0) {
			g_padNavHold = false;
		}

		// phone navigation: arrows, Enter, Backspace (unbound in the game by default)
		if (g_open && g_steps.empty()) {
			constexpr std::pair<int, Ui::Key> nav[] = {
				{ 0x26, Ui::Key::kUp }, { 0x28, Ui::Key::kDown }, { 0x25, Ui::Key::kLeft },
				{ 0x27, Ui::Key::kRight }, { 0x0D, Ui::Key::kEnter }, { 0x08, Ui::Key::kBack },
			};
			for (std::size_t i = 0; i < std::size(nav); ++i) {
				if (KeyEdge(nav[i].first, g_navDown[i])) {
					Ui::SetPadPrompts(false);
					Navigate(nav[i].second);
				}
			}
			if (!g_padNavHold) {
				constexpr std::pair<std::uint32_t, Ui::Key> padNav[] = {
					{ Gamepad::kUp, Ui::Key::kUp }, { Gamepad::kDown, Ui::Key::kDown }, { Gamepad::kLeft, Ui::Key::kLeft },
					{ Gamepad::kRight, Ui::Key::kRight },
				};
				for (const auto& [button, key] : padNav) {
					if (Gamepad::Repeat(button)) {
						Ui::SetPadPrompts(true);
						Navigate(key);
					}
				}
				if (Gamepad::Pressed(Gamepad::kA)) {
					Ui::SetPadPrompts(true);
					Navigate(Ui::Key::kEnter);
				}
				if (Gamepad::Pressed(Gamepad::kB)) {
					Ui::SetPadPrompts(true);
					if (Ui::AtHome()) {
						TogglePhone();  // B on the home screen puts the phone away
					} else {
						Navigate(Ui::Key::kBack);
					}
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

		// holstering for the phone: open 0.25 s after the weapon is away (the hands are free by then); give up waiting
		// after 2 s and open anyway
		if (g_holstering) {
			static bool away = false;
			const auto  now = std::chrono::steady_clock::now();
			const int   state = Stats::WeaponState();
			if (state == 0 && !away) {
				away = true;
				g_holsterUntil = now + std::chrono::milliseconds(250);
			}
			if (now >= g_holsterUntil) {
				if (!away) REX::INFO("phone: weapon still out (state {}) after 2 s, opening anyway", state);
				away = false;
				g_holstering = false;
				TogglePhone(true);
			}
		}

		if (!g_steps.empty() && std::chrono::steady_clock::now() >= g_steps.front().at) {
			const auto b = g_steps.front().binding;
			g_steps.pop_front();
			if (b == kStepPhotoMode) {
				OpenPhotoMode();
			} else {
				Perform(kBindings[b]);
			}
		}

		// Camera page: the phone takes its camera pose, the HUD is hidden and the viewfinder crops the main frame
		// (the second render view, ViewProbe::Tick, is off: it drew characters and ships only partly)
		{
			int        cw = 0, ch = 0;
			const bool cameraPage = g_open && Ui::CameraTarget(cw, ch);
			CameraHoldTick(g_open, cameraPage);
			Tuner::Tick(g_open, g_devKeys, cameraPage);
			CameraPageTick(cameraPage);
		}

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
				case Ui::Info::kOutposts: Ui::SetInfo(info, Stats::Outposts()); break;
				default: break;
				}
			}
		} else {
			nextInfo = std::chrono::steady_clock::now();
		}

		// Contacts app: every 2 s; Scanner app: 4 times a second
		static auto nextContacts = std::chrono::steady_clock::now();
		if (g_open && Ui::WantsContacts()) {
			if (std::chrono::steady_clock::now() >= nextContacts) {
				nextContacts = std::chrono::steady_clock::now() + std::chrono::seconds(2);
				Ui::SetContacts(Stats::Contacts());
			}
		} else {
			nextContacts = std::chrono::steady_clock::now();
		}
		static auto nextRadar = std::chrono::steady_clock::now();
		if (float range = 0; g_open && Ui::WantsRadar(range) && std::chrono::steady_clock::now() >= nextRadar) {
			nextRadar = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
			Ui::SetRadar(Stats::Radar(range));
		}

		// photo "GPS": keep the current location for photo tags; tag photo mode photos as they appear
		static auto nextGeo = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		if (std::chrono::steady_clock::now() >= nextGeo) {
			nextGeo = std::chrono::steady_clock::now() + std::chrono::seconds(1);
			Gallery::SetCurrentGeo(Stats::CurrentGeo());
			static int geoTick = 0;
			if (++geoTick % 2 == 0) {
				for (const auto& [path, geo] : Gallery::PollNewPhotoModePhotos()) {
					const auto place = Gallery::PlaceTitle(geo);
					REX::INFO("gallery: photo mode photo tagged '{}' gps {} {} {}", place, geo.gps, geo.lat, geo.lon);
					Stats::HudNotify(place.empty() ? Tr("照片已保存（位置未知）") : Tr("照片已记录位置：") + place);
				}
			}
		}

		// Music app: lower the music while the player is in a conversation (DialogueMenu open), 4 checks a second.
		// UI::IsMenuOpen = ID 130475 (UI*, BSFixedString*), UI singleton ID 937580.
		static auto nextDuck = std::chrono::steady_clock::now();
		if (std::chrono::steady_clock::now() >= nextDuck) {
			nextDuck = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
			static const RE::BSFixedString kDialogueMenu{ "DialogueMenu" };
			auto*                          ui = RE::UI::GetSingleton();
			const bool                     talking = ui && ui->IsMenuOpen(kDialogueMenu);
			static bool                    wasTalking = false;
			if (talking != wasTalking) {
				wasTalking = talking;
				Music::SetDuck(talking ? kMusicDuck : 1.0f);
				REX::INFO("music: dialogue {} -> volume x{}", talking ? "started" : "ended", talking ? kMusicDuck : 1.0f);
			}
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
			if (silent && gameHours >= 0 && g_open) {
				// a save load (the clock jumped) drops the phone from the hands: drop our open state and give the
				// controls back, else the blocked controls would outlive the phone
				REX::INFO("phone: game clock jumped while the phone was out (load?); reset to closed");
				g_open = false;
				Screen::SetActive(false);
				Camera::SetActive(false);
				BlockGameInput(false);
			}
			gameHours = hours;
			bool buzz = false;
			for (auto& ch : Quests::PollChanges(silent)) {
				REX::INFO("message: {} | {}", ch.title, ch.text);
				Ui::PushMessage(std::move(ch.title), std::move(ch.text));
				buzz = true;
			}
			static int lastLevel = 0;
			const int  level = Stats::PlayerLevel();
			if (!silent && lastLevel > 0 && level == lastLevel + 1) {
				Ui::PushMessage(Tr("角色"), Tr("升级到 ") + std::to_string(level) + Tr(" 级"));
				buzz = true;
			}
			if (buzz) {
				Buzz(0.25f, 0.45f, 0.3f);  // new message (the game plays its own quest / level sounds)
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
				DetectGameLanguage();
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
	SFSE::Init(a_sfse, { .trampoline = true, .trampolineSize = 128 });
	REX::INFO("PhoneTest loaded; PerformAction ptr at Starfield.exe+0x{:X}", kPerformActionPtr.offset());
	Ui::Init([](const char* a_msg) { REX::INFO("{}", a_msg); });
	Screen::Install(kScreenWidth, kScreenHeight, [](const char* a_msg) { REX::INFO("{}", a_msg); });
	Screen::WatchViewTarget(ViewProbe::kTargetWidth, ViewProbe::kTargetHeight);
	Camera::Install([](const char* a_msg) { REX::INFO("{}", a_msg); });
	Tuner::Install();
	Stats::InstallPhotoModeHook();
	if (const auto* msg = SFSE::GetMessagingInterface(); !msg || !msg->RegisterListener(OnMessage)) {
		REX::ERROR("could not register for SFSE messages");
	}
	return true;
}
