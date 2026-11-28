#pragma once

// Diagnostic: find which game code writes two addresses (hardware data breakpoints, once per session).
namespace BoneTrace
{
	using LogFn = void (*)(const char* a_message);

	void Arm(void* a_addr0, void* a_addr1, LogFn a_log);  // from the task thread
	void Poll();                                          // from the task thread: prints the result once
	bool Done();                                          // the result was printed (or never armed: false)
}
