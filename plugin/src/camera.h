#pragma once

#include <cstdint>

// Camera app: copies the centre of the game's back buffer at Present time, reads it back to the CPU and hands a
// down-scaled frame to the UI (Ui::SetCameraFrame) as a live viewfinder. RequestPhoto saves the next full-size crop
// as a PNG in Documents\My Games\Starfield\PhoneCamera.
//
// This file does not use CommonLibSF.
namespace Camera
{
	using LogFn = void (*)(const char* a_message);

	bool Install(LogFn a_log);   // hooks IDXGISwapChain::Present / Present1 (vtable of a dummy swap chain)
	void SetActive(bool a_open);  // the phone is out (capture only while the Camera page is shown, too)
	// Save a photo once a_waitPresents more frames were presented and a_waitMs passed (false if one is in flight).
	bool RequestPhoto(int a_waitPresents = 0, int a_waitMs = 0);
	bool PhotoCopied();  // the requested photo's frame has been copied

	// part of the screen the viewfinder and photos take (fractions, x0 y0 x1 y1); the largest rectangle with the
	// viewfinder's aspect inside it is used. Default: the whole screen.
	void SetCropRegion(float a_x0, float a_y0, float a_x1, float a_y1);
}
