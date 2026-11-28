#pragma once

#include <cstdint>

// Controller input for the phone, read with XInput (the game reads the same pad; XInput allows several readers).
// Call Update() once per game tick; the rest reports what Update() saw. Game thread only.
// This file does not use CommonLibSF.
namespace Gamepad
{
	enum Button : std::uint32_t
	{
		kUp = 0x0001,     // D-pad
		kDown = 0x0002,
		kLeft = 0x0004,
		kRight = 0x0008,
		kStart = 0x0010,  // Menu
		kView = 0x0020,   // View / Back
		kLS = 0x0040,     // stick clicks
		kRS = 0x0080,
		kLB = 0x0100,
		kRB = 0x0200,
		kA = 0x1000,
		kB = 0x2000,
		kX = 0x4000,
		kY = 0x8000,
		kLT = 0x10000,  // triggers past half way
		kRT = 0x20000,
	};

	void Update();
	bool Connected();

	bool Down(std::uint32_t a_buttons);     // all of them held now
	bool Pressed(std::uint32_t a_button);   // went down in the last Update
	bool Repeat(std::uint32_t a_button);    // went down, or held long enough to auto-repeat (menu navigation)
	float HeldFor(std::uint32_t a_button);  // seconds held (0 when up)
	std::uint32_t Any();                    // every button held now

	// ways to take the phone out with the pad (Settings: 手柄打开)
	enum class Open
	{
		kOff,
		kHoldDpadDown,  // hold D-pad down 0.5 s
		kHoldView,      // hold View 0.5 s
		kLbRb,          // LB + RB together
		kSticks,        // LS + RS clicks together
		kCount
	};
	// true once when the chosen gesture completes (then not again until the buttons are released)
	bool OpenGesture(Open a_how);
	const char* OpenName(Open a_how);  // Chinese source text, pass through Tr()
}
