#pragma once

#include <cstdint>

// Writes an animated test pattern into the phone screen texture (found by its unique size) while active.
namespace Screen
{
	using LogFn = void (*)(const char* a_message);

	bool Install(std::uint32_t a_width, std::uint32_t a_height, LogFn a_log);
	void SetActive(bool a_active);
	void SetMarker(std::uint32_t a_count);  // draw this many white squares along the top edge
}
