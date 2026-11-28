// Who writes a bone? Hardware data breakpoints (DR0 / DR1) on two addresses in every game thread; the first write
// to each is caught by a vectored exception handler, which records the writing instruction and its callers. The
// result is printed from the task thread (Poll), then all breakpoints are cleared.
//
// This file does not use CommonLibSF so that <windows.h> can be included normally.

#include "bonetrace.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <thread>

namespace
{
	BoneTrace::LogFn g_log = nullptr;

	void Log(const char* a_fmt, ...)
	{
		if (!g_log) {
			return;
		}
		char    buf[1024];
		va_list ap;
		va_start(ap, a_fmt);
		std::vsnprintf(buf, sizeof(buf), a_fmt, ap);
		va_end(ap);
		g_log(buf);
	}

	constexpr int kFrames = 16;

	struct Hit
	{
		std::atomic_bool done{ false };
		void*            rip = nullptr;
		void*            frames[kFrames]{};
		USHORT           count = 0;
		DWORD            thread = 0;
	};

	Hit              g_hits[2];
	std::atomic_bool g_armed{ false };
	std::atomic_bool g_reported{ false };
	PVOID            g_veh = nullptr;
	std::chrono::steady_clock::time_point g_deadline{};

	// DR7: L0 (bit 0) / L1 (bit 2), RW = 01 (write), LEN = 10 (8 bytes)
	constexpr DWORD64 kDr7Mask0 = (1ull << 0) | (1ull << 16) | (2ull << 18);
	constexpr DWORD64 kDr7Mask1 = (1ull << 2) | (1ull << 20) | (2ull << 22);
	constexpr DWORD64 kDr7Clear = ~((3ull << 0) | (3ull << 2) | (0xFull << 16) | (0xFull << 20));

	LONG CALLBACK Handler(EXCEPTION_POINTERS* a_ep)
	{
		if (a_ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) {
			return EXCEPTION_CONTINUE_SEARCH;
		}
		auto*         ctx = a_ep->ContextRecord;
		const DWORD64 dr6 = ctx->Dr6;
		if (!(dr6 & 3)) {
			return EXCEPTION_CONTINUE_SEARCH;  // a real single step, not ours
		}
		for (int i = 0; i < 2; ++i) {
			if (!(dr6 & (1ull << i))) {
				continue;
			}
			auto& h = g_hits[i];
			bool  expected = false;
			if (h.done.compare_exchange_strong(expected, true)) {
				h.rip = a_ep->ExceptionRecord->ExceptionAddress;
				h.thread = GetCurrentThreadId();
				h.count = CaptureStackBackTrace(0, kFrames, h.frames, nullptr);
			}
		}
		// this thread: stop watching both addresses
		ctx->Dr7 &= kDr7Clear;
		ctx->Dr6 = 0;
		return EXCEPTION_CONTINUE_EXECUTION;
	}

	void SetAllThreads(void* a_a0, void* a_a1, bool a_on)
	{
		const DWORD self = GetCurrentThreadId();
		HANDLE      snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
		if (snap == INVALID_HANDLE_VALUE) {
			return;
		}
		THREADENTRY32 te{};
		te.dwSize = sizeof(te);
		int n = 0;
		for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
			if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == self) {
				continue;
			}
			HANDLE th = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
			if (!th) {
				continue;
			}
			if (SuspendThread(th) != DWORD(-1)) {
				CONTEXT ctx{};
				ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
				if (GetThreadContext(th, &ctx)) {
					if (a_on) {
						ctx.Dr0 = reinterpret_cast<DWORD64>(a_a0);
						ctx.Dr1 = reinterpret_cast<DWORD64>(a_a1);
						ctx.Dr7 = (ctx.Dr7 & kDr7Clear) | kDr7Mask0 | kDr7Mask1;
					} else {
						ctx.Dr7 &= kDr7Clear;
					}
					ctx.Dr6 = 0;
					if (SetThreadContext(th, &ctx)) {
						++n;
					}
				}
				ResumeThread(th);
			}
			CloseHandle(th);
		}
		CloseHandle(snap);
		Log("bonetrace: watchpoints %s on %d threads", a_on ? "set" : "cleared", n);
	}

	void Report(const char* a_what, const Hit& a_h)
	{
		const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
		char       line[1024];
		int        len = std::snprintf(line, sizeof(line), "bonetrace: %s written at Starfield.exe+0x%llX (thread %lu); stack:", a_what,
				   static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(a_h.rip) - base), a_h.thread);
		for (USHORT i = 0; i < a_h.count && len < int(sizeof(line)) - 24; ++i) {
			const auto a = reinterpret_cast<std::uintptr_t>(a_h.frames[i]);
			if (a >= base && a < base + 0x10000000) {
				len += std::snprintf(line + len, sizeof(line) - len, " +0x%llX", static_cast<unsigned long long>(a - base));
			} else {
				len += std::snprintf(line + len, sizeof(line) - len, " %llX", static_cast<unsigned long long>(a));
			}
		}
		Log("%s", line);
	}
}

namespace BoneTrace
{
	void Arm(void* a_addr0, void* a_addr1, LogFn a_log)
	{
		bool expected = false;
		if (!g_armed.compare_exchange_strong(expected, true)) {
			return;  // once per session
		}
		g_log = a_log;
		if (!g_veh) {
			g_veh = AddVectoredExceptionHandler(1, &Handler);
		}
		g_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		Log("bonetrace: watching %p and %p", a_addr0, a_addr1);
		// from a helper thread, so the calling (main) thread is covered too
		std::thread([=] { SetAllThreads(a_addr0, a_addr1, true); }).join();
	}

	bool Done()
	{
		return g_reported.load();
	}

	void Poll()
	{
		if (!g_armed.load() || g_reported.load()) {
			return;
		}
		const bool both = g_hits[0].done.load() && g_hits[1].done.load();
		if (!both && std::chrono::steady_clock::now() < g_deadline) {
			return;
		}
		g_reported = true;
		std::thread([] { SetAllThreads(nullptr, nullptr, false); }).join();
		if (g_hits[0].done.load()) Report("local", g_hits[0]);
		else Log("bonetrace: local not written within 5 s");
		if (g_hits[1].done.load()) Report("world", g_hits[1]);
		else Log("bonetrace: world not written within 5 s");
	}
}
