// UiPreview: renders the phone UI (src/ui.cpp) outside the game and saves contact sheets of the page transitions
// as PNG files, so UI changes can be checked without starting Starfield. Built and run by CI.
//
//   UiPreview.exe <out dir>
//
// Each sheet is one transition: frames every 40 ms, left to right, top to bottom.

#include "ui.h"
#include "gallery.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#if __has_include(<stb_image_write.h>)
#	include <stb_image_write.h>
#else
#	include <stb/stb_image_write.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

// ---- Gallery stubs: 14 made-up photos over 3 days, mixed aspects; "decoding" paints a gradient ----
namespace
{
	struct FakePhoto
	{
		const wchar_t* name;
		int            w, h;
		const char*    caption;
	};
	constexpr FakePhoto kFake[] = {
		{ L"a", 1920, 1080, "2026-11-28 15:30" }, { L"b", 1080, 1920, "2026-11-28 15:10" }, { L"c", 1600, 1200, "2026-11-28 14:55" },
		{ L"d", 1920, 1080, "2026-11-28 14:20" }, { L"e", 1200, 1600, "2026-11-28 13:05" }, { L"f", 1920, 800, "2026-11-27 22:40" },
		{ L"g", 1080, 1920, "2026-11-27 21:15" }, { L"h", 1920, 1080, "2026-11-27 20:00" }, { L"i", 1500, 1500, "2026-11-27 18:30" },
		{ L"j", 1920, 1080, "2026-11-25 09:10" }, { L"k", 1080, 1920, "2026-11-25 08:45" }, { L"l", 1920, 1080, "2026-11-25 08:30" },
		{ L"m", 1600, 1200, "2026-11-25 08:00" }, { L"n", 1920, 1080, "2026-11-25 07:40" },
	};
	const FakePhoto* FakeOf(const std::wstring& a_path)
	{
		for (const auto& f : kFake)
			if (a_path == f.name) return &f;
		return nullptr;
	}
}

namespace Gallery
{
	bool Size(const std::wstring& a_path, int& a_w, int& a_h)
	{
		const auto* f = FakeOf(a_path);
		if (!f) return false;
		a_w = f->w, a_h = f->h;
		return true;
	}
	std::vector<Photo> ListFake()
	{
		std::vector<Photo> out;
		for (const auto& f : kFake) {
			Photo p;
			p.path = p.display = f.name;
			p.caption = f.caption;
			out.push_back(p);
		}
		return out;
	}
}

namespace Gallery
{
	void               SetCurrentGeo(const Geo&) {}
	Geo                CurrentGeo() { return {}; }
	std::string        PlaceTitle(const Geo&) { return {}; }
	std::string        GpsText(const Geo&) { return {}; }
	std::wstring       Dir() { return {}; }
	std::wstring       PhotoModeDir() { return {}; }
	std::vector<Photo> List() { return ListFake(); }
	void               TagCurrent(const std::wstring&) {}
	void               QueueShotGeo(const Geo&) {}
	std::vector<std::pair<std::wstring, Geo>> PollNewPhotoModePhotos() { return {}; }
	bool Load(const std::wstring& a_path, int a_maxW, int a_maxH, std::vector<std::uint8_t>& a_rgba, int& a_w, int& a_h)
	{
		const auto* f = FakeOf(a_path);
		if (!f) return false;
		const float s = std::min(float(a_maxW) / f->w, float(a_maxH) / f->h);
		a_w = std::max(1, int(f->w * s)), a_h = std::max(1, int(f->h * s));
		a_rgba.assign(std::size_t(a_w) * a_h * 4, 255);
		const int hue = int(a_path[0]) * 47;
		for (int y = 0; y < a_h; ++y)
			for (int x = 0; x < a_w; ++x) {
				auto* p = &a_rgba[(std::size_t(y) * a_w + x) * 4];
				p[0] = std::uint8_t((hue + x) % 256), p[1] = std::uint8_t((hue / 2 + y) % 256), p[2] = std::uint8_t(160 + (x + y) % 96);
			}
		std::this_thread::sleep_for(std::chrono::milliseconds(15));  // like a real decode
		return true;
	}
	bool Delete(const Photo&) { return false; }
}

namespace
{
	constexpr int kW = 378, kH = 666;  // half the in-game screen texture (756 x 1332)

	std::vector<std::uint8_t> Frame()
	{
		std::vector<std::uint8_t> px(std::size_t(kW) * kH * 4);
		Ui::Render(px.data(), kW * 4, kW, kH);
		return px;
	}

	// a_frames rendered every a_stepMs (real time: the UI animates on steady_clock), saved as one sheet
	void Sheet(const std::string& a_path, int a_frames, int a_stepMs, int a_cols = 6)
	{
		const int                 rows = (a_frames + a_cols - 1) / a_cols;
		const int                 gap = 6;
		const int                 sw = a_cols * kW + (a_cols - 1) * gap, sh = rows * kH + (rows - 1) * gap;
		std::vector<std::uint8_t> sheet(std::size_t(sw) * sh * 4, 40);
		auto                      next = std::chrono::steady_clock::now();
		for (int i = 0; i < a_frames; ++i) {
			std::this_thread::sleep_until(next);
			next += std::chrono::milliseconds(a_stepMs);
			const auto f = Frame();
			const int  ox = (i % a_cols) * (kW + gap), oy = (i / a_cols) * (kH + gap);
			for (int y = 0; y < kH; ++y) {
				std::memcpy(&sheet[(std::size_t(oy + y) * sw + ox) * 4], &f[std::size_t(y) * kW * 4], std::size_t(kW) * 4);
			}
		}
		for (std::size_t i = 3; i < sheet.size(); i += 4) sheet[i] = 255;
		stbi_write_png(a_path.c_str(), sw, sh, 4, sheet.data(), sw * 4);
		std::printf("wrote %s (animating at end: %d)\n", a_path.c_str(), int(Ui::Animating()));
	}

	void Settle()
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(700));
		Frame();
	}
}

int main(int argc, char** argv)
{
	const std::string out = argc > 1 ? argv[1] : ".";
	if (!Ui::Init([](const char* a_msg) { std::printf("%s\n", a_msg); })) {
		std::printf("no font\n");
		return 1;
	}
	Ui::SetGameTime(330, 10, 28, 14.5f);
	Ui::SetOrientation(0, false, 1332.0f / 756.0f);
	Ui::SetInsets(0.02f, 0.06f, 0.02f, 0.02f);

	Ui::OnOpen();
	Sheet(out + "/1-wake.png", 18, 40);

	Ui::Input(Ui::Key::kRight);  // select Quests: the selection box slides
	Sheet(out + "/0-select.png", 12, 40);
	Ui::Input(Ui::Key::kEnter);
	std::vector<Ui::QuestEntry> quests = {
		{ "古老的家园", "主线", { "前往新亚特兰蒂斯" }, { { "前往新亚特兰蒂斯", 1 } }, 1, true },
		{ "小规模入侵", "派系", { "清除地球舰队入侵" }, { { "清除地球舰队入侵", 1 }, { "返回", 2 } }, 2, false } };
	for (int i = 0; i < 6; ++i) {
		quests.push_back({ "支线任务 " + std::to_string(i + 1), "支线", { "和 NPC 对话", "收集样本" }, { { "和 NPC 对话", 1 } }, std::uint32_t(10 + i), false });
	}
	Ui::SetQuests(quests);
	Sheet(out + "/2-launch.png", 18, 40);

	Ui::Input(Ui::Key::kDown);  // the list scrolls one quest
	Sheet(out + "/2b-scroll.png", 12, 40);
	Ui::Input(Ui::Key::kUp);
	Settle();

	Ui::Input(Ui::Key::kEnter);  // quest details
	Sheet(out + "/3-push.png", 12, 40);

	Ui::Input(Ui::Key::kBack);
	Sheet(out + "/4-pop.png", 12, 40);

	Ui::Input(Ui::Key::kBack);  // home
	Sheet(out + "/5-close.png", 18, 40);

	for (int i = 0; i < 7; ++i) Ui::Input(Ui::Key::kRight);  // Music app
	Settle();
	Ui::Input(Ui::Key::kEnter);
	Sheet(out + "/8-music.png", 12, 40);
	Ui::Input(Ui::Key::kBack);
	Settle();
	for (int i = 0; i < 4; ++i) Ui::Input(Ui::Key::kRight);  // Settings app
	Ui::Input(Ui::Key::kEnter);
	Settle();
	Ui::Input(Ui::Key::kDown);
	Ui::Input(Ui::Key::kDown);
	Sheet(out + "/9-settings.png", 12, 40);
	Ui::Input(Ui::Key::kBack);
	Settle();

	// Gallery (Settings is selected: app 12, Gallery is app 7): album list -> grid -> move down -> viewer -> back
	for (int i = 0; i < 5; ++i) Ui::Input(Ui::Key::kLeft);
	Ui::Input(Ui::Key::kEnter);
	Settle();
	Ui::Input(Ui::Key::kEnter);  // "All photos" -> grid
	Sheet(out + "/a-grid-open.png", 12, 40);
	for (int i = 0; i < 4; ++i) Ui::Input(Ui::Key::kDown);
	Sheet(out + "/b-grid-scroll.png", 12, 40);
	Ui::Input(Ui::Key::kRight);
	Sheet(out + "/c-grid-right.png", 6, 40);
	Ui::Input(Ui::Key::kBack);
	Settle();
	Ui::Input(Ui::Key::kBack);
	Settle();

	Ui::PushMessage("小规模入侵：…", "新目标：前往…并清除地球舰队入侵");
	Sheet(out + "/6-banner-in.png", 18, 40);
	std::this_thread::sleep_for(std::chrono::milliseconds(3200));
	Sheet(out + "/7-banner-out.png", 12, 40);
	return 0;
}
