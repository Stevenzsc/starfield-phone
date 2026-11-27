// Phone home screen (P1): status bar, big clock with the in-game date, and an app grid.
// Rendered on the CPU with stb_truetype into a cached canvas; the canvas is rebuilt only when the shown minute or
// the orientation changes, and copied into the screen texture on every write.
//
// The font is loaded from the user's Windows fonts at runtime (no font ships with the mod).
// This file does not use CommonLibSF.

#include "ui.h"

#define STB_TRUETYPE_IMPLEMENTATION
#if __has_include(<stb_truetype.h>)
#	include <stb_truetype.h>
#else
#	include <stb/stb_truetype.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

namespace
{
	Ui::LogFn g_log = nullptr;

	void Log(const char* a_fmt, ...)
	{
		if (!g_log) {
			return;
		}
		char    buf[512];
		va_list ap;
		va_start(ap, a_fmt);
		std::vsnprintf(buf, sizeof(buf), a_fmt, ap);
		va_end(ap);
		g_log(buf);
	}

	struct Rgb
	{
		std::uint8_t r, g, b;
	};

	// ---- font ----
	std::vector<unsigned char> g_fontData;
	stbtt_fontinfo             g_font{};
	bool                       g_fontOk = false;

	bool LoadFont()
	{
		// first that loads wins; .ttc files hold several faces, face 0 is the regular one
		const char* candidates[] = {
			"C:\\Windows\\Fonts\\msyh.ttc",    // Microsoft YaHei
			"C:\\Windows\\Fonts\\simhei.ttf",  // SimHei
			"C:\\Windows\\Fonts\\simsun.ttc",  // SimSun
			"C:\\Windows\\Fonts\\segoeui.ttf", // no CJK, last resort
		};
		for (const char* path : candidates) {
			std::ifstream in(path, std::ios::binary);
			if (!in) {
				continue;
			}
			g_fontData.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
			const int offset = stbtt_GetFontOffsetForIndex(g_fontData.data(), 0);
			if (offset >= 0 && stbtt_InitFont(&g_font, g_fontData.data(), offset)) {
				Log("ui: font %s (%zu bytes)", path, g_fontData.size());
				return true;
			}
		}
		Log("ui: no usable font found; home screen disabled");
		return false;
	}

	// UTF-8 -> code points
	std::vector<int> Decode(const char* a_utf8)
	{
		std::vector<int> out;
		const auto*      s = reinterpret_cast<const unsigned char*>(a_utf8);
		while (*s) {
			int cp = 0, extra = 0;
			if (*s < 0x80) {
				cp = *s++;
			} else if ((*s >> 5) == 0x6) {
				cp = *s++ & 0x1F, extra = 1;
			} else if ((*s >> 4) == 0xE) {
				cp = *s++ & 0x0F, extra = 2;
			} else if ((*s >> 3) == 0x1E) {
				cp = *s++ & 0x07, extra = 3;
			} else {
				++s;
				continue;
			}
			while (extra-- > 0 && (*s & 0xC0) == 0x80) {
				cp = (cp << 6) | (*s++ & 0x3F);
			}
			out.push_back(cp);
		}
		return out;
	}

	// ---- canvas (logical, upright) ----
	struct Canvas
	{
		int                       w = 0, h = 0;
		std::vector<std::uint8_t> px;  // RGBA

		void Resize(int a_w, int a_h)
		{
			w = a_w;
			h = a_h;
			px.assign(std::size_t(w) * h * 4, 0);
		}
		void Blend(int x, int y, Rgb c, float a)
		{
			if (x < 0 || y < 0 || x >= w || y >= h || a <= 0) {
				return;
			}
			a = std::min(a, 1.0f);
			auto* p = &px[(std::size_t(y) * w + x) * 4];
			p[0] = std::uint8_t(p[0] + (c.r - p[0]) * a);
			p[1] = std::uint8_t(p[1] + (c.g - p[1]) * a);
			p[2] = std::uint8_t(p[2] + (c.b - p[2]) * a);
			p[3] = 255;
		}
	};

	Rgb Mix(Rgb a, Rgb b, float t)
	{
		return { std::uint8_t(a.r + (b.r - a.r) * t), std::uint8_t(a.g + (b.g - a.g) * t), std::uint8_t(a.b + (b.b - a.b) * t) };
	}

	void FillBackground(Canvas& c)
	{
		const Rgb top{ 10, 18, 38 }, bottom{ 34, 46, 82 };
		for (int y = 0; y < c.h; ++y) {
			const Rgb row = Mix(top, bottom, float(y) / float(c.h - 1));
			for (int x = 0; x < c.w; ++x) {
				auto* p = &c.px[(std::size_t(y) * c.w + x) * 4];
				p[0] = row.r, p[1] = row.g, p[2] = row.b, p[3] = 255;
			}
		}
		// a few fixed "stars"
		std::uint32_t seed = 1234567;
		for (int i = 0; i < 90; ++i) {
			seed = seed * 1103515245 + 12345;
			const int x = int((seed >> 8) % std::uint32_t(c.w));
			seed = seed * 1103515245 + 12345;
			const int y = int((seed >> 8) % std::uint32_t(c.h));
			c.Blend(x, y, { 255, 255, 255 }, 0.25f + float((seed >> 4) % 50) / 100.0f);
		}
	}

	// anti-aliased rounded rectangle
	void RoundRect(Canvas& c, int x0, int y0, int w, int h, int r, Rgb top, Rgb bottom, float alpha = 1.0f)
	{
		for (int y = y0; y < y0 + h; ++y) {
			const Rgb col = Mix(top, bottom, h > 1 ? float(y - y0) / float(h - 1) : 0.0f);
			for (int x = x0; x < x0 + w; ++x) {
				// distance outside the rounded corner
				const float cx = float(std::clamp(x, x0 + r, x0 + w - 1 - r));
				const float cy = float(std::clamp(y, y0 + r, y0 + h - 1 - r));
				const float d = std::hypot(float(x) - cx, float(y) - cy) - float(r);
				const float cover = std::clamp(0.5f - d, 0.0f, 1.0f);
				c.Blend(x, y, col, cover * alpha);
			}
		}
	}

	int TextWidth(const char* a_utf8, float a_px)
	{
		const float scale = stbtt_ScaleForPixelHeight(&g_font, a_px);
		const auto  cps = Decode(a_utf8);
		float       x = 0;
		for (std::size_t i = 0; i < cps.size(); ++i) {
			int adv = 0, lsb = 0;
			stbtt_GetCodepointHMetrics(&g_font, cps[i], &adv, &lsb);
			x += adv * scale;
			if (i + 1 < cps.size()) {
				x += scale * stbtt_GetCodepointKernAdvance(&g_font, cps[i], cps[i + 1]);
			}
		}
		return int(std::ceil(x));
	}

	// a_y = top of the text line
	void Text(Canvas& c, int a_x, int a_y, const char* a_utf8, float a_px, Rgb a_color, float a_alpha = 1.0f)
	{
		const float scale = stbtt_ScaleForPixelHeight(&g_font, a_px);
		int         ascent = 0, descent = 0, gap = 0;
		stbtt_GetFontVMetrics(&g_font, &ascent, &descent, &gap);
		const int baseline = a_y + int(std::round(ascent * scale));
		const auto cps = Decode(a_utf8);
		float      x = float(a_x);
		std::vector<unsigned char> bmp;
		for (std::size_t i = 0; i < cps.size(); ++i) {
			int adv = 0, lsb = 0;
			stbtt_GetCodepointHMetrics(&g_font, cps[i], &adv, &lsb);
			int bx0, by0, bx1, by1;
			stbtt_GetCodepointBitmapBox(&g_font, cps[i], scale, scale, &bx0, &by0, &bx1, &by1);
			const int gw = bx1 - bx0, gh = by1 - by0;
			if (gw > 0 && gh > 0) {
				bmp.assign(std::size_t(gw) * gh, 0);
				stbtt_MakeCodepointBitmap(&g_font, bmp.data(), gw, gh, gw, scale, scale, cps[i]);
				const int ox = int(std::round(x)) + bx0, oy = baseline + by0;
				for (int yy = 0; yy < gh; ++yy)
					for (int xx = 0; xx < gw; ++xx)
						c.Blend(ox + xx, oy + yy, a_color, a_alpha * bmp[std::size_t(yy) * gw + xx] / 255.0f);
			}
			x += adv * scale;
			if (i + 1 < cps.size()) {
				x += scale * stbtt_GetCodepointKernAdvance(&g_font, cps[i], cps[i + 1]);
			}
		}
	}

	std::vector<std::string> Wrap(const std::string& a_utf8, float a_px, int a_maxW)
	{
		std::vector<std::string> lines;
		std::string              line;
		const auto*              p = reinterpret_cast<const unsigned char*>(a_utf8.c_str());
		while (*p) {
			int len = 1;
			if ((*p >> 5) == 0x6) len = 2;
			else if ((*p >> 4) == 0xE) len = 3;
			else if ((*p >> 3) == 0x1E) len = 4;
			std::string ch(reinterpret_cast<const char*>(p), std::min<std::size_t>(len, std::strlen(reinterpret_cast<const char*>(p))));
			p += ch.size();
			if (!line.empty() && TextWidth((line + ch).c_str(), a_px) > a_maxW) {
				lines.push_back(line);
				line.clear();
				if (ch == " ") continue;
			}
			line += ch;
		}
		if (!line.empty()) lines.push_back(line);
		return lines;
	}

	void TextCentered(Canvas& c, int a_cx, int a_y, const char* a_utf8, float a_px, Rgb a_color, float a_alpha = 1.0f)
	{
		Text(c, a_cx - TextWidth(a_utf8, a_px) / 2, a_y, a_utf8, a_px, a_color, a_alpha);
	}

	// ---- home screen ----
	struct App
	{
		const char* label;
		const char* glyph;
		Rgb         top, bottom;
	};
	constexpr App kApps[] = {
		{ "联系人", "人", { 76, 217, 100 }, { 40, 160, 70 } },
		{ "任务", "务", { 255, 159, 10 }, { 220, 110, 0 } },
		{ "星图", "星", { 64, 156, 255 }, { 20, 100, 220 } },
		{ "前哨站", "哨", { 48, 200, 200 }, { 10, 140, 150 } },
		{ "飞船", "船", { 175, 82, 222 }, { 120, 50, 180 } },
		{ "相机", "相", { 142, 142, 147 }, { 90, 90, 98 } },
		{ "音乐", "乐", { 255, 69, 120 }, { 210, 30, 80 } },
		{ "设置", "设", { 100, 116, 139 }, { 60, 72, 92 } },
	};

	struct State
	{
		std::uint32_t year = 2330, month = 0, day = 1;
		float         hour = 12.0f;
		int           rotate = 0;
		bool          mirror = false;
		float         aspect = 1.0f;  // visible screen height / width (upright)
		// navigation
		int page = 0;      // Page
		int homeSel = 0;   // selected app on the home screen
		int setSel = 0;    // selected row in Settings
		int openedApp = 0; // app whose page is shown (for placeholder titles)
		// settings
		int brightness = 100;
		int openKey = 0;
		// quests
		int questScroll = 0;  // index of the first quest shown
		// safe area (fractions of the upright screen)
		float insetL = 0, insetT = 0, insetR = 0, insetB = 0;
	};

	enum Page
	{
		kHome,
		kSettingsPage,
		kQuestsPage,
		kCameraPage,
		kPlaceholderPage,
	};

	// app index -> page (order of kApps)
	int PageFor(int a_app)
	{
		switch (a_app) {
		case 1: return kQuestsPage;
		case 5: return kCameraPage;
		case 7: return kSettingsPage;
		default: return kPlaceholderPage;
		}
	}

	struct OpenKey
	{
		int         vk;
		const char* name;
	};
	// numpad keys are taken by the placement tuner; F5 / F9 are quicksave / quickload
	constexpr OpenKey kOpenKeys[] = {
		{ 0x76, "F7" }, { 0x75, "F6" }, { 0x79, "F10" }, { 0x2D, "Insert" }, { 0x24, "Home" }, { 0x23, "End" },
	};
	constexpr int kSettingsRows = 2;  // brightness, open key

	std::mutex g_lock;
	State      g_state;     // latest values (guarded)
	State      g_drawn;     // values the canvas was drawn with
	int        g_drawnMinute = -1;
	unsigned   g_version = 1;  // bumped on every navigation / settings change
	unsigned   g_drawnVersion = 0;
	std::vector<Ui::QuestEntry> g_quests;   // guarded by g_lock
	bool                        g_questsLoaded = false;
	Canvas     g_canvas;  // whole upright screen
	Canvas     g_inner;   // safe area

	void DrawHome(Canvas& c, const State& s)
	{
		const Rgb white{ 255, 255, 255 };
		const int hh = int(s.hour) % 24;
		const int mm = int((s.hour - std::floor(s.hour)) * 60.0f) % 60;
		char      clock[16];
		std::snprintf(clock, sizeof(clock), "%02d:%02d", hh, mm);

		// everything scales with the canvas (its aspect follows the physical screen, see screen_aspect)
		const float H = float(c.h), W = float(c.w);
		const int   pad = int(W * 0.045f);

		// status bar: time left, network + battery right
		const float sbFont = std::max(16.0f, H * 0.045f);
		const int   sbY = int(H * 0.018f);
		Text(c, pad, sbY, clock, sbFont, white);
		const int batH = int(sbFont * 0.7f), batW = int(batH * 2.2f);
		const int batX = c.w - pad - batW - 4, batY = sbY + int(sbFont * 0.2f);
		RoundRect(c, batX, batY, batW, batH, 4, { 230, 230, 230 }, { 200, 200, 200 });
		RoundRect(c, batX + 3, batY + 3, batW - 6, batH - 6, 2, { 52, 199, 89 }, { 40, 170, 75 });
		RoundRect(c, batX + batW, batY + batH / 4, 4, batH / 2, 1, { 230, 230, 230 }, { 230, 230, 230 });
		const int barsX = batX - int(sbFont * 2.0f);
		for (int i = 0; i < 4; ++i) {  // signal bars
			const int bh = int(batH * (0.35f + 0.22f * i));
			RoundRect(c, barsX + i * int(sbFont * 0.35f), batY + batH - bh, std::max(3, int(sbFont * 0.2f)), bh, 1, white, white);
		}
		const char* net = "星联";
		Text(c, barsX - TextWidth(net, sbFont * 0.85f) - int(sbFont * 0.4f), sbY + int(sbFont * 0.08f), net, sbFont * 0.85f, white, 0.9f);

		// big clock + date
		const float clockFont = std::min(H * 0.2f, W * 0.3f);
		const int   clockY = int(H * 0.1f);
		TextCentered(c, c.w / 2, clockY, clock, clockFont, white);
		char date[64];
		std::snprintf(date, sizeof(date), "%u年%u月%u日", s.year, s.month + 1, s.day);
		TextCentered(c, c.w / 2, clockY + int(clockFont * 1.05f), date, std::max(14.0f, H * 0.05f), { 210, 220, 240 });

		// app grid (lower part)
		const int   cols = W > H * 1.3f ? 8 : 4;
		const int   rows = int((std::size(kApps) + cols - 1) / cols);
		const int   cellW = (c.w - 2 * pad) / cols;
		const float labelFont = std::max(12.0f, H * 0.04f);
		const int   icon = int(std::min(cellW * 0.72f, H * 0.15f));
		const int   cellH = icon + int(labelFont * 1.7f);
		const int   gridY = c.h - rows * cellH - int(H * 0.06f);
		for (std::size_t i = 0; i < std::size(kApps); ++i) {
			const auto& a = kApps[i];
			const int   col = int(i) % cols, row = int(i) / cols;
			const int   cx = pad + col * cellW + cellW / 2;
			const int   y = gridY + row * cellH;
			if (int(i) == s.homeSel) {  // selection ring
				const int ring = std::max(4, icon / 12);
				RoundRect(c, cx - icon / 2 - ring, y - ring, icon + 2 * ring, icon + 2 * ring, icon / 4 + ring, white, white, 0.55f);
			}
			RoundRect(c, cx - icon / 2, y, icon, icon, icon / 4, a.top, a.bottom);
			TextCentered(c, cx, y + icon / 2 - int(icon * 0.27f), a.glyph, icon * 0.5f, white);
			TextCentered(c, cx, y + icon + int(labelFont * 0.25f), a.label, labelFont, white, 0.95f);
		}

		// home indicator
		const int ind = int(W * 0.28f);
		RoundRect(c, c.w / 2 - ind / 2, c.h - int(H * 0.03f) - 4, ind, std::max(4, int(H * 0.009f)), 3, white, white, 0.8f);
	}

	// header: back hint + title; returns the y where content starts
	int DrawHeader(Canvas& c, const char* a_title, Rgb a_accent)
	{
		const float H = float(c.h), W = float(c.w);
		const int   pad = int(W * 0.045f);
		const float f = std::max(16.0f, H * 0.045f);
		Text(c, pad, int(H * 0.03f), "‹ 退格返回", f * 0.8f, { 200, 210, 230 }, 0.9f);
		const float tf = std::max(20.0f, H * 0.075f);
		const int   ty = int(H * 0.1f);
		TextCentered(c, c.w / 2, ty, a_title, tf, { 255, 255, 255 });
		RoundRect(c, c.w / 2 - int(W * 0.08f), ty + int(tf * 1.2f), int(W * 0.16f), std::max(3, int(H * 0.008f)), 2, a_accent, a_accent);
		return ty + int(tf * 1.2f) + int(H * 0.05f);
	}

	void DrawHint(Canvas& c, const char* a_hint)
	{
		const float H = float(c.h);
		TextCentered(c, c.w / 2, c.h - int(H * 0.09f), a_hint, std::max(12.0f, H * 0.036f), { 190, 200, 220 }, 0.85f);
	}

	void DrawSettings(Canvas& c, const State& s)
	{
		const float H = float(c.h), W = float(c.w);
		int         y = DrawHeader(c, "设置", kApps[7].top);
		const int   pad = int(W * 0.06f);
		const int   rowH = int(H * 0.12f);
		const float f = std::max(14.0f, H * 0.048f);
		char        bright[16];
		std::snprintf(bright, sizeof(bright), "%d%%", s.brightness);
		const char* labels[kSettingsRows] = { "屏幕亮度", "打开手机" };
		const char* values[kSettingsRows] = { bright, kOpenKeys[s.openKey].name };
		for (int i = 0; i < kSettingsRows; ++i) {
			const bool sel = i == s.setSel;
			RoundRect(c, pad, y, c.w - 2 * pad, rowH - int(H * 0.02f), int(H * 0.025f), sel ? Rgb{ 90, 110, 150 } : Rgb{ 40, 52, 82 },
				sel ? Rgb{ 70, 90, 130 } : Rgb{ 34, 44, 70 }, sel ? 0.95f : 0.8f);
			const int ty = y + (rowH - int(H * 0.02f)) / 2 - int(f * 0.62f);
			Text(c, pad + int(W * 0.05f), ty, labels[i], f, { 255, 255, 255 });
			char val[40];
			std::snprintf(val, sizeof(val), sel ? "‹ %s ›" : "%s", values[i]);
			Text(c, c.w - pad - int(W * 0.05f) - TextWidth(val, f), ty, val, f, sel ? Rgb{ 255, 214, 10 } : Rgb{ 220, 230, 245 });
			y += rowH;
		}
		DrawHint(c, "↑↓ 选择   ←→ 调整");
	}

	void DrawCamera(Canvas& c, const State&)
	{
		const float H = float(c.h), W = float(c.w);
		const int   y = DrawHeader(c, "相机", kApps[5].top);
		const int   d = int(std::min(W * 0.38f, H * 0.3f));
		const int   cy = y + int(H * 0.08f);
		RoundRect(c, c.w / 2 - d / 2, cy, d, d, d / 2, { 255, 255, 255 }, { 230, 230, 230 });
		const int in = int(d * 0.8f);
		RoundRect(c, c.w / 2 - in / 2, cy + (d - in) / 2, in, in, in / 2, { 240, 240, 240 }, { 200, 200, 205 });
		const float f = std::max(14.0f, H * 0.046f);
		TextCentered(c, c.w / 2, cy + d + int(H * 0.06f), "回车：进入拍照模式", f, { 255, 255, 255 });
		TextCentered(c, c.w / 2, cy + d + int(H * 0.06f) + int(f * 1.5f), "手机会先收起", f * 0.8f, { 190, 200, 220 }, 0.9f);
	}

	void DrawQuests(Canvas& c, const State& s)
	{
		const float H = float(c.h), W = float(c.w);
		int         y = DrawHeader(c, "任务", kApps[1].top);
		const int   pad = int(W * 0.06f);
		const float nameF = std::max(14.0f, H * 0.047f);
		const float objF = std::max(12.0f, H * 0.038f);
		const int   bottom = c.h - int(H * 0.13f);
		if (!g_questsLoaded) {
			TextCentered(c, c.w / 2, y + int(H * 0.1f), "读取中…", nameF, { 255, 255, 255 });
			return;
		}
		if (g_quests.empty()) {
			TextCentered(c, c.w / 2, y + int(H * 0.1f), "没有进行中的任务", nameF, { 255, 255, 255 });
			return;
		}
		const int first = std::clamp(s.questScroll, 0, int(g_quests.size()) - 1);
		int       shown = 0;
		for (int i = first; i < int(g_quests.size()); ++i) {
			const auto& q = g_quests[i];
			const auto  nameLines = Wrap(q.name, nameF, c.w - 2 * pad);
			if (shown > 0 && y + int(nameF * 1.3f) * int(nameLines.size()) > bottom) {
				break;
			}
			for (const auto& l : nameLines) {
				Text(c, pad, y, l.c_str(), nameF, { 255, 214, 10 });
				y += int(nameF * 1.3f);
			}
			for (const auto& o : q.objectives) {
				const auto lines = Wrap(o, objF, c.w - 2 * pad - int(objF * 1.2f));
				for (std::size_t k = 0; k < lines.size(); ++k) {
					if (y + int(objF * 1.3f) > bottom) break;
					if (k == 0) Text(c, pad, y, "•", objF, { 220, 230, 245 });
					Text(c, pad + int(objF * 1.2f), y, lines[k].c_str(), objF, { 220, 230, 245 });
					y += int(objF * 1.3f);
				}
			}
			y += int(H * 0.025f);
			++shown;
			if (y > bottom) break;
		}
		char hint[64];
		std::snprintf(hint, sizeof(hint), "↑↓ 翻页   %d / %d", first + 1, int(g_quests.size()));
		DrawHint(c, hint);
	}

	void DrawPlaceholder(Canvas& c, const State& s)
	{
		const float H = float(c.h);
		const int   y = DrawHeader(c, kApps[s.openedApp].label, kApps[s.openedApp].top);
		TextCentered(c, c.w / 2, y + int(H * 0.12f), "即将推出", std::max(16.0f, H * 0.06f), { 255, 255, 255 });
	}

	void DrawPage(Canvas& c, const State& s)
	{
		if (s.page == kHome) {
			DrawHome(c, s);
			return;
		}
		switch (s.page) {
		case kSettingsPage: DrawSettings(c, s); break;
		case kCameraPage: DrawCamera(c, s); break;
		case kQuestsPage: DrawQuests(c, s); break;
		default: DrawPlaceholder(c, s); break;
		}
		// home indicator
		const int ind = int(c.w * 0.28f);
		RoundRect(c, c.w / 2 - ind / 2, c.h - int(c.h * 0.03f) - 4, ind, std::max(4, int(c.h * 0.009f)), 3, { 255, 255, 255 }, { 255, 255, 255 }, 0.8f);
	}
}

namespace Ui
{
	bool Init(LogFn a_log)
	{
		g_log = a_log;
		g_fontOk = LoadFont();
		return g_fontOk;
	}

	void SetGameTime(std::uint32_t a_year, std::uint32_t a_month, std::uint32_t a_day, float a_hour)
	{
		std::lock_guard lk(g_lock);
		// the GameYear global holds 330 for the year the game shows as 2330 (seen in game)
		g_state.year = a_year < 1000 ? a_year + 2000 : a_year;
		g_state.month = a_month;
		g_state.day = a_day;
		g_state.hour = a_hour;
	}

	void SetOrientation(int a_rotateDeg, bool a_mirror, float a_aspect)
	{
		std::lock_guard lk(g_lock);
		g_state.rotate = ((a_rotateDeg % 360) + 360) % 360 / 90 * 90;
		g_state.mirror = a_mirror;
		g_state.aspect = std::clamp(a_aspect, 0.2f, 5.0f);
	}

	Action Input(Key a_key)
	{
		std::lock_guard lk(g_lock);
		auto&           st = g_state;
		const int       n = int(std::size(kApps));
		const int       cols = 4;
		Action          act = Action::kNone;
		switch (st.page) {
		case kHome:
			switch (a_key) {
			case Key::kLeft: st.homeSel = (st.homeSel + n - 1) % n; break;
			case Key::kRight: st.homeSel = (st.homeSel + 1) % n; break;
			case Key::kUp: st.homeSel = (st.homeSel + n - cols) % n; break;
			case Key::kDown: st.homeSel = (st.homeSel + cols) % n; break;
			case Key::kEnter:
				st.openedApp = st.homeSel;
				st.page = PageFor(st.homeSel);
				st.setSel = 0;
				st.questScroll = 0;
				if (st.page == kQuestsPage) {
					g_questsLoaded = false;
				}
				break;
			default: return Action::kNone;
			}
			break;
		case kSettingsPage:
			switch (a_key) {
			case Key::kUp: st.setSel = (st.setSel + kSettingsRows - 1) % kSettingsRows; break;
			case Key::kDown: st.setSel = (st.setSel + 1) % kSettingsRows; break;
			case Key::kLeft:
			case Key::kRight: {
				const int dir = a_key == Key::kRight ? 1 : -1;
				if (st.setSel == 0) {
					st.brightness = std::clamp(st.brightness + dir * 10, 30, 100);
				} else {
					const int k = int(std::size(kOpenKeys));
					st.openKey = (st.openKey + dir + k) % k;
				}
				act = Action::kSettingsChanged;
				break;
			}
			case Key::kBack: st.page = kHome; break;
			default: return Action::kNone;
			}
			break;
		case kQuestsPage:
			switch (a_key) {
			case Key::kUp: st.questScroll = std::max(0, st.questScroll - 1); break;
			case Key::kDown: st.questScroll = std::min(std::max(0, int(g_quests.size()) - 1), st.questScroll + 1); break;
			case Key::kBack: st.page = kHome; break;
			default: return Action::kNone;
			}
			break;
		case kCameraPage:
			if (a_key == Key::kEnter) {
				act = Action::kOpenPhotoMode;
			} else if (a_key == Key::kBack) {
				st.page = kHome;
			} else {
				return Action::kNone;
			}
			break;
		default:
			if (a_key != Key::kBack) {
				return Action::kNone;
			}
			st.page = kHome;
			break;
		}
		++g_version;
		return act;
	}

	void OnOpen()
	{
		std::lock_guard lk(g_lock);
		g_state.page = kHome;
		++g_version;
	}

	void SetInsets(float a_left, float a_top, float a_right, float a_bottom)
	{
		std::lock_guard lk(g_lock);
		g_state.insetL = std::clamp(a_left, 0.0f, 0.45f);
		g_state.insetT = std::clamp(a_top, 0.0f, 0.45f);
		g_state.insetR = std::clamp(a_right, 0.0f, 0.45f);
		g_state.insetB = std::clamp(a_bottom, 0.0f, 0.45f);
		++g_version;
	}

	bool WantsQuests()
	{
		std::lock_guard lk(g_lock);
		return g_state.page == kQuestsPage;
	}

	void SetQuests(std::vector<QuestEntry> a_quests)
	{
		std::lock_guard lk(g_lock);
		g_quests = std::move(a_quests);
		g_questsLoaded = true;
		if (g_state.questScroll >= int(g_quests.size())) {
			g_state.questScroll = std::max(0, int(g_quests.size()) - 1);
		}
		++g_version;
	}

	Settings GetSettings()
	{
		std::lock_guard lk(g_lock);
		return { g_state.brightness, g_state.openKey };
	}

	void SetSettings(const Settings& a_settings)
	{
		std::lock_guard lk(g_lock);
		g_state.brightness = std::clamp(a_settings.brightness, 30, 100);
		g_state.openKey = std::clamp(a_settings.openKey, 0, int(std::size(kOpenKeys)) - 1);
		++g_version;
	}

	int OpenKeyCount() { return int(std::size(kOpenKeys)); }

	int OpenKeyVk(int a_index)
	{
		return kOpenKeys[std::clamp(a_index, 0, int(std::size(kOpenKeys)) - 1)].vk;
	}

	const char* OpenKeyName(int a_index)
	{
		return kOpenKeys[std::clamp(a_index, 0, int(std::size(kOpenKeys)) - 1)].name;
	}

	bool Render(std::uint8_t* a_dst, std::uint32_t a_pitch, std::uint32_t a_width, std::uint32_t a_height)
	{
		if (!g_fontOk) {
			return false;
		}
		std::lock_guard lk(g_lock);
		const State s = g_state;
		const bool  sideways = s.rotate == 90 || s.rotate == 270;
		// the upright view spans the texture's width (or height when sideways); its height follows the aspect
		const int   tw = sideways ? int(a_height) : int(a_width);  // upright width in texture pixels
		const int   th = sideways ? int(a_width) : int(a_height);  // upright height in texture pixels
		const int   cw = tw;
		const int   ch = std::max(16, int(std::lround(cw * s.aspect)));
		const int   minute = int(s.hour * 60.0f);
		if (g_canvas.w != cw || g_canvas.h != ch || minute != g_drawnMinute || s.day != g_drawn.day ||
			g_version != g_drawnVersion) {
			g_canvas.Resize(cw, ch);
			FillBackground(g_canvas);
			// draw the page on the safe area: start from the background under it, then paste it back
			const int ix = std::clamp(int(cw * s.insetL), 0, cw - 16), iy = std::clamp(int(ch * s.insetT), 0, ch - 16);
			const int iw = std::max(16, cw - ix - int(cw * s.insetR)), ih = std::max(16, ch - iy - int(ch * s.insetB));
			g_inner.Resize(iw, ih);
			for (int y = 0; y < ih; ++y) {
				std::memcpy(&g_inner.px[std::size_t(y) * iw * 4], &g_canvas.px[(std::size_t(iy + y) * cw + ix) * 4], std::size_t(iw) * 4);
			}
			DrawPage(g_inner, s);
			for (int y = 0; y < ih; ++y) {
				std::memcpy(&g_canvas.px[(std::size_t(iy + y) * cw + ix) * 4], &g_inner.px[std::size_t(y) * iw * 4], std::size_t(iw) * 4);
			}
			g_drawnMinute = minute;
			g_drawnVersion = g_version;
		}
		const float bright = s.brightness / 100.0f;
		g_drawn = s;
		const float sy = float(ch) / float(th);  // canvas rows per upright texture row
		for (std::uint32_t ty = 0; ty < a_height; ++ty) {
			auto* row = a_dst + std::size_t(ty) * a_pitch;
			for (std::uint32_t tx = 0; tx < a_width; ++tx) {
				// texture pixel -> upright position (ux, uy) in texture-pixel units
				int ux, uy;
				switch (s.rotate) {
				case 90: ux = int(ty), uy = int(a_width - 1 - tx); break;
				case 180: ux = int(a_width - 1 - tx), uy = int(a_height - 1 - ty); break;
				case 270: ux = int(a_height - 1 - ty), uy = int(tx); break;
				default: ux = int(tx), uy = int(ty); break;
				}
				if (s.mirror) {
					ux = tw - 1 - ux;
				}
				// vertical resample with linear filtering (horizontal is 1:1)
				const float fy = std::clamp((uy + 0.5f) * sy - 0.5f, 0.0f, float(ch - 1));
				const int   y0 = int(fy), y1 = std::min(y0 + 1, ch - 1);
				const float t = fy - float(y0);
				const auto* a = &g_canvas.px[(std::size_t(y0) * cw + ux) * 4];
				const auto* b = &g_canvas.px[(std::size_t(y1) * cw + ux) * 4];
				auto*       d = row + std::size_t(tx) * 4;
				d[0] = std::uint8_t((a[0] + (b[0] - a[0]) * t) * bright);
				d[1] = std::uint8_t((a[1] + (b[1] - a[1]) * t) * bright);
				d[2] = std::uint8_t((a[2] + (b[2] - a[2]) * t) * bright);
				d[3] = 255;
			}
		}
		return true;
	}
}
