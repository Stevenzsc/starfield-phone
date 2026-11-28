#pragma once

#include <cstdint>

struct ID3D12CommandQueue;
struct ID3D12Resource;

// Writes an animated test pattern into the phone screen texture (found by its unique size) while active.
namespace Screen
{
	using LogFn = void (*)(const char* a_message);

	bool Install(std::uint32_t a_width, std::uint32_t a_height, LogFn a_log);
	void SetActive(bool a_active);
	void SetMarker(std::uint32_t a_count);  // draw this many white squares along the top edge

	// The direct queue the game submits most command lists to (AddRef'd; nullptr if none seen yet).
	ID3D12CommandQueue* MainQueue();

	// Second-view render texture: catch the texture of this size (any format) when the game creates it.
	void            WatchViewTarget(std::uint32_t a_width, std::uint32_t a_height);
	void            ArmViewTarget();  // the next matching RGBA8 texture created is the view target
	ID3D12Resource* ViewTarget();  // AddRef'd; nullptr until created
}
