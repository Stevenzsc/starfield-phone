#pragma once

#include <cstdint>

// Second render view research: read-only probe of the renderer's view / output tables (logs only).
namespace ViewProbe
{
	void Run();  // from the game's task thread

	// Step 2: create (once) our camera + view in the world scene and put it into / take it out of the idle
	// second main output. Logs what happens; engine calls are SEH-guarded.
	void SetSecondView(bool a_on);

	// Step 3: our camera + view rendered into our own output and texture (kTargetWidth x kTargetHeight), queued
	// every frame while a_on. screen.cpp finds the texture's D3D12 resource by this unique size.
	inline constexpr std::uint32_t kTargetWidth = 1308;
	inline constexpr std::uint32_t kTargetHeight = 909;
	void Tick(bool a_on);
}
