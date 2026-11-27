#pragma once

// Live placement tuner for the hand-held object (numpad, while the slate/phone is open).
// Finds the object's root node by name in the scene, and applies an offset transform to its geometry children.
namespace Tuner
{
	void Load();               // read the saved offset (PhoneTest_offset.txt)
	void Tick(bool a_isOpen);  // call often from the game's task queue
}
