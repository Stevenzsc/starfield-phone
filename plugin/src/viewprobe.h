#pragma once

// Second render view research: read-only probe of the renderer's view / output tables (logs only).
namespace ViewProbe
{
	void Run();  // from the game's task thread

	// Step 2: create (once) our camera + view in the world scene and put it into / take it out of the idle
	// second main output. Logs what happens; engine calls are SEH-guarded.
	void SetSecondView(bool a_on);
}
