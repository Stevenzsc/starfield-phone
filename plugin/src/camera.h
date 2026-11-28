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
	void RequestPhoto();
}
