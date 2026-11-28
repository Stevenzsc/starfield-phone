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
		kSettingsChanged,  // save GetSettings() to the ini
	};
	Action Input(Key a_key);  // from the game's task thread while the phone is open
	void   OnOpen();          // phone taken out: back to the home screen

	// ---- settings (Settings app; saved in PhoneTest.ini) ----
	struct Settings
	{
		int brightness = 100;  // percent, 30..100
		int openKey = 0;       // index into the open-key choices
	};
	Settings    GetSettings();
	void        SetSettings(const Settings& a_settings);
	int         OpenKeyCount();
	int         OpenKeyVk(int a_index);  // Windows virtual-key code
	const char* OpenKeyName(int a_index);

	// ---- Quests app ----
	struct QuestEntry
	{
		std::string              name;
		std::string              group;       // quest type shown as a section header ("主线", "派系", ...)
		std::vector<std::string> objectives;  // in-progress, displayed objectives
	};
	bool WantsQuests();                                 // the Quests page is open: feed it with SetQuests
	void SetQuests(std::vector<QuestEntry> a_quests);  // from the game's task thread

	// Write the current frame into an R8G8B8A8 texture-sized buffer (a_pitch bytes per row).
	// Returns false if the UI is not ready (no font).
	bool Render(std::uint8_t* a_dst, std::uint32_t a_pitch, std::uint32_t a_width, std::uint32_t a_height);
}
