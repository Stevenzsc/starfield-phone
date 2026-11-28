#pragma once

// Live placement tuner for the hand-held object (numpad, while the slate/phone is open).
// Captures the object when the engine attaches it (hook on the AnimObject attach call), and applies an offset
// transform to its geometry children.
namespace Tuner
{
	void Install();            // at plugin load (needs the SFSE trampoline)
	void Load();               // read the saved offset (PhoneTest_offset.txt)
	// call often from the game's task queue. a_keys = dev keys (numpad tunes the active pose), a_camera = the Camera
	// page is shown (the phone takes its camera pose, PhoneTest_camera_offset.txt)
	void Tick(bool a_isOpen, bool a_keys, bool a_camera);

	// Camera page arm pose from runtime bone offsets (on, the fallback) or from the graph's PhoneCamera animation (off).
	// The phone's own camera offset applies either way.
	void SetArmOffsets(bool a_on);

	// Hide / show the 1st-person body and the phone (for photos). Returns false if the branch was not found.
	bool SetFirstPersonHidden(bool a_hide);
}
