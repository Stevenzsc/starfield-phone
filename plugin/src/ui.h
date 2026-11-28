#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Phone home screen, rendered on the CPU into the screen texture (see screen.cpp).
// Thread use: Set* from the game's task thread, Render from the render thread (internally locked).
namespace Ui
{
	using LogFn = void (*)(const char* a_message);

	// Load the font (a CJK system font) and prepare the renderer. Returns false if no font could be loaded.
	bool Init(LogFn a_log);

	// In-game calendar, shown in the status bar and the big clock. a_month is the game's 0-based month.
	void SetGameTime(std::uint32_t a_year, std::uint32_t a_month, std::uint32_t a_day, float a_hour);

	// How the logical (upright) screen maps onto the texture: rotation in degrees (0/90/180/270), a mirror, and the
	// visible screen's height / width (the UI is drawn at that aspect, then stretched over the whole texture).
	void SetOrientation(int a_rotateDeg, bool a_mirror, float a_aspect);

	// Safe area: fractions of the upright screen hidden by the slate's frame (left, top, right, bottom).
	// The background fills the whole screen; the UI is drawn inside the safe area.
	void SetInsets(float a_left, float a_top, float a_right, float a_bottom);

	// ---- navigation ----
	enum class Key
	{
		kUp,
		kDown,
		kLeft,
		kRight,
		kEnter,
		kBack
	};
	enum class Action
	{
		kNone,
		kOpenPhotoMode,    // camera app: put the phone away, then open the game's photo mode
		kTakePhoto,        // camera app: save the viewfinder picture (Camera::RequestPhoto)
		kSettingsChanged,  // save GetSettings() to the ini
		kToggleTrack,      // Quests page: track / untrack SelectedQuest()
		kSummonContact,    // call screen: bring SelectedContact() to the player
	};
	// what a key press did, for the button sound / rumble
	enum class Feedback
	{
		kNone,    // key not used here
		kMove,    // selection, scroll or value changed
		kOpen,    // a deeper page opened
		kBack,    // back to a shallower page
		kAction,  // Enter did something on this page
	};
	Action Input(Key a_key, Feedback* a_feedback = nullptr);  // from the game's task thread while the phone is open
	void   OnOpen();          // phone taken out: back to the home screen
	bool   Animating();       // a page transition or the banner is moving: write the screen at a higher rate

	// ---- settings (Settings app; saved in PhoneTest.ini) ----
	struct Settings
	{
		int  brightness = 100;   // percent, 30..100
		int  openKey = 0;        // index into the open-key choices
		bool sound = true;       // button sounds
		bool vibration = true;   // controller rumble
		int  musicVolume = 60;   // Music app, percent
	};
	Settings    GetSettings();
	void        SetSettings(const Settings& a_settings);
	int         OpenKeyCount();
	int         OpenKeyVk(int a_index);  // Windows virtual-key code
	const char* OpenKeyName(int a_index);

	// ---- Quests app ----
	struct Objective
	{
		std::string text;
		int         state = 1;  // 1 in progress, 2 completed, 3 failed
	};
	struct QuestEntry
	{
		std::string              name;
		std::string              group;       // quest type shown as a section header ("主线", "派系", ...)
		std::vector<std::string> objectives;  // in-progress, displayed objectives
		std::vector<Objective>   details;     // every displayed objective, in quest order (details page)
		std::uint32_t            formID = 0;
		bool                     tracked = false;  // Quest.IsActive: shown in the HUD / compass
	};
	// the quest under the selection on the Quests page (formID 0 if none)
	QuestEntry SelectedQuest();
	bool WantsQuests();                                 // the Quests page is open: feed it with SetQuests
	void SetQuests(std::vector<QuestEntry> a_quests);  // from the game's task thread

	// ---- info apps (Character, Ship, Star map): label / value rows, optional bar, section headers ----
	struct InfoRow
	{
		std::string label;
		std::string value;
		float       fraction = -1.0f;  // 0..1 draws a bar under the row
		bool        header = false;     // section header (label only)
	};
	enum class Info
	{
		kNone,
		kCharacter,
		kShip,
		kMap,
		kOutposts,
	};
	Info WantsInfo();                                       // which info page is shown (feed it with SetInfo)
	void SetInfo(Info a_page, std::vector<InfoRow> a_rows);  // empty = not available

	// ---- Contacts app (crew of the home ship) ----
	struct Contact
	{
		std::string   name;
		std::string   where;            // planet / location, or "附近 12 m"
		std::uint32_t formID = 0;
		float         distance = -1.0f;  // metres when loaded near the player, else -1
		bool          dead = false;
	};
	bool    WantsContacts();                          // Contacts page (or a call) is shown
	void    SetContacts(std::vector<Contact> a_list);  // from the game's task thread
	Contact SelectedContact();                        // formID 0 if none
	void    SetCallResult(std::string a_text);         // line shown on the call screen

	// ---- Scanner app ----
	struct Blip
	{
		std::string name;
		float       right = 0, forward = 0;  // metres relative to the player's heading
		float       distance = 0;
		int         kind = 0;  // 0 neutral, 1 hostile, 2 crew, 3 dead
	};
	bool WantsRadar(float& a_range);            // Scanner page shown; a_range = radius in metres
	void SetRadar(std::vector<Blip> a_blips);  // nearest first

	// ---- Messages app ----
	// add a message (stamped with the current game date / time); shows a banner while the phone is open
	void PushMessage(std::string a_title, std::string a_text);
	int  UnreadMessages();

	// ---- Camera app (fed from the Present hook, see camera.cpp) ----
	// true while the Camera page is shown; a_w x a_h = viewfinder size in canvas pixels (its aspect sets the crop)
	bool CameraTarget(int& a_w, int& a_h);
	void SetCameraFrame(std::vector<std::uint8_t> a_rgba, int a_w, int a_h);  // RGBA8, a_w x a_h
	void SetCameraStatus(std::string a_text);                                  // short toast ("已保存 ...")

	// Write the current frame into an R8G8B8A8 texture-sized buffer (a_pitch bytes per row).
	// Returns false if the UI is not ready (no font).
	bool Render(std::uint8_t* a_dst, std::uint32_t a_pitch, std::uint32_t a_width, std::uint32_t a_height);
}
