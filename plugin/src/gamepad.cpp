// XInput controller polling for the phone.

#include "gamepad.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <xinput.h>

#include <chrono>

namespace
{
	using clock = std::chrono::steady_clock;
	using GetState_t = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);

	GetState_t g_getState = nullptr;
	bool       g_loadTried = false;
	int        g_pad = -1;  // connected pad index, -1 none
	clock::time_point g_nextPoll{}, g_nextScan{};

	std::uint32_t     g_now = 0, g_prev = 0, g_pressed = 0, g_repeat = 0;
	clock::time_point g_downAt[32]{};
	clock::time_point g_nextRepeat[32]{};
	bool              g_gestureLatched = false;

	constexpr auto kPollEvery = std::chrono::milliseconds(8);
	constexpr auto kScanEvery = std::chrono::seconds(1);  // XInputGetState on an empty slot is slow: look rarely
	constexpr auto kRepeatDelay = std::chrono::milliseconds(400);
	constexpr auto kRepeatEvery = std::chrono::milliseconds(110);
	constexpr float kHoldOpen = 0.5f;

	int Bit(std::uint32_t a_button)
	{
		for (int i = 0; i < 32; ++i) {
			if (a_button == (1u << i)) return i;
		}
		return 0;
	}

	bool Load()
	{
		if (g_loadTried) return g_getState != nullptr;
		g_loadTried = true;
		for (const wchar_t* dll : { L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll" }) {
			if (HMODULE m = LoadLibraryW(dll)) {
				g_getState = reinterpret_cast<GetState_t>(GetProcAddress(m, "XInputGetState"));
				if (g_getState) return true;
			}
		}
		return false;
	}

	std::uint32_t Read(int a_pad, bool& a_ok)
	{
		XINPUT_STATE st{};
		a_ok = g_getState(DWORD(a_pad), &st) == ERROR_SUCCESS;
		if (!a_ok) return 0;
		std::uint32_t b = st.Gamepad.wButtons & 0xF3FF;
		if (st.Gamepad.bLeftTrigger > 128) b |= Gamepad::kLT;
		if (st.Gamepad.bRightTrigger > 128) b |= Gamepad::kRT;
		return b;
	}
}

namespace Gamepad
{
	void Update()
	{
		g_pressed = 0;
		g_repeat = 0;
		const auto now = clock::now();
		if (now < g_nextPoll || !Load()) return;
		g_nextPoll = now + kPollEvery;
		std::uint32_t b = 0;
		bool          ok = false;
		if (g_pad >= 0) {
			b = Read(g_pad, ok);
			if (!ok) g_pad = -1;
		}
		if (g_pad < 0) {
			if (now < g_nextScan) {
				g_now = g_prev = 0;
				return;
			}
			g_nextScan = now + kScanEvery;
			for (int i = 0; i < 4 && g_pad < 0; ++i) {
				b = Read(i, ok);
				if (ok) g_pad = i;
			}
			if (g_pad < 0) {
				g_now = g_prev = 0;
				return;
			}
		}
		g_prev = g_now;
		g_now = b;
		g_pressed = g_now & ~g_prev;
		for (int i = 0; i < 32; ++i) {
			const std::uint32_t m = 1u << i;
			if (g_pressed & m) {
				g_downAt[i] = now;
				g_nextRepeat[i] = now + kRepeatDelay;
				g_repeat |= m;
			} else if ((g_now & m) && now >= g_nextRepeat[i]) {
				g_nextRepeat[i] = now + kRepeatEvery;
				g_repeat |= m;
			}
		}
		if (!(g_now & (kDown | kView | kLB | kRB | kLS | kRS))) {
			g_gestureLatched = false;
		}
	}

	bool          Connected() { return g_pad >= 0; }
	bool          Down(std::uint32_t a_buttons) { return a_buttons && (g_now & a_buttons) == a_buttons; }
	bool          Pressed(std::uint32_t a_button) { return (g_pressed & a_button) != 0; }
	bool          Repeat(std::uint32_t a_button) { return (g_repeat & a_button) != 0; }
	std::uint32_t Any() { return g_now; }

	float HeldFor(std::uint32_t a_button)
	{
		if (!(g_now & a_button)) return 0.0f;
		return std::chrono::duration<float>(clock::now() - g_downAt[Bit(a_button)]).count();
	}

	bool OpenGesture(Open a_how)
	{
		if (g_gestureLatched) return false;
		bool done = false;
		switch (a_how) {
		case Open::kHoldDpadDown: done = HeldFor(kDown) >= kHoldOpen; break;
		case Open::kHoldView: done = HeldFor(kView) >= kHoldOpen; break;
		case Open::kLbRb: done = Down(kLB | kRB); break;
		case Open::kSticks: done = Down(kLS | kRS); break;
		default: break;
		}
		if (done) g_gestureLatched = true;
		return done;
	}

	const char* OpenName(Open a_how)
	{
		switch (a_how) {
		case Open::kHoldDpadDown: return "长按十字键下";
		case Open::kHoldView: return "长按 View";
		case Open::kLbRb: return "LB + RB";
		case Open::kSticks: return "按下双摇杆";
		default: return "关";
		}
	}
}
