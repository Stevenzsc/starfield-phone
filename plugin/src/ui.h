#pragma once

#include <cstdint>

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

	// Write the current frame into an R8G8B8A8 texture-sized buffer (a_pitch bytes per row).
	// Returns false if the UI is not ready (no font).
	bool Render(std::uint8_t* a_dst, std::uint32_t a_pitch, std::uint32_t a_width, std::uint32_t a_height);
}
