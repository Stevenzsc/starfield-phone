#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Photos taken with the Camera app (PNG files in Documents\My Games\Starfield\PhoneCamera).
// This file does not use CommonLibSF.
namespace Gallery
{
	std::wstring              Dir();   // the photo folder (created if missing); empty if Documents is unknown
	std::vector<std::wstring> List();  // full paths of the photos, newest first

	// Decode a photo and scale it to fit inside a_maxW x a_maxH (aspect kept). RGBA8 out.
	bool Load(const std::wstring& a_path, int a_maxW, int a_maxH, std::vector<std::uint8_t>& a_rgba, int& a_w, int& a_h);

	bool Recycle(const std::wstring& a_path);  // move to the Recycle Bin

	// "2026-11-28 15:30" from a name like Photo_20261128_153012.png (empty if the name has another form)
	std::string Caption(const std::wstring& a_path);
}
