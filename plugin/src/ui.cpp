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
	};

	std::mutex g_lock;
	State      g_state;     // latest values (guarded)
	State      g_drawn;     // values the canvas was drawn with
	int        g_drawnMinute = -1;
	Canvas     g_canvas;

	void DrawHome(Canvas& c, const State& s)
	{
		FillBackground(c);
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
			RoundRect(c, cx - icon / 2, y, icon, icon, icon / 4, a.top, a.bottom);
			TextCentered(c, cx, y + icon / 2 - int(icon * 0.27f), a.glyph, icon * 0.5f, white);
			TextCentered(c, cx, y + icon + int(labelFont * 0.25f), a.label, labelFont, white, 0.95f);
		}

		// home indicator
		const int ind = int(W * 0.28f);
		RoundRect(c, c.w / 2 - ind / 2, c.h - int(H * 0.03f) - 4, ind, std::max(4, int(H * 0.009f)), 3, white, white, 0.8f);
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
		if (g_canvas.w != cw || g_canvas.h != ch || minute != g_drawnMinute || s.day != g_drawn.day) {
			g_canvas.Resize(cw, ch);
			DrawHome(g_canvas, s);
			g_drawnMinute = minute;
		}
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
				d[0] = std::uint8_t(a[0] + (b[0] - a[0]) * t);
				d[1] = std::uint8_t(a[1] + (b[1] - a[1]) * t);
				d[2] = std::uint8_t(a[2] + (b[2] - a[2]) * t);
				d[3] = 255;
			}
		}
		return true;
	}
}
