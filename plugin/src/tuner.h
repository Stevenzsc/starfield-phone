#pragma once

// Live placement tuner for the hand-held object (numpad, while the slate/phone is open).
// Captures the object when the engine attaches it (hook on the AnimObject attach call), and applies an offset
// transform to its geometry children.
namespace Tuner
{
	void Install();            // at plugin load (needs the SFSE trampoline)
	void Load();               // read the saved offset (PhoneTest_offset.txt)
	void Tick(bool a_isOpen);  // call often from the game's task queue
}
