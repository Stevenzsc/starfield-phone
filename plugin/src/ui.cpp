// Phone UI in the game's menu style: dark panels, thin rules, white selection, the four-colour stripe and button
// prompts. Status bar, home (clock, date, app tiles) and the app pages.
// Rendered on the CPU with stb_truetype into a cached canvas; the canvas is rebuilt only when the shown minute or
// the orientation changes, and copied into the screen texture on every write.
//
// The font is loaded from the user's Windows fonts at runtime (no font ships with the mod).
// This file does not use CommonLibSF.

#include "ui.h"
#include "gallery.h"

#define STB_TRUETYPE_IMPLEMENTATION
#if __has_include(<stb_truetype.h>)
#	include <stb_truetype.h>
#else
#	include <stb/stb_truetype.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
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

	// ---- fonts ----
	// Latin text and digits use Bahnschrift (DIN-like, close to the game's UI font), app icons use the Segoe Fluent /
	// MDL2 icon font, everything else (CJK) uses a Chinese system font. All are loaded from the user's Windows fonts.
	struct Face
	{
		std::vector<unsigned char> data;
		stbtt_fontinfo             info{};
		bool                       ok = false;
	};
	Face g_cjk, g_latin, g_icons;
	bool g_fontOk = false;

	bool LoadFace(Face& a_face, std::initializer_list<const char*> a_paths, const char* a_what)
	{
		for (const char* path : a_paths) {
			std::ifstream in(path, std::ios::binary);
			if (!in) {
				continue;
			}
			a_face.data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
			const int offset = stbtt_GetFontOffsetForIndex(a_face.data.data(), 0);
			if (offset >= 0 && stbtt_InitFont(&a_face.info, a_face.data.data(), offset)) {
				a_face.ok = true;
				Log("ui: %s font %s (%zu bytes)", a_what, path, a_face.data.size());
				return true;
			}
		}
		Log("ui: no %s font found", a_what);
		return false;
	}

	bool LoadFont()
	{
		// .ttc files hold several faces, face 0 is the regular one
		LoadFace(g_latin, { "C:\\Windows\\Fonts\\bahnschrift.ttf", "C:\\Windows\\Fonts\\segoeui.ttf" }, "latin");
		LoadFace(g_icons, { "C:\\Windows\\Fonts\\SegoeIcons.ttf", "C:\\Windows\\Fonts\\segmdl2.ttf" }, "icon");
		return LoadFace(g_cjk,
			{ "C:\\Windows\\Fonts\\msyh.ttc", "C:\\Windows\\Fonts\\simhei.ttf", "C:\\Windows\\Fonts\\simsun.ttc",
				"C:\\Windows\\Fonts\\segoeui.ttf" },
			"text");
	}

	// face and glyph for a code point: icons (private use area) -> latin (below U+2000) -> CJK
	const Face& FaceFor(int a_cp, int& a_glyph)
	{
		if (a_cp >= 0xE000 && a_cp <= 0xF8FF && g_icons.ok) {
			if ((a_glyph = stbtt_FindGlyphIndex(&g_icons.info, a_cp)) != 0) {
				return g_icons;
			}
		}
		if (a_cp < 0x2000 && g_latin.ok) {
			if ((a_glyph = stbtt_FindGlyphIndex(&g_latin.info, a_cp)) != 0) {
				return g_latin;
			}
		}
		a_glyph = stbtt_FindGlyphIndex(&g_cjk.info, a_cp);
		return g_cjk;
	}

	bool HasIcon(int a_cp)
	{
		return g_icons.ok && stbtt_FindGlyphIndex(&g_icons.info, a_cp) != 0;
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

	// ---- palette (Starfield menus: near-black panels, off-white text, white selection, the four-colour stripe) ----
	constexpr Rgb kBgTop{ 11, 14, 19 };
	constexpr Rgb kBgBottom{ 20, 25, 32 };
	constexpr Rgb kPanel{ 30, 36, 45 };
	constexpr Rgb kLine{ 78, 88, 100 };
	constexpr Rgb kText{ 236, 236, 230 };
	constexpr Rgb kDim{ 150, 158, 168 };
	constexpr Rgb kSelBg{ 236, 236, 230 };
	constexpr Rgb kSelText{ 14, 17, 22 };
	constexpr Rgb kStripe[4] = { { 214, 66, 44 }, { 234, 139, 44 }, { 243, 198, 62 }, { 48, 128, 194 } };
	constexpr Rgb kWarn{ 214, 66, 44 };
	constexpr Rgb kGold{ 243, 198, 62 };

	void FillBackground(Canvas& c)
	{
		for (int y = 0; y < c.h; ++y) {
			Rgb row = Mix(kBgTop, kBgBottom, float(y) / float(c.h - 1));
			if (y % 4 == 0) {  // faint scan lines
				row = Mix(row, kText, 0.025f);
			}
			for (int x = 0; x < c.w; ++x) {
				auto* p = &c.px[(std::size_t(y) * c.w + x) * 4];
				p[0] = row.r, p[1] = row.g, p[2] = row.b, p[3] = 255;
			}
		}
	}

	// sharp-cornered filled rectangle
	void Rect(Canvas& c, int x0, int y0, int w, int h, Rgb col, float alpha = 1.0f)
	{
		for (int y = std::max(0, y0); y < std::min(c.h, y0 + h); ++y)
			for (int x = std::max(0, x0); x < std::min(c.w, x0 + w); ++x)
				c.Blend(x, y, col, alpha);
	}

	// rectangle outline of thickness t
	void Frame(Canvas& c, int x0, int y0, int w, int h, int t, Rgb col, float alpha = 1.0f)
	{
		Rect(c, x0, y0, w, t, col, alpha);
		Rect(c, x0, y0 + h - t, w, t, col, alpha);
		Rect(c, x0, y0 + t, t, h - 2 * t, col, alpha);
		Rect(c, x0 + w - t, y0 + t, t, h - 2 * t, col, alpha);
	}

	// four short corner brackets (viewfinder / photo frame)
	void Brackets(Canvas& c, int x0, int y0, int w, int h, int len, int t, Rgb col)
	{
		Rect(c, x0, y0, len, t, col), Rect(c, x0, y0, t, len, col);
		Rect(c, x0 + w - len, y0, len, t, col), Rect(c, x0 + w - t, y0, t, len, col);
		Rect(c, x0, y0 + h - t, len, t, col), Rect(c, x0, y0 + h - len, t, len, col);
		Rect(c, x0 + w - len, y0 + h - t, len, t, col), Rect(c, x0 + w - t, y0 + h - len, t, len, col);
	}

	// the game's four-colour stripe badge: four vertical bars of width a_bw, gap a_gap
	void Stripes(Canvas& c, int x0, int y0, int a_bw, int h, int a_gap)
	{
		for (int i = 0; i < 4; ++i) {
			Rect(c, x0 + i * (a_bw + a_gap), y0, a_bw, h, kStripe[i]);
		}
	}

	// anti-aliased rounded rectangle (circles for the shutter)
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
		const auto cps = Decode(a_utf8);
		float      x = 0;
		for (std::size_t i = 0; i < cps.size(); ++i) {
			int         glyph = 0;
			const Face& f = FaceFor(cps[i], glyph);
			const float scale = stbtt_ScaleForPixelHeight(&f.info, a_px);
			int         adv = 0, lsb = 0;
			stbtt_GetGlyphHMetrics(&f.info, glyph, &adv, &lsb);
			x += adv * scale;
			if (i + 1 < cps.size()) {
				int         g2 = 0;
				const Face& f2 = FaceFor(cps[i + 1], g2);
				if (&f2 == &f) {
					x += scale * stbtt_GetGlyphKernAdvance(&f.info, glyph, g2);
				}
			}
		}
		return int(std::ceil(x));
	}

	// a_y = top of the text line; the baseline comes from the CJK face so mixed text lines up
	void Text(Canvas& c, int a_x, int a_y, const char* a_utf8, float a_px, Rgb a_color, float a_alpha = 1.0f)
	{
		int ascent = 0, descent = 0, gap = 0;
		stbtt_GetFontVMetrics(&g_cjk.info, &ascent, &descent, &gap);
		const int  baseline = a_y + int(std::round(ascent * stbtt_ScaleForPixelHeight(&g_cjk.info, a_px)));
		const auto cps = Decode(a_utf8);
		float      x = float(a_x);
		std::vector<unsigned char> bmp;
		for (std::size_t i = 0; i < cps.size(); ++i) {
			int         glyph = 0;
			const Face& f = FaceFor(cps[i], glyph);
			const float scale = stbtt_ScaleForPixelHeight(&f.info, a_px);
			int         adv = 0, lsb = 0;
			stbtt_GetGlyphHMetrics(&f.info, glyph, &adv, &lsb);
			int bx0, by0, bx1, by1;
			stbtt_GetGlyphBitmapBox(&f.info, glyph, scale, scale, &bx0, &by0, &bx1, &by1);
			const int gw = bx1 - bx0, gh = by1 - by0;
			if (gw > 0 && gh > 0) {
				bmp.assign(std::size_t(gw) * gh, 0);
				stbtt_MakeGlyphBitmap(&f.info, bmp.data(), gw, gh, gw, scale, scale, glyph);
				const int ox = int(std::round(x)) + bx0, oy = baseline + by0;
				for (int yy = 0; yy < gh; ++yy)
					for (int xx = 0; xx < gw; ++xx)
						c.Blend(ox + xx, oy + yy, a_color, a_alpha * bmp[std::size_t(yy) * gw + xx] / 255.0f);
			}
			x += adv * scale;
			if (i + 1 < cps.size()) {
				int         g2 = 0;
				const Face& f2 = FaceFor(cps[i + 1], g2);
				if (&f2 == &f) {
					x += scale * stbtt_GetGlyphKernAdvance(&f.info, glyph, g2);
				}
			}
		}
	}

	// one icon glyph centred on (a_cx, a_cy) by its own bitmap box
	void Icon(Canvas& c, int a_cx, int a_cy, int a_cp, float a_px, Rgb a_color)
	{
		const int glyph = stbtt_FindGlyphIndex(&g_icons.info, a_cp);
		if (!g_icons.ok || !glyph) {
			return;
		}
		const float scale = stbtt_ScaleForPixelHeight(&g_icons.info, a_px);
		int         bx0, by0, bx1, by1;
		stbtt_GetGlyphBitmapBox(&g_icons.info, glyph, scale, scale, &bx0, &by0, &bx1, &by1);
		const int gw = bx1 - bx0, gh = by1 - by0;
		if (gw <= 0 || gh <= 0) {
			return;
		}
		std::vector<unsigned char> bmp(std::size_t(gw) * gh, 0);
		stbtt_MakeGlyphBitmap(&g_icons.info, bmp.data(), gw, gh, gw, scale, scale, glyph);
		const int ox = a_cx - gw / 2, oy = a_cy - gh / 2;
		for (int yy = 0; yy < gh; ++yy)
			for (int xx = 0; xx < gw; ++xx)
				c.Blend(ox + xx, oy + yy, a_color, bmp[std::size_t(yy) * gw + xx] / 255.0f);
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
		const char* glyph;  // fallback when the icon font is missing
		int         icon;   // Segoe Fluent / MDL2 code point
	};
	constexpr App kApps[] = {
		{ "消息", "信", 0xE8BD },    // Message
		{ "任务", "务", 0xE7C1 },    // Flag
		{ "角色", "角", 0xE77B },    // Contact
		{ "飞船", "船", 0xE709 },    // Airplane
		{ "星图", "星", 0xE774 },    // Globe
		{ "日历", "历", 0xE787 },    // Calendar
		{ "相机", "相", 0xE722 },    // Camera
		{ "相册", "册", 0xE8B9 },    // Picture
		{ "联系人", "人", 0xE716 },  // People
		{ "前哨站", "哨", 0xE80F },  // Home
		{ "扫描仪", "扫", 0xE721 },  // Search
		{ "设置", "设", 0xE713 },    // Settings
	};
	enum AppId
	{
		kAppMessages,
		kAppQuests,
		kAppCharacter,
		kAppShip,
		kAppMap,
		kAppCalendar,
		kAppCamera,
		kAppGallery,
		kAppContacts,
		kAppOutposts,
		kAppScanner,
		kAppSettings,
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
		int questScroll = 0;  // selected quest (drawn first, highlighted)
		int detailScroll = 0; // first objective row on the quest details page
		// gallery
		int  galIndex = 0;         // shown photo in the open album (0 = newest)
		bool galConfirm = false;   // Enter pressed once: the next Enter deletes
		int  albumSel = 0;         // selected album on the album list
		int  galFilter = 0;        // 0 all, 1 phone, 2 photo mode
		// contacts / scanner
		int  contactSel = 0;
		int  radarRange = 1;       // index into kRadarRanges
		// info pages / messages
		int  infoScroll = 0;       // first row shown
		int  msgScroll = 0;        // first message shown
		bool msgConfirm = false;   // Enter pressed once: the next Enter clears all
		// safe area (fractions of the upright screen)
		float insetL = 0, insetT = 0, insetR = 0, insetB = 0;
	};

	enum Page
	{
		kHome,
		kSettingsPage,
		kQuestsPage,
		kCameraPage,
		kGalleryPage,
		kPlaceholderPage,
		kQuestDetailPage,
		kInfoPage,
		kCalendarPage,
		kMessagesPage,
		kPhotoPage,      // one album's photos (kGalleryPage is the album list)
		kContactsPage,
		kCallPage,
		kRadarPage,
	};

	constexpr float kRadarRanges[] = { 25.0f, 50.0f, 100.0f, 200.0f };

	// app index -> page (order of kApps)
	int PageFor(int a_app)
	{
		switch (a_app) {
		case kAppMessages: return kMessagesPage;
		case kAppQuests: return kQuestsPage;
		case kAppCharacter:
		case kAppShip:
		case kAppMap:
		case kAppOutposts: return kInfoPage;
		case kAppContacts: return kContactsPage;
		case kAppScanner: return kRadarPage;
		case kAppCalendar: return kCalendarPage;
		case kAppCamera: return kCameraPage;
		case kAppGallery: return kGalleryPage;
		case kAppSettings: return kSettingsPage;
		default: return kPlaceholderPage;
		}
	}

	Ui::Info InfoFor(int a_app)
	{
		switch (a_app) {
		case kAppCharacter: return Ui::Info::kCharacter;
		case kAppShip: return Ui::Info::kShip;
		case kAppMap: return Ui::Info::kMap;
		case kAppOutposts: return Ui::Info::kOutposts;
		default: return Ui::Info::kNone;
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
	// info pages (g_lock)
	Ui::Info                 g_infoPage = Ui::Info::kNone;  // page g_infoRows belongs to
	std::vector<Ui::InfoRow> g_infoRows;
	// messages (g_lock), newest first
	struct Message
	{
		std::string title, text, stamp;
	};
	std::vector<Message>                  g_messages;
	int                                   g_unread = 0;
	std::string                           g_bannerTitle, g_bannerText;
	std::chrono::steady_clock::time_point g_bannerUntil{};
	Canvas     g_canvas;  // whole upright screen
	Canvas     g_inner;   // safe area

	// camera: frame and toast come from the render thread's Present hook (own lock, taken after g_lock)
	std::mutex                g_camLock;
	std::vector<std::uint8_t> g_camFrame;  // RGBA8
	int                       g_camFrameW = 0, g_camFrameH = 0;
	std::string               g_camStatus;
	std::chrono::steady_clock::time_point g_camStatusUntil{}, g_camFlashUntil{};
	std::atomic_uint32_t      g_camSeq{ 0 };  // bumped by every new frame / toast
	std::uint32_t             g_drawnCamSeq = 0;
	std::atomic_bool          g_camOpen{ false };
	std::atomic_int           g_camW{ 0 }, g_camH{ 0 };

	// contacts / scanner (g_lock)
	std::vector<Ui::Contact> g_contacts;
	bool                     g_contactsLoaded = false;
	Ui::Contact              g_callee;       // the contact on the call screen
	std::string              g_callResult;  // "已呼叫，正在赶来" etc.
	std::vector<Ui::Blip>    g_blips;
	bool                     g_radarLoaded = false;

	// gallery: photo list and albums (g_lock); the decoded photo comes from a worker thread (g_camLock, bumps g_camSeq)
	struct Album
	{
		std::string      title;  // place, "全部照片" or "未知位置"
		std::string      sub;    // star system / count line
		std::vector<int> photos;  // indexes into g_photos, newest first
	};
	std::vector<Gallery::Photo> g_photos;
	std::vector<Album>          g_albums;
	std::vector<std::wstring>   g_galFiles;  // display paths of the open album (viewer)
	std::wstring              g_galImgPath;  // photo in g_galImg (or being loaded)
	std::vector<std::uint8_t> g_galImg;      // RGBA8; empty = could not be opened
	int                       g_galImgW = 0, g_galImgH = 0, g_galImgMaxW = 0, g_galImgMaxH = 0;
	bool                      g_galLoading = false;

	// ---- transitions (iOS-like) ----
	// A page change snapshots what is on screen (g_shown), draws the new page once into g_inner, and then only
	// composites the two bitmaps (scale / slide / dim) until the animation ends: no text is laid out per frame.
	// The status bar stays fixed; only the rows under it move.
	enum class Anim
	{
		kNone,
		kLaunch,  // home -> app: the page grows out of its tile, home zooms in and dims behind it
		kClose,   // app -> home: the page shrinks back into its tile
		kPush,    // deeper page slides in from the right, the old one moves left (parallax) and dims
		kPop,     // reverse of kPush
		kWake,    // phone taken out: the screen fades in from black while the content settles from a slight zoom
	};
	struct Transition
	{
		Anim                                  kind = Anim::kNone;
		int                                   tile = 0;  // app tile for kLaunch / kClose
		std::chrono::steady_clock::time_point start{};
		float                                 seconds = 0.4f;
	};
	struct IRect
	{
		int x = 0, y = 0, w = 0, h = 0;
	};
	Anim             g_pending = Anim::kNone;  // set by Input / OnOpen, started by the next Render (g_lock)
	int              g_pendingTile = 0;
	Transition       g_anim;
	std::atomic_bool g_animating{ false };  // read by the screen writer to raise its frame rate
	Canvas           g_from;    // the screen as it was when the transition started (safe-area size)
	Canvas           g_shown;   // last composited safe area (without the banner)
	Canvas           g_view;    // g_shown + banner
	Canvas           g_bg;      // full-screen background
	int              g_statusBottom = 0;  // safe-area row under the status bar of the last drawn page
	IRect            g_tiles[std::size(kApps)]{};  // home tiles of the last drawn home screen (safe-area coordinates)
	float            g_fade = 1.0f;       // screen brightness factor of the last composite (kWake)
	bool             g_bannerWasOn = false;

	constexpr float kBannerIn = 0.45f, kBannerOut = 0.3f;  // seconds

	// critically damped spring, normalised so it settles at t = 1 (the feel of UIKit's default spring)
	float Spring(float a_t)
	{
		if (a_t <= 0) return 0;
		if (a_t >= 1) return 1;
		const float w = 7.0f * a_t;
		return std::min(1.0f, (1.0f - (1.0f + w) * std::exp(-w)) / 0.99271f);
	}

	// under-damped spring (damping 0.75, ~3% overshoot) for the banner drop
	float SpringBouncy(float a_t)
	{
		if (a_t <= 0) return 0;
		if (a_t >= 1) return 1;
		constexpr float z = 0.75f, w = 10.0f;
		const float     wd = w * std::sqrt(1 - z * z);
		return 1.0f - std::exp(-z * w * a_t) * (std::cos(wd * a_t) + z / std::sqrt(1 - z * z) * std::sin(wd * a_t));
	}

	int Depth(int a_page);

	// copy rows [y0, y1) of a_src into a_dst shifted by a_ox, darkened by a_dim (0..1)
	void BlitShift(Canvas& a_dst, const Canvas& a_src, int a_ox, int a_y0, int a_y1, float a_dim)
	{
		const int x0 = std::max(0, a_ox), x1 = std::min(a_dst.w, a_src.w + a_ox);
		if (x1 <= x0) return;
		const int k = int((1.0f - std::clamp(a_dim, 0.0f, 1.0f)) * 256.0f);
		for (int y = std::max(0, a_y0); y < std::min({ a_y1, a_dst.h, a_src.h }); ++y) {
			auto*       d = &a_dst.px[(std::size_t(y) * a_dst.w + x0) * 4];
			const auto* s = &a_src.px[(std::size_t(y) * a_src.w + (x0 - a_ox)) * 4];
			if (k >= 256) {
				std::memcpy(d, s, std::size_t(x1 - x0) * 4);
				continue;
			}
			for (int i = 0; i < (x1 - x0) * 4; i += 4) {
				d[i] = std::uint8_t(s[i] * k >> 8), d[i + 1] = std::uint8_t(s[i + 1] * k >> 8), d[i + 2] = std::uint8_t(s[i + 2] * k >> 8);
				d[i + 3] = 255;
			}
		}
	}

	// draw rows [a_sy0, a_sy1) of a_src scaled into the rectangle (a_rx, a_ry, a_rw, a_rh) of a_dst, clipped to rows
	// [a_cy0, a_cy1); a_alpha blends over a_dst, a_dim darkens; bilinear or nearest sampling
	void BlitScaled(Canvas& a_dst, const Canvas& a_src, int a_sy0, int a_sy1, float a_rx, float a_ry, float a_rw, float a_rh,
		int a_cy0, int a_cy1, float a_alpha, float a_dim, bool a_bilinear)
	{
		if (a_rw < 1 || a_rh < 1 || a_alpha <= 0 || a_sy1 <= a_sy0 || a_src.w <= 0) return;
		const int   x0 = std::max(0, int(std::floor(a_rx))), x1 = std::min(a_dst.w, int(std::ceil(a_rx + a_rw)));
		const int   y0 = std::max({ 0, a_cy0, int(std::floor(a_ry)) }), y1 = std::min({ a_dst.h, a_cy1, int(std::ceil(a_ry + a_rh)) });
		const float kx = float(a_src.w) / a_rw, ky = float(a_sy1 - a_sy0) / a_rh;
		const float a = std::min(1.0f, a_alpha), keep = 1.0f - std::clamp(a_dim, 0.0f, 1.0f);
		static thread_local std::vector<int>   xi;
		static thread_local std::vector<float> xf;
		xi.resize(std::max(0, x1 - x0));
		xf.resize(xi.size());
		for (int x = x0; x < x1; ++x) {
			const float fx = std::clamp((x + 0.5f - a_rx) * kx - 0.5f, 0.0f, float(a_src.w - 1));
			xi[x - x0] = int(fx);
			xf[x - x0] = a_bilinear ? fx - float(int(fx)) : 0.0f;
		}
		for (int y = y0; y < y1; ++y) {
			const float fy = std::clamp(a_sy0 + (y + 0.5f - a_ry) * ky - 0.5f, float(a_sy0), float(a_sy1 - 1));
			const int   r0 = int(fy), r1 = std::min(r0 + 1, a_sy1 - 1);
			const float ty = a_bilinear ? fy - float(r0) : 0.0f;
			const auto* row0 = &a_src.px[std::size_t(r0) * a_src.w * 4];
			const auto* row1 = &a_src.px[std::size_t(r1) * a_src.w * 4];
			auto*       d = &a_dst.px[(std::size_t(y) * a_dst.w + x0) * 4];
			for (int x = x0; x < x1; ++x, d += 4) {
				const int   c0 = xi[x - x0], c1 = std::min(c0 + 1, a_src.w - 1);
				const float tx = xf[x - x0];
				for (int ch = 0; ch < 3; ++ch) {
					float v;
					if (a_bilinear) {
						const float top = row0[c0 * 4 + ch] + (row0[c1 * 4 + ch] - row0[c0 * 4 + ch]) * tx;
						const float bot = row1[c0 * 4 + ch] + (row1[c1 * 4 + ch] - row1[c0 * 4 + ch]) * tx;
						v = (top + (bot - top) * ty) * keep;
					} else {
						v = row0[c0 * 4 + ch] * keep;
					}
					d[ch] = std::uint8_t(a >= 1.0f ? v : d[ch] + (v - d[ch]) * a);
				}
				d[3] = 255;
			}
		}
	}

	// soft shadow left of column a_x (the edge of a page sliding over another)
	void ShadowLeft(Canvas& c, int a_x, int a_y0, int a_y1, int a_width, float a_strength)
	{
		for (int d = 0; d < a_width; ++d) {
			const int x = a_x - 1 - d;
			if (x < 0 || x >= c.w) continue;
			const float k = 1.0f - float(d) / float(a_width);
			const float a = a_strength * k * k;
			for (int y = std::max(0, a_y0); y < std::min(c.h, a_y1); ++y) c.Blend(x, y, { 0, 0, 0 }, a);
		}
	}

	// ---- shared chrome ----
	int Pad(const Canvas& c) { return int(c.w * 0.05f); }

	void ClockText(const State& s, char (&a_out)[16])
	{
		const int hh = int(s.hour) % 24;
		const int mm = int((s.hour - std::floor(s.hour)) * 60.0f) % 60;
		std::snprintf(a_out, sizeof(a_out), "%02d:%02d", hh, mm);
	}

	// status bar: time left, network + signal + battery right, thin rule below; returns the y under it
	int StatusBar(Canvas& c, const State& s)
	{
		const float H = float(c.h);
		const int   pad = Pad(c);
		const float f = std::max(14.0f, H * 0.04f);
		const int   y = int(H * 0.018f);
		char        clock[16];
		ClockText(s, clock);
		Text(c, pad, y, clock, f, kText);
		if (g_unread > 0) {  // unread messages: envelope + count after the clock
			const int mx = pad + TextWidth(clock, f) + int(f * 0.6f);
			const int my = y + int(f * 0.2f), mh = int(f * 0.66f), mw = int(mh * 1.35f);
			Rect(c, mx, my, mw, mh, kGold);
			char n[8];
			std::snprintf(n, sizeof(n), "%d", std::min(g_unread, 99));
			Text(c, mx + mw + int(f * 0.25f), y + int(f * 0.06f), n, f * 0.85f, kGold);
		}

		const int bh = int(f * 0.62f), bw = int(bh * 2.1f);
		const int bx = c.w - pad - bw - 3, by = y + int(f * 0.22f);
		Frame(c, bx, by, bw, bh, 2, kText);
		Rect(c, bx + 4, by + 4, int((bw - 8) * 0.8f), bh - 8, kText);
		Rect(c, bx + bw, by + bh / 4, 3, bh / 2, kText);
		const int sx = bx - int(f * 1.7f);
		for (int i = 0; i < 4; ++i) {
			const int h = int(bh * (0.3f + 0.233f * i));
			Rect(c, sx + i * int(f * 0.3f), by + bh - h, std::max(2, int(f * 0.17f)), h, kText);
		}
		const char* net = "星联";
		Text(c, sx - TextWidth(net, f * 0.85f) - int(f * 0.45f), y + int(f * 0.06f), net, f * 0.85f, kDim);
		const int line = y + int(f * 1.45f);
		Rect(c, pad, line, c.w - 2 * pad, 1, kLine);
		return line + 1;
	}

	// page title with the stripe badge and a rule; optional right-aligned note; returns where content starts
	int Header(Canvas& c, int a_y, const char* a_title, const char* a_note = nullptr)
	{
		const float H = float(c.h);
		const int   pad = Pad(c);
		const float tf = std::max(18.0f, H * 0.062f);
		const int   y = a_y + int(H * 0.03f);
		const int   bw = std::max(3, int(H * 0.009f)), gap = std::max(2, bw / 2);
		Stripes(c, pad, y + int(tf * 0.18f), bw, int(tf * 0.95f), gap);
		Text(c, pad + 4 * (bw + gap) + int(tf * 0.35f), y, a_title, tf, kText);
		if (a_note) {
			const float nf = tf * 0.55f;
			Text(c, c.w - pad - TextWidth(a_note, nf), y + int(tf * 0.42f), a_note, nf, kDim);
		}
		const int line = y + int(tf * 1.45f);
		Rect(c, pad, line, c.w - 2 * pad, 1, kLine);
		return line + int(H * 0.025f);
	}

	struct Prompt
	{
		const char* key;
		const char* label;
	};

	// button prompts along the bottom, right-aligned like the game's ([Enter] Select); returns the bar's top y
	int Prompts(Canvas& c, std::initializer_list<Prompt> a_items)
	{
		const float H = float(c.h);
		const int   pad = Pad(c);
		const float f = std::max(12.0f, H * 0.034f);
		const int   barH = int(H * 0.08f);
		const int   top = c.h - barH;
		Rect(c, pad, top, c.w - 2 * pad, 1, kLine);
		const int boxH = int(f * 1.45f);
		const int by = top + (barH - boxH) / 2;
		int       x = c.w - pad;
		for (auto it = std::rbegin(a_items); it != std::rend(a_items); ++it) {
			const int lw = TextWidth(it->label, f);
			x -= lw;
			Text(c, x, by + int(f * 0.2f), it->label, f, kText);
			const int kw = TextWidth(it->key, f * 0.85f) + int(f * 0.8f);
			x -= kw + int(f * 0.4f);
			Frame(c, x, by, kw, boxH, 2, kText, 0.9f);
			Text(c, x + int(f * 0.4f), by + int(f * 0.28f), it->key, f * 0.85f, kText);
			x -= int(f * 1.1f);
		}
		return top;
	}

	void DrawHome(Canvas& c, const State& s, int a_y)
	{
		const float H = float(c.h), W = float(c.w);
		const int   pad = Pad(c);
		const int   bottom = Prompts(c, { { "←→", "选择" }, { "Enter", "打开" } });

		// big clock, date, stripe badge
		char clock[16];
		ClockText(s, clock);
		const float clockF = std::min(H * 0.17f, W * 0.24f);
		const int   cy = a_y + int(H * 0.02f);
		Text(c, pad, cy, clock, clockF, kText);
		const int bw = std::max(4, int(H * 0.014f)), gap = std::max(2, bw / 2);
		Stripes(c, c.w - pad - 4 * bw - 3 * gap, cy + int(clockF * 0.28f), bw, int(clockF * 0.72f), gap);
		char date[64];
		std::snprintf(date, sizeof(date), "%u年%u月%u日", s.year, s.month + 1, s.day);
		const float dateF = std::max(13.0f, H * 0.042f);
		const int   dy = cy + int(clockF * 1.08f);
		Text(c, pad, dy, date, dateF, kDim);
		const int rule = dy + int(dateF * 1.6f);
		Rect(c, pad, rule, c.w - 2 * pad, 1, kLine);

		// app tiles: 4 x 2, sharp corners; the selection is filled white with dark content
		const int   cols = W > H * 1.3f ? 8 : 4;
		const int   rows = int((std::size(kApps) + cols - 1) / cols);
		const float labelF = std::max(11.0f, H * 0.034f);
		const int   labelH = int(labelF * 1.7f);
		const int   gapX = int(W * 0.025f), gapY = int(H * 0.02f);
		const int   gridTop = rule + int(H * 0.03f);
		const int   byWidth = (c.w - 2 * pad - (cols - 1) * gapX) / cols;
		const int   byHeight = (bottom - gridTop - int(H * 0.02f) - rows * labelH - (rows - 1) * gapY) / rows;
		const int   tile = std::max(16, std::min(byWidth, byHeight));
		const int   gridW = cols * tile + (cols - 1) * gapX;
		const int   gx = (c.w - gridW) / 2;
		for (std::size_t i = 0; i < std::size(kApps); ++i) {
			const auto& a = kApps[i];
			const int   col = int(i) % cols, row = int(i) / cols;
			const int   x = gx + col * (tile + gapX);
			const int   y = gridTop + row * (tile + labelH + gapY);
			const bool  sel = int(i) == s.homeSel;
			g_tiles[i] = { x, y, tile, tile };
			if (sel) {
				Rect(c, x, y, tile, tile, kSelBg);
			} else {
				Rect(c, x, y, tile, tile, kPanel, 0.9f);
				Frame(c, x, y, tile, tile, 1, kLine);
			}
			const Rgb fg = sel ? kSelText : kText;
			if (HasIcon(a.icon)) {
				Icon(c, x + tile / 2, y + tile / 2, a.icon, tile * 0.42f, fg);
			} else {
				TextCentered(c, x + tile / 2, y + tile / 2 - int(tile * 0.24f), a.glyph, tile * 0.42f, fg);
			}
			TextCentered(c, x + tile / 2, y + tile + int(labelF * 0.3f), a.label, labelF, sel ? kText : kDim);
			if (i == kAppMessages && g_unread > 0) {  // unread badge on the Messages tile
				char n[8];
				std::snprintf(n, sizeof(n), "%d", std::min(g_unread, 99));
				const float bf = std::max(10.0f, tile * 0.22f);
				const int   bw = std::max(int(bf * 1.3f), TextWidth(n, bf) + int(bf * 0.6f)), bh = int(bf * 1.3f);
				Rect(c, x + tile - bw + int(bf * 0.3f), y - int(bf * 0.3f), bw, bh, kWarn);
				TextCentered(c, x + tile - bw / 2 + int(bf * 0.3f), y - int(bf * 0.3f) + int(bf * 0.1f), n, bf, kText);
			}
		}
	}

	void DrawSettings(Canvas& c, const State& s, int a_y)
	{
		const float H = float(c.h), W = float(c.w);
		const int   pad = Pad(c);
		Prompts(c, { { "↑↓", "选择" }, { "←→", "调整" }, { "退格", "返回" } });
		int         y = Header(c, a_y, "设置");
		const int   rowH = int(H * 0.095f);
		const float f = std::max(13.0f, H * 0.044f);
		char        bright[16];
		std::snprintf(bright, sizeof(bright), "%d%%", s.brightness);
		const char* labels[kSettingsRows] = { "屏幕亮度", "打开手机" };
		const char* values[kSettingsRows] = { bright, kOpenKeys[s.openKey].name };
		for (int i = 0; i < kSettingsRows; ++i) {
			const bool sel = i == s.setSel;
			const int  ty = y + rowH / 2 - int(f * 0.62f);
			if (sel) {
				Rect(c, pad, y, c.w - 2 * pad, rowH - 2, kSelBg);
			} else {
				Rect(c, pad, y + rowH - 2, c.w - 2 * pad, 1, kLine, 0.7f);
			}
			const Rgb fg = sel ? kSelText : kText;
			Text(c, pad + int(W * 0.03f), ty, labels[i], f, fg);
			char val[40];
			std::snprintf(val, sizeof(val), sel ? "‹  %s  ›" : "%s", values[i]);
			Text(c, c.w - pad - int(W * 0.03f) - TextWidth(val, f), ty, val, f, sel ? kSelText : kDim);
			y += rowH;
		}
	}

	// live viewfinder (frames from camera.cpp), shutter, toast after a photo
	void DrawCamera(Canvas& c, const State&, int a_y)
	{
		const float H = float(c.h), W = float(c.w);
		const float f = std::max(12.0f, H * 0.034f);
		const int   bottom = Prompts(c, { { "Enter", "拍照" }, { "↑", "拍照模式" }, { "退格", "返回" } });
		const int   d = int(std::min(W * 0.16f, H * 0.1f));  // shutter size
		const int   vx = int(W * 0.03f), vy = a_y + int(H * 0.02f);
		const int   vw = c.w - 2 * vx, vh = bottom - vy - d - int(H * 0.04f);
		g_camW = vw;
		g_camH = vh;
		const auto now = std::chrono::steady_clock::now();
		{
			std::lock_guard lk(g_camLock);
			if (g_camFrameW == vw && g_camFrameH == vh && !g_camFrame.empty()) {
				for (int y = 0; y < vh; ++y) {
					if (vy + y >= c.h) break;
					std::memcpy(&c.px[(std::size_t(vy + y) * c.w + vx) * 4], &g_camFrame[std::size_t(y) * vw * 4], std::size_t(vw) * 4);
				}
			} else {
				Rect(c, vx, vy, vw, vh, { 0, 0, 0 });
				TextCentered(c, c.w / 2, vy + vh / 2 - int(f), "取景中…", f * 1.2f, kDim);
			}
			if (now < g_camFlashUntil) {
				Rect(c, vx, vy, vw, vh, { 255, 255, 255 }, 0.7f);
			}
			const int m = int(W * 0.02f);
			Brackets(c, vx + m, vy + m, vw - 2 * m, vh - 2 * m, int(std::min(vw, vh) * 0.09f), 3, kText);
			if (now < g_camStatusUntil && !g_camStatus.empty()) {
				const int tw = TextWidth(g_camStatus.c_str(), f) + int(f * 1.6f);
				const int ty = vy + vh - int(f * 3.0f);
				Rect(c, c.w / 2 - tw / 2, ty, tw, int(f * 1.8f), kBgTop, 0.8f);
				Rect(c, c.w / 2 - tw / 2, ty, 3, int(f * 1.8f), kStripe[1]);
				TextCentered(c, c.w / 2, ty + int(f * 0.3f), g_camStatus.c_str(), f, kText);
			}
		}
		// shutter: white ring around a white disc
		const int sy = vy + vh + (bottom - vy - vh - d) / 2;
		RoundRect(c, c.w / 2 - d / 2, sy, d, d, d / 2, kText, kText);
		const int ring = std::max(3, d / 14);
		RoundRect(c, c.w / 2 - d / 2 + ring, sy + ring, d - 2 * ring, d - 2 * ring, d / 2 - ring, kBgBottom, kBgBottom);
		const int in = d - 4 * ring;
		RoundRect(c, c.w / 2 - in / 2, sy + (d - in) / 2, in, in, in / 2, kText, kText);
	}

	std::string Fit(const std::string& a_text, float a_px, int a_maxW);

	const char* kGalFilters[] = { "全部", "手机", "拍照模式" };

	// (g_lock held) group g_photos into albums by place: "全部照片", then places (newest first), then "未知位置"
	void BuildAlbums(int a_filter)
	{
		g_albums.clear();
		Album              all{ "全部照片", "", {} };
		Album              unknown{ "未知位置", "没有位置记录的照片", {} };
		std::vector<Album> places;
		for (int i = 0; i < int(g_photos.size()); ++i) {
			const auto& p = g_photos[i];
			if ((a_filter == 1 && p.source != Gallery::Source::kPhone) || (a_filter == 2 && p.source != Gallery::Source::kPhotoMode)) {
				continue;
			}
			all.photos.push_back(i);
			const auto title = p.tagged ? Gallery::PlaceTitle(p.geo) : std::string{};
			if (title.empty()) {
				unknown.photos.push_back(i);
				continue;
			}
			auto it = std::find_if(places.begin(), places.end(), [&](const Album& a) { return a.title == title; });
			if (it == places.end()) {
				places.push_back({ title, p.geo.system, {} });
				it = places.end() - 1;
			}
			it->photos.push_back(i);
		}
		if (all.photos.empty()) {
			return;
		}
		g_albums.push_back(std::move(all));
		for (auto& a : places) g_albums.push_back(std::move(a));
		if (!unknown.photos.empty()) g_albums.push_back(std::move(unknown));
	}

	// (g_lock held) the viewer's list = display paths of the selected album
	void OpenAlbum(const State& s)
	{
		g_galFiles.clear();
		if (g_albums.empty()) return;
		for (const int i : g_albums[std::clamp(s.albumSel, 0, int(g_albums.size()) - 1)].photos) {
			g_galFiles.push_back(g_photos[i].display);
		}
	}

	void ReloadPhotos(int a_filter)
	{
		g_photos = Gallery::List();
		BuildAlbums(a_filter);
	}

	// album list: place, star system, photo count
	void DrawAlbums(Canvas& c, const State& s, int a_y)
	{
		const float H = float(c.h), W = float(c.w);
		const int   pad = Pad(c);
		const int   n = int(g_albums.size());
		const int   bottom = Prompts(c, { { "↑↓", "选择" }, { "←→", kGalFilters[s.galFilter] }, { "Enter", "打开" }, { "退格", "返回" } });
		char        note[32];
		std::snprintf(note, sizeof(note), "%d 张", n ? int(g_albums[0].photos.size()) : 0);
		int         y = Header(c, a_y, "相册", note);
		const float f = std::max(13.0f, H * 0.042f), sf = f * 0.78f;
		if (!n) {
			TextCentered(c, c.w / 2, y + int(H * 0.12f), "还没有照片", std::max(16.0f, H * 0.05f), kText);
			TextCentered(c, c.w / 2, y + int(H * 0.12f) + int(H * 0.08f), "手机相机或拍照模式拍的照片会出现在这里", sf, kDim);
			return;
		}
		const int limit = bottom - int(H * 0.01f);
		const int inner = int(W * 0.03f);
		const int rowH = int(f * 1.35f + sf * 1.35f + H * 0.02f);
		const int sel = std::clamp(s.albumSel, 0, n - 1);
		// keep the selection visible
		const int visible = std::max(1, (limit - y) / rowH);
		const int first = std::clamp(sel - visible + 1, 0, std::max(0, n - visible));
		for (int i = first; i < n && y + rowH <= limit + 2; ++i) {
			const auto& a = g_albums[i];
			const bool  on = i == sel;
			if (on) {
				Rect(c, pad, y, c.w - 2 * pad, rowH - 4, kSelBg);
			} else {
				Rect(c, pad, y + rowH - 3, c.w - 2 * pad, 1, kLine, 0.7f);
			}
			const Rgb fg = on ? kSelText : kText, dim = on ? Rgb{ 70, 76, 84 } : kDim;
			// pin mark for place albums
			const bool place = i > 0 && a.title != "未知位置";
			int        tx = pad + inner;
			if (place) {
				Rect(c, tx, y + int(f * 0.3f), int(f * 0.28f), int(f * 0.75f), on ? kSelText : kStripe[1]);
				tx += int(f * 0.6f);
			}
			char count[16];
			std::snprintf(count, sizeof(count), "%d", int(a.photos.size()));
			const int cw = TextWidth(count, f);
			Text(c, tx, y + int(H * 0.008f), Fit(a.title, f, c.w - pad - inner - tx - cw - inner).c_str(), f, fg);
			Text(c, c.w - pad - inner - cw, y + int(H * 0.008f), count, f, dim);
			const std::string sub = i == 0 ? std::string(kGalFilters[s.galFilter]) + "照片" : (a.sub.empty() ? "—" : a.sub);
			Text(c, tx, y + int(H * 0.008f) + int(f * 1.35f), Fit(sub, sf, c.w - pad - inner - tx).c_str(), sf, dim);
			y += rowH;
		}
	}

	// one photo of the open album; decoded on a worker thread
	void DrawPhoto(Canvas& c, const State& s, int a_y)
	{
		const float H = float(c.h), W = float(c.w);
		const float f = std::max(12.0f, H * 0.034f);
		const int   n = int(g_galFiles.size());
		const bool  confirm = s.galConfirm;
		const int   bottom = n ? Prompts(c, { { "←→", "翻看" }, { "Enter", confirm ? "确认删除" : "删除" }, { "退格", "返回" } }) :
		                         Prompts(c, { { "退格", "返回" } });
		if (!n || g_albums.empty()) {
			const int y = Header(c, a_y, "相册");
			TextCentered(c, c.w / 2, y + int(H * 0.15f), "这个相册是空的", std::max(16.0f, H * 0.05f), kText);
			return;
		}
		const auto& album = g_albums[std::clamp(s.albumSel, 0, int(g_albums.size()) - 1)];
		const int   idx = std::clamp(s.galIndex, 0, n - 1);
		const auto& path = g_galFiles[idx];
		const auto& photo = g_photos[album.photos[std::clamp(idx, 0, int(album.photos.size()) - 1)]];
		const int   infoH = int(f * 4.4f);
		const int   vx = int(W * 0.03f), vy = a_y + int(H * 0.02f);
		const int   vw = c.w - 2 * vx, vh = bottom - vy - infoH - int(H * 0.015f);
		Rect(c, vx, vy, vw, vh, { 0, 0, 0 }, 0.85f);
		{
			std::lock_guard lk(g_camLock);
			const bool current = g_galImgPath == path && g_galImgMaxW == vw && g_galImgMaxH == vh;
			if (current && !g_galLoading) {
				if (g_galImg.empty()) {
					TextCentered(c, c.w / 2, vy + vh / 2 - int(f), "无法打开", f * 1.2f, kWarn);
				} else {
					const int ox = vx + (vw - g_galImgW) / 2, oy = vy + (vh - g_galImgH) / 2;
					for (int y = 0; y < g_galImgH && oy + y < c.h; ++y) {
						std::memcpy(&c.px[(std::size_t(oy + y) * c.w + ox) * 4], &g_galImg[std::size_t(y) * g_galImgW * 4],
							std::size_t(g_galImgW) * 4);
					}
				}
			} else {
				TextCentered(c, c.w / 2, vy + vh / 2 - int(f), "加载中…", f * 1.2f, kDim);
				if (!g_galLoading) {
					g_galLoading = true;
					g_galImgPath = path;
					g_galImgMaxW = vw;
					g_galImgMaxH = vh;
					std::thread([path, vw, vh] {
						std::vector<std::uint8_t> px;
						int                       w = 0, h = 0;
						if (!Gallery::Load(path, vw, vh, px, w, h)) {
							px.clear();
						}
						{
							std::lock_guard lk2(g_camLock);
							if (g_galImgPath == path) {
								g_galImg = std::move(px);
								g_galImgW = w;
								g_galImgH = h;
							}
							g_galLoading = false;
						}
						++g_camSeq;  // redraw (also starts the next load if the selection moved meanwhile)
					}).detach();
				}
			}
		}
		Brackets(c, vx, vy, vw, vh, int(std::min(vw, vh) * 0.08f), 3, kText);
		// source badge on the photo
		const char* src = photo.source == Gallery::Source::kPhone ? "手机" : "拍照模式";
		const int   bw = TextWidth(src, f * 0.8f) + int(f * 0.8f);
		Rect(c, vx + int(f * 0.5f), vy + int(f * 0.5f), bw, int(f * 1.3f), kBgTop, 0.8f);
		Text(c, vx + int(f * 0.9f), vy + int(f * 0.62f), src, f * 0.8f, kText);
		// info: counter + time, then place (the album key) and GPS
		const int pad = vx;
		int       ty = vy + vh + int(H * 0.012f);
		char      count[32];
		std::snprintf(count, sizeof(count), "%02d / %02d", idx + 1, n);
		Text(c, pad, ty, count, f, kText);
		if (confirm) {
			const char* warn = "再按 Enter 删除（移到回收站）";
			Text(c, c.w - pad - TextWidth(warn, f), ty, warn, f, kWarn);
		} else {
			const auto& cap = photo.tagged && !photo.geo.date.empty() ? photo.geo.date : photo.caption;
			Text(c, c.w - pad - TextWidth(cap.c_str(), f), ty, cap.c_str(), f, kDim);
		}
		ty += int(f * 1.45f);
		const auto place = photo.tagged ? Gallery::PlaceTitle(photo.geo) : std::string{};
		std::string where = place.empty() ? "位置未知" : place;
		if (photo.tagged && !photo.geo.system.empty()) where += "，" + photo.geo.system;
		Rect(c, pad, ty + int(f * 0.25f), int(f * 0.28f), int(f * 0.8f), place.empty() ? kDim : kStripe[1]);
		Text(c, pad + int(f * 0.6f), ty, Fit(where, f, c.w - 2 * pad - int(f * 0.6f)).c_str(), f, place.empty() ? kDim : kText);
		ty += int(f * 1.4f);
		const auto gps = photo.tagged ? Gallery::GpsText(photo.geo) : std::string{};
		Text(c, pad + int(f * 0.6f), ty, gps.empty() ? "GPS 无信号" : ("GPS  " + gps).c_str(), f * 0.85f, kDim);
	}

	void DrawQuests(Canvas& c, const State& s, int a_y)
	{
		const float H = float(c.h), W = float(c.w);
		const int   pad = Pad(c);
		const float nameF = std::max(13.0f, H * 0.044f);
		const float objF = std::max(11.0f, H * 0.035f);
		const bool  any = g_questsLoaded && !g_quests.empty();
		const int   first = any ? std::clamp(s.questScroll, 0, int(g_quests.size()) - 1) : 0;
		const int   bottom = any ? Prompts(c, { { "↑↓", "选择" }, { "Enter", "详情" }, { "退格", "返回" } }) :
		                           Prompts(c, { { "退格", "返回" } });
		char        note[32] = "";
		if (any) {
			std::snprintf(note, sizeof(note), "%d / %d", first + 1, int(g_quests.size()));
		}
		int y = Header(c, a_y, "任务", any ? note : nullptr);
		if (!g_questsLoaded) {
			TextCentered(c, c.w / 2, y + int(H * 0.1f), "读取中…", nameF, kDim);
			return;
		}
		if (g_quests.empty()) {
			TextCentered(c, c.w / 2, y + int(H * 0.1f), "没有进行中的任务", nameF, kDim);
			return;
		}
		const int limit = bottom - int(H * 0.01f);
		const int inner = int(W * 0.025f);  // text inset inside the selection block
		int       shown = 0;
		for (int i = first; i < int(g_quests.size()); ++i) {
			const auto& q = g_quests[i];
			// section header at the top of the page and wherever the quest type changes
			if (!q.group.empty() && (i == first || q.group != g_quests[i - 1].group)) {
				if (shown > 0 && y + int(objF * 1.8f) + int(nameF * 1.3f) > limit) {
					break;
				}
				Text(c, pad, y, q.group.c_str(), objF * 0.92f, kStripe[1]);
				const int gw = TextWidth(q.group.c_str(), objF * 0.92f) + int(objF * 0.6f);
				Rect(c, pad + gw, y + int(objF * 0.62f), c.w - 2 * pad - gw, 1, kLine);
				y += int(objF * 1.6f);
			}
			const std::string title = q.tracked ? "◆ " + q.name : q.name;
			const auto        nameLines = Wrap(title, nameF, c.w - 2 * pad - 2 * inner);
			if (shown > 0 && y + int(nameF * 1.3f) * int(nameLines.size()) > limit) {
				break;
			}
			std::vector<std::vector<std::string>> objLines;
			int                                   blockH = int(nameF * 1.3f) * int(nameLines.size());
			for (const auto& o : q.objectives) {
				objLines.push_back(Wrap(o, objF, c.w - 2 * pad - 2 * inner - int(objF * 1.2f)));
				blockH += int(objF * 1.3f) * int(objLines.back().size());
			}
			const bool sel = i == first;
			if (sel) {
				Rect(c, pad, y - int(H * 0.008f), c.w - 2 * pad, std::min(blockH, limit - y) + int(H * 0.016f), kSelBg);
			}
			const Rgb nameColor = sel ? kSelText : (q.tracked ? kGold : kText);
			const Rgb objColor = sel ? Rgb{ 70, 76, 84 } : kDim;
			for (const auto& l : nameLines) {
				Text(c, pad + inner, y, l.c_str(), nameF, nameColor);
				y += int(nameF * 1.3f);
			}
			for (const auto& lines : objLines) {
				for (std::size_t k = 0; k < lines.size(); ++k) {
					if (y + int(objF * 1.3f) > limit) break;
					if (k == 0) Text(c, pad + inner, y, "·", objF, objColor);
					Text(c, pad + inner + int(objF * 1.2f), y, lines[k].c_str(), objF, objColor);
					y += int(objF * 1.3f);
				}
			}
			y += int(H * 0.028f);
			++shown;
			if (y > limit) break;
		}
	}

	// first wrapped line, with "…" when the text is longer
	std::string Fit(const std::string& a_text, float a_px, int a_maxW)
	{
		const auto lines = Wrap(a_text, a_px, a_maxW - TextWidth("…", a_px));
		if (lines.empty()) return {};
		return lines.size() > 1 ? lines[0] + "…" : lines[0];
	}

	// one quest: name, tracking, every displayed objective (open box = in progress, check = done, cross = failed)
	void DrawQuestDetail(Canvas& c, const State& s, int a_y)
	{
		const float H = float(c.h);
		const int   pad = Pad(c);
		const float nameF = std::max(15.0f, H * 0.05f);
		const float objF = std::max(11.0f, H * 0.037f);
		if (g_quests.empty()) {
			Prompts(c, { { "退格", "返回" } });
			const int y = Header(c, a_y, "任务详情");
			TextCentered(c, c.w / 2, y + int(H * 0.1f), "没有进行中的任务", nameF * 0.85f, kDim);
			return;
		}
		const auto& q = g_quests[std::clamp(s.questScroll, 0, int(g_quests.size()) - 1)];
		const int   bottom = Prompts(c, { { "↑↓", "滚动" }, { "Enter", q.tracked ? "取消追踪" : "追踪" }, { "退格", "返回" } });
		int         y = Header(c, a_y, "任务详情", q.group.c_str());
		for (const auto& l : Wrap(q.name, nameF, c.w - 2 * pad)) {
			Text(c, pad, y, l.c_str(), nameF, q.tracked ? kGold : kText);
			y += int(nameF * 1.3f);
		}
		Text(c, pad, y + int(objF * 0.2f), q.tracked ? "◆ 正在追踪" : "未追踪", objF, q.tracked ? kGold : kDim);
		y += int(objF * 1.9f);
		Rect(c, pad, y, c.w - 2 * pad, 1, kLine);
		y += int(H * 0.02f);
		const int limit = bottom - int(H * 0.01f);
		const int indent = int(objF * 1.6f);
		const int n = int(q.details.size());
		for (int i = std::clamp(s.detailScroll, 0, std::max(0, n - 1)); i < n; ++i) {
			const auto& o = q.details[i];
			if (y + int(objF * 1.3f) > limit) break;
			const Rgb col = o.state == 1 ? kText : (o.state == 2 ? kDim : kWarn);
			const int ms = int(objF * 0.6f), mx = pad + int(objF * 0.2f), my = y + int(objF * 0.35f);
			if (o.state == 1) {
				Frame(c, mx, my, ms, ms, 2, kText);
			} else if (o.state == 2) {
				if (HasIcon(0xE73E)) Icon(c, mx + ms / 2, my + ms / 2, 0xE73E, objF * 0.8f, kDim);
				else Rect(c, mx, my, ms, ms, kDim);
			} else {
				Text(c, mx, y, "×", objF, kWarn);
			}
			for (const auto& l : Wrap(o.text, objF, c.w - 2 * pad - indent)) {
				if (y + int(objF * 1.3f) > limit) break;
				Text(c, pad + indent, y, l.c_str(), objF, col);
				y += int(objF * 1.3f);
			}
			y += int(objF * 0.55f);
		}
	}

	// label / value rows with optional bars and section headers (Character, Ship, Star map)
	void DrawInfo(Canvas& c, const State& s, int a_y)
	{
		const float H = float(c.h), W = float(c.w);
		const int   pad = Pad(c);
		const int   bottom = Prompts(c, { { "↑↓", "滚动" }, { "退格", "返回" } });
		int         y = Header(c, a_y, kApps[s.openedApp].label);
		const float f = std::max(13.0f, H * 0.042f);
		if (g_infoPage != InfoFor(s.openedApp)) {
			TextCentered(c, c.w / 2, y + int(H * 0.1f), "读取中…", f, kDim);
			return;
		}
		if (g_infoRows.empty()) {
			TextCentered(c, c.w / 2, y + int(H * 0.1f), s.openedApp == kAppShip ? "没有家园飞船" : s.openedApp == kAppOutposts ? "没有找到前哨站" : "暂无数据", f, kDim);
			return;
		}
		const int limit = bottom - int(H * 0.01f);
		const int inner = int(W * 0.02f);
		const int barH = std::max(3, int(H * 0.008f));
		const int n = int(g_infoRows.size());
		for (int i = std::clamp(s.infoScroll, 0, n - 1); i < n; ++i) {
			const auto& r = g_infoRows[i];
			if (r.header) {
				if (y + int(f * 1.7f) > limit) break;
				y += int(f * 0.3f);
				const float hf = f * 0.8f;
				Text(c, pad, y, r.label.c_str(), hf, kStripe[1]);
				const int gw = TextWidth(r.label.c_str(), hf) + int(hf * 0.6f);
				Rect(c, pad + gw, y + int(hf * 0.62f), c.w - 2 * pad - gw, 1, kLine);
				y += int(f * 1.4f);
				continue;
			}
			const int lw = TextWidth(r.label.c_str(), f);
			const int avail = c.w - 2 * pad - 2 * inner;
			const int vw = TextWidth(r.value.c_str(), f);
			const bool twoLines = lw + int(f) + vw > avail;
			const auto valueLines = twoLines ? Wrap(r.value, f, avail) : std::vector<std::string>{ r.value };
			int h = int(f * 1.5f) + (twoLines ? int(f * 1.3f) * int(valueLines.size()) : 0) + (r.fraction >= 0 ? barH + int(f * 0.4f) : 0);
			if (y + h > limit) break;
			Text(c, pad + inner, y, r.label.c_str(), f, kDim);
			int yy = y;
			if (twoLines) {
				for (const auto& l : valueLines) {
					yy += int(f * 1.3f);
					Text(c, pad + inner, yy, l.c_str(), f, kText);
				}
			} else {
				Text(c, c.w - pad - inner - vw, y, r.value.c_str(), f, kText);
			}
			if (r.fraction >= 0) {
				const int by = yy + int(f * 1.4f);
				const int bw = c.w - 2 * pad - 2 * inner;
				Rect(c, pad + inner, by, bw, barH, kPanel);
				Rect(c, pad + inner, by, int(bw * r.fraction), barH, r.fraction < 0.25f ? kWarn : kText);
			}
			y += h + int(f * 0.35f);
			Rect(c, pad, y - int(f * 0.2f), c.w - 2 * pad, 1, kLine, 0.5f);
		}
	}

	bool Leap(int a_y) { return (a_y % 4 == 0 && a_y % 100 != 0) || a_y % 400 == 0; }

	int DaysIn(int a_y, int a_m0)
	{
		constexpr int d[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
		return a_m0 == 1 && Leap(a_y) ? 29 : d[std::clamp(a_m0, 0, 11)];
	}

	// 0 = Sunday (Sakamoto)
	int Weekday(int a_y, int a_m0, int a_d)
	{
		constexpr int t[] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
		const int     m = std::clamp(a_m0, 0, 11);
		const int     y = m < 2 ? a_y - 1 : a_y;
		return (y + y / 4 - y / 100 + y / 400 + t[m] + a_d) % 7;
	}

	constexpr const char* kWeekdays[] = { "日", "一", "二", "三", "四", "五", "六" };

	void DrawCalendar(Canvas& c, const State& s, int a_y)
	{
		const float H = float(c.h), W = float(c.w);
		const int   pad = Pad(c);
		Prompts(c, { { "退格", "返回" } });
		char note[16];
		std::snprintf(note, sizeof(note), "%u年", s.year);
		int y = Header(c, a_y, "日历", note);
		const int year = int(s.year), m0 = int(s.month), day = int(s.day);
		// time and the full date
		char clock[16];
		ClockText(s, clock);
		const float clockF = std::min(H * 0.1f, W * 0.16f);
		Text(c, pad, y, clock, clockF, kText);
		y += int(clockF * 1.05f);
		char date[64];
		std::snprintf(date, sizeof(date), "%d年%d月%d日  星期%s", year, m0 + 1, day, kWeekdays[Weekday(year, m0, day)]);
		const float dateF = std::max(13.0f, H * 0.038f);
		Text(c, pad, y, date, dateF, kDim);
		y += int(dateF * 1.8f);
		// month grid
		char month[16];
		std::snprintf(month, sizeof(month), "%d月", m0 + 1);
		const float mf = std::max(13.0f, H * 0.042f);
		Text(c, pad, y, month, mf, kStripe[1]);
		const int mw = TextWidth(month, mf) + int(mf * 0.6f);
		Rect(c, pad + mw, y + int(mf * 0.62f), c.w - 2 * pad - mw, 1, kLine);
		y += int(mf * 1.6f);
		const int   cellW = (c.w - 2 * pad) / 7;
		const float cf = std::max(11.0f, std::min(H * 0.036f, cellW * 0.42f));
		const int   cellH = int(cf * 2.0f);
		for (int i = 0; i < 7; ++i) {
			TextCentered(c, pad + i * cellW + cellW / 2, y, kWeekdays[i], cf * 0.9f, i == 0 || i == 6 ? kStripe[1] : kDim);
		}
		y += int(cf * 1.7f);
		const int first = Weekday(year, m0, 1);
		const int days = DaysIn(year, m0);
		for (int d = 1; d <= days; ++d) {
			const int slot = first + d - 1;
			const int x = pad + (slot % 7) * cellW, cy = y + (slot / 7) * cellH;
			char      num[4];
			std::snprintf(num, sizeof(num), "%d", d);
			if (d == day) {
				Rect(c, x + int(cellW * 0.08f), cy - int(cf * 0.3f), cellW - int(cellW * 0.16f), int(cf * 1.6f), kSelBg);
			}
			TextCentered(c, x + cellW / 2, cy, num, cf, d == day ? kSelText : kText);
		}
	}

	void DrawMessages(Canvas& c, const State& s, int a_y)
	{
		const float H = float(c.h), W = float(c.w);
		const int   pad = Pad(c);
		const int   n = int(g_messages.size());
		const int   bottom = n ? Prompts(c, { { "↑↓", "滚动" }, { "Enter", s.msgConfirm ? "确认清除" : "全部清除" }, { "退格", "返回" } }) :
		                         Prompts(c, { { "退格", "返回" } });
		char note[16] = "";
		if (n) {
			std::snprintf(note, sizeof(note), "%d 条", n);
		}
		int         y = Header(c, a_y, "消息", n ? note : nullptr);
		const float textF = std::max(12.0f, H * 0.038f);
		const float titleF = textF * 0.82f;
		if (!n) {
			TextCentered(c, c.w / 2, y + int(H * 0.12f), "没有消息", std::max(16.0f, H * 0.05f), kText);
			TextCentered(c, c.w / 2, y + int(H * 0.12f) + int(H * 0.08f), "任务目标更新、升级会出现在这里", textF * 0.9f, kDim);
			return;
		}
		const int limit = bottom - int(H * 0.01f);
		const int inner = int(W * 0.03f);
		const int gap = int(H * 0.015f);
		int       shown = 0;
		for (int i = std::clamp(s.msgScroll, 0, n - 1); i < n; ++i) {
			const auto& m = g_messages[i];
			const auto  lines = Wrap(m.text, textF, c.w - 2 * pad - 2 * inner);
			const int   h = int(titleF * 1.7f) + int(textF * 1.3f) * int(lines.size()) + int(textF * 0.5f);
			if (shown > 0 && y + h > limit) break;
			Rect(c, pad, y, c.w - 2 * pad, std::min(h, limit - y), kPanel, 0.9f);
			Rect(c, pad, y, 3, std::min(h, limit - y), kStripe[1]);
			const int sw = TextWidth(m.stamp.c_str(), titleF);
			Text(c, pad + inner, y + int(titleF * 0.4f), Fit(m.title, titleF, c.w - 2 * pad - 3 * inner - sw).c_str(), titleF, kStripe[1]);
			Text(c, c.w - pad - inner - sw, y + int(titleF * 0.4f), m.stamp.c_str(), titleF, kDim);
			int ty = y + int(titleF * 1.7f);
			for (const auto& l : lines) {
				if (ty + int(textF * 1.3f) > limit) break;
				Text(c, pad + inner, ty, l.c_str(), textF, kText);
				ty += int(textF * 1.3f);
			}
			y += h + gap;
			++shown;
			if (y > limit) break;
		}
	}

	// new-message banner under the status bar (any page)
	void DrawBanner(Canvas& c, int a_y)
	{
		const float H = float(c.h), W = float(c.w);
		const int   pad = Pad(c);
		const float f = std::max(12.0f, H * 0.036f);
		const int   inner = int(W * 0.03f);
		const int   y = a_y + int(H * 0.01f), h = int(f * 3.1f);
		Rect(c, pad, y, c.w - 2 * pad, h, kBgTop, 0.96f);
		Frame(c, pad, y, c.w - 2 * pad, h, 1, kLine);
		Rect(c, pad, y, 4, h, kStripe[1]);
		Text(c, pad + inner, y + int(f * 0.25f), Fit(g_bannerTitle, f * 0.82f, c.w - 2 * pad - 2 * inner).c_str(), f * 0.82f, kStripe[1]);
		Text(c, pad + inner, y + int(f * 1.45f), Fit(g_bannerText, f, c.w - 2 * pad - 2 * inner).c_str(), f, kText);
	}

	// filled anti-aliased disc / ring
	void Disc(Canvas& c, float a_cx, float a_cy, float a_r, Rgb a_col, float a_alpha = 1.0f)
	{
		for (int y = int(a_cy - a_r - 1); y <= int(a_cy + a_r + 1); ++y)
			for (int x = int(a_cx - a_r - 1); x <= int(a_cx + a_r + 1); ++x) {
				const float d = std::hypot(x + 0.5f - a_cx, y + 0.5f - a_cy) - a_r;
				c.Blend(x, y, a_col, std::clamp(0.5f - d, 0.0f, 1.0f) * a_alpha);
			}
	}

	void Ring(Canvas& c, float a_cx, float a_cy, float a_r, float a_t, Rgb a_col, float a_alpha = 1.0f)
	{
		for (int y = int(a_cy - a_r - a_t); y <= int(a_cy + a_r + a_t); ++y)
			for (int x = int(a_cx - a_r - a_t); x <= int(a_cx + a_r + a_t); ++x) {
				const float d = std::fabs(std::hypot(x + 0.5f - a_cx, y + 0.5f - a_cy) - a_r) - a_t / 2;
				c.Blend(x, y, a_col, std::clamp(0.5f - d, 0.0f, 1.0f) * a_alpha);
			}
	}

	std::string FirstChar(const std::string& a_utf8)
	{
		if (a_utf8.empty()) return "?";
		const auto b = static_cast<unsigned char>(a_utf8[0]);
		const int  len = b < 0x80 ? 1 : (b >> 5) == 0x6 ? 2 : (b >> 4) == 0xE ? 3 : 4;
		return a_utf8.substr(0, std::min<std::size_t>(len, a_utf8.size()));
	}

	void DrawContacts(Canvas& c, const State& s, int a_y)
	{
		const float H = float(c.h), W = float(c.w);
		const int   pad = Pad(c);
		const int   n = int(g_contacts.size());
		const int   bottom = n ? Prompts(c, { { "↑↓", "选择" }, { "Enter", "呼叫" }, { "退格", "返回" } }) : Prompts(c, { { "退格", "返回" } });
		char        note[16] = "";
		if (n) std::snprintf(note, sizeof(note), "%d 人", n);
		int         y = Header(c, a_y, "联系人", n ? note : nullptr);
		const float f = std::max(13.0f, H * 0.042f), sf = f * 0.78f;
		if (!g_contactsLoaded) {
			TextCentered(c, c.w / 2, y + int(H * 0.1f), "读取中…", f, kDim);
			return;
		}
		if (!n) {
			TextCentered(c, c.w / 2, y + int(H * 0.12f), "没有船员", std::max(16.0f, H * 0.05f), kText);
			TextCentered(c, c.w / 2, y + int(H * 0.12f) + int(H * 0.08f), "分配到家园飞船的船员会出现在这里", sf, kDim);
			return;
		}
		const int limit = bottom - int(H * 0.01f);
		const int inner = int(W * 0.03f);
		const int rowH = int(f * 1.35f + sf * 1.35f + H * 0.02f);
		const int av = int(rowH * 0.7f);  // avatar size
		const int sel = std::clamp(s.contactSel, 0, n - 1);
		const int visible = std::max(1, (limit - y) / rowH);
		const int first = std::clamp(sel - visible + 1, 0, std::max(0, n - visible));
		for (int i = first; i < n && y + rowH <= limit + 2; ++i) {
			const auto& ct = g_contacts[i];
			const bool  on = i == sel;
			if (on) Rect(c, pad, y, c.w - 2 * pad, rowH - 4, kSelBg);
			else Rect(c, pad, y + rowH - 3, c.w - 2 * pad, 1, kLine, 0.7f);
			const Rgb fg = on ? kSelText : kText, dim = on ? Rgb{ 70, 76, 84 } : kDim;
			const int ax = pad + inner, ay = y + (rowH - 4 - av) / 2;
			Disc(c, ax + av / 2.0f, ay + av / 2.0f, av / 2.0f, ct.dead ? kDim : (ct.distance >= 0 ? kStripe[3] : kPanel));
			const auto ini = FirstChar(ct.name);
			TextCentered(c, ax + av / 2, ay + int(av * 0.18f), ini.c_str(), av * 0.55f, kText);
			const int tx = ax + av + inner;
			Text(c, tx, y + int(H * 0.008f), Fit(ct.name, f, c.w - pad - inner - tx).c_str(), f, fg);
			const std::string where = ct.dead ? "已阵亡" : ct.where;
			Text(c, tx, y + int(H * 0.008f) + int(f * 1.35f), Fit(where, sf, c.w - pad - inner - tx).c_str(), sf,
				ct.distance >= 0 && !on ? kStripe[3] : dim);
			y += rowH;
		}
	}

	void DrawCall(Canvas& c, const State&, int a_y)
	{
		const float H = float(c.h), W = float(c.w);
		const auto& ct = g_callee;
		Prompts(c, { { "Enter", ct.dead ? "—" : "叫过来" }, { "退格", "挂断" } });
		const float big = std::max(18.0f, H * 0.065f), f = std::max(13.0f, H * 0.04f);
		const int   av = int(std::min(W * 0.4f, H * 0.2f));
		const int   cy = a_y + int(H * 0.1f) + av / 2;
		Ring(c, c.w / 2.0f, float(cy), av / 2.0f + av * 0.12f, 3, kStripe[1], 0.8f);
		Disc(c, c.w / 2.0f, float(cy), av / 2.0f, ct.dead ? kDim : kStripe[3]);
		const auto ini = FirstChar(ct.name);
		TextCentered(c, c.w / 2, cy - int(av * 0.3f), ini.c_str(), av * 0.55f, kText);
		int y = cy + av / 2 + int(av * 0.25f);
		TextCentered(c, c.w / 2, y, Fit(ct.name, big, c.w - 2 * Pad(c)).c_str(), big, kText);
		y += int(big * 1.5f);
		TextCentered(c, c.w / 2, y, Fit(ct.dead ? "已阵亡" : ct.where, f, c.w - 2 * Pad(c)).c_str(), f, kDim);
		y += int(f * 2.2f);
		const std::string status = ct.dead ? "无法接通" : (g_callResult.empty() ? "通话中…" : g_callResult);
		TextCentered(c, c.w / 2, y, status.c_str(), f, ct.dead ? kWarn : kGold);
	}

	const char* Direction(float a_right, float a_forward)
	{
		constexpr const char* dirs[] = { "前方", "右前", "右侧", "右后", "后方", "左后", "左侧", "左前" };
		float deg = std::atan2(a_right, a_forward) * 57.2957795f;  // 0 = ahead, clockwise
		if (deg < 0) deg += 360.0f;
		return dirs[int((deg + 22.5f) / 45.0f) % 8];
	}

	void DrawRadar(Canvas& c, const State& s, int a_y)
	{
		const float H = float(c.h), W = float(c.w);
		const int   pad = Pad(c);
		const float range = kRadarRanges[std::clamp(s.radarRange, 0, int(std::size(kRadarRanges)) - 1)];
		char        rangeLabel[32];
		std::snprintf(rangeLabel, sizeof(rangeLabel), "范围 %.0f m", range);
		const int bottom = Prompts(c, { { "←→", rangeLabel }, { "退格", "返回" } });
		int       hostile = 0;
		for (const auto& b : g_blips) hostile += b.kind == 1;
		char note[32] = "";
		if (hostile) std::snprintf(note, sizeof(note), "敌对 %d", hostile);
		int         y = Header(c, a_y, "扫描仪", hostile ? note : nullptr);
		const float f = std::max(12.0f, H * 0.036f);
		// radar disc
		const float R = std::min((c.w - 2 * pad) * 0.48f, (bottom - y) * 0.3f);
		const float cx = c.w / 2.0f, cy = y + R + H * 0.01f;
		Disc(c, cx, cy, R, kPanel, 0.85f);
		for (int k = 1; k <= 3; ++k) Ring(c, cx, cy, R * k / 3.0f, 1.2f, kLine);
		Rect(c, int(cx - R), int(cy), int(2 * R), 1, kLine, 0.7f);
		Rect(c, int(cx), int(cy - R), 1, int(2 * R), kLine, 0.7f);
		auto colorOf = [](int a_kind) { return a_kind == 1 ? kWarn : a_kind == 2 ? kStripe[3] : a_kind == 3 ? kDim : kText; };
		for (auto it = g_blips.rbegin(); it != g_blips.rend(); ++it) {  // nearest drawn last (on top)
			const float bx = cx + std::clamp(it->right / range, -1.0f, 1.0f) * R;
			const float by = cy - std::clamp(it->forward / range, -1.0f, 1.0f) * R;
			Disc(c, bx, by, std::max(3.0f, R * 0.035f), colorOf(it->kind));
		}
		// the player: a small arrow pointing up (forward)
		const float a = std::max(5.0f, R * 0.06f);
		for (int yy = 0; yy < int(a * 1.6f); ++yy) {
			const float half = a * 0.6f * yy / (a * 1.6f);
			Rect(c, int(cx - half), int(cy - a + yy), std::max(1, int(2 * half)), 1, kGold);
		}
		char rl[16];
		std::snprintf(rl, sizeof(rl), "%.0f m", range);
		Text(c, int(cx + R * 0.72f), int(cy - R), rl, f * 0.75f, kDim);
		y = int(cy + R + H * 0.02f);
		if (!g_radarLoaded) {
			TextCentered(c, c.w / 2, y, "扫描中…", f, kDim);
			return;
		}
		if (g_blips.empty()) {
			TextCentered(c, c.w / 2, y, "附近没有发现生命体", f, kDim);
			return;
		}
		// nearest contacts
		const int limit = bottom - int(H * 0.01f);
		const int rowH = int(f * 1.55f);
		for (const auto& b : g_blips) {
			if (y + rowH > limit) break;
			Disc(c, pad + f * 0.4f, y + f * 0.65f, f * 0.3f, colorOf(b.kind));
			char dist[48];
			std::snprintf(dist, sizeof(dist), "%s %.0f m", Direction(b.right, b.forward), b.distance);
			const int dw = TextWidth(dist, f);
			Text(c, pad + int(f * 1.2f), y, Fit(b.name, f, c.w - 2 * pad - int(f * 1.6f) - dw).c_str(), f, b.kind == 3 ? kDim : kText);
			Text(c, c.w - pad - dw, y, dist, f, b.kind == 1 ? kWarn : kDim);
			y += rowH;
		}
	}

	void DrawPlaceholder(Canvas& c, const State& s, int a_y)
	{
		const float H = float(c.h);
		Prompts(c, { { "退格", "返回" } });
		const int y = Header(c, a_y, kApps[s.openedApp].label);
		TextCentered(c, c.w / 2, y + int(H * 0.14f), "即将推出", std::max(16.0f, H * 0.055f), kDim);
	}

	int Depth(int a_page)
	{
		switch (a_page) {
		case kHome: return 0;
		case kQuestDetailPage:
		case kPhotoPage:
		case kCallPage: return 2;
		default: return 1;
		}
	}

	void DrawPage(Canvas& c, const State& s)
	{
		const int y = StatusBar(c, s);
		g_statusBottom = y;
		switch (s.page) {
		case kHome: DrawHome(c, s, y); break;
		case kSettingsPage: DrawSettings(c, s, y); break;
		case kCameraPage: DrawCamera(c, s, y); break;
		case kGalleryPage: DrawAlbums(c, s, y); break;
		case kPhotoPage: DrawPhoto(c, s, y); break;
		case kContactsPage: DrawContacts(c, s, y); break;
		case kCallPage: DrawCall(c, s, y); break;
		case kRadarPage: DrawRadar(c, s, y); break;
		case kQuestsPage: DrawQuests(c, s, y); break;
		case kQuestDetailPage: DrawQuestDetail(c, s, y); break;
		case kInfoPage: DrawInfo(c, s, y); break;
		case kCalendarPage: DrawCalendar(c, s, y); break;
		case kMessagesPage: DrawMessages(c, s, y); break;
		default: DrawPlaceholder(c, s, y); break;
		}
		// the banner is a separate layer (see Compose / Render) so it can slide
	}

	// g_inner (the current page, drawn) -> g_shown, through the running transition; sets g_fade
	void Compose(std::chrono::steady_clock::time_point a_now)
	{
		const int iw = g_inner.w, ih = g_inner.h;
		if (g_shown.w != iw || g_shown.h != ih) {
			g_shown.Resize(iw, ih);
		}
		g_fade = 1.0f;
		auto& A = g_anim;
		const float t = std::chrono::duration<float>(a_now - A.start).count() / A.seconds;
		if (A.kind == Anim::kNone || t >= 1.0f || (A.kind != Anim::kWake && (g_from.w != iw || g_from.h != ih))) {
			A.kind = Anim::kNone;
			g_shown.px = g_inner.px;
			return;
		}
		const int top = std::clamp(g_statusBottom, 0, ih);  // the status bar stays put
		std::memcpy(g_shown.px.data(), g_inner.px.data(), std::size_t(top) * iw * 4);
		const float W = float(iw), CH = float(ih - top);
		const float e = Spring(t);
		switch (A.kind) {
		case Anim::kLaunch:
		case Anim::kClose: {
			const bool    launch = A.kind == Anim::kLaunch;
			const float   p = launch ? e : 1.0f - e;  // 0 = tile, 1 = full page
			const Canvas& fg = launch ? g_inner : g_from;
			const Canvas& bg = launch ? g_from : g_inner;
			IRect         T = g_tiles[std::clamp(A.tile, 0, int(std::size(g_tiles)) - 1)];
			if (T.w <= 0) {
				T = { iw / 2 - iw / 10, top + int(CH / 2) - iw / 10, iw / 5, iw / 5 };
			}
			const float cx = T.x + T.w / 2.0f, cy = T.y + T.h / 2.0f;
			// home behind: zooms towards the tile and dims as the page grows
			const float s = 1.0f + 0.1f * p;
			BlitScaled(g_shown, bg, top, ih, cx - cx * s, cy + (top - cy) * s, W * s, CH * s, top, ih, 1.0f, 0.5f * p, false);
			// the page: from the tile rectangle to the content area, fading in over the first third
			const float rx = T.x * (1 - p), ry = T.y + (top - T.y) * p;
			const float rw = T.w + (W - T.w) * p, rh = T.h + (CH - T.h) * p;
			BlitScaled(g_shown, fg, top, ih, rx, ry, rw, rh, top, ih, std::clamp(p / 0.3f, 0.0f, 1.0f), 0.0f, true);
			break;
		}
		case Anim::kPush:
		case Anim::kPop: {
			const bool    push = A.kind == Anim::kPush;
			const float   p = push ? e : 1.0f - e;  // 0 = shallow page shown, 1 = deep page shown
			const Canvas& deep = push ? g_inner : g_from;
			const Canvas& shallow = push ? g_from : g_inner;
			BlitShift(g_shown, shallow, -int(std::lround(W * 0.3f * p)), top, ih, 0.3f * p);
			const int dx = int(std::lround(W * (1.0f - p)));
			BlitShift(g_shown, deep, dx, top, ih, 0.0f);
			ShadowLeft(g_shown, dx, top, ih, std::max(4, int(W * 0.04f)), 0.45f * (1.0f - p * 0.6f));
			break;
		}
		case Anim::kWake: {
			if (t < 0) {
				g_shown.px = g_inner.px;
				g_fade = 0.0f;
				return;
			}
			const float s = 1.06f - 0.06f * e;
			const float cx = W / 2, cy = top + CH / 2;
			BlitScaled(g_shown, g_inner, top, ih, cx - cx * s, cy + (top - cy) * s, W * s, CH * s, top, ih, 1.0f, 0.0f, false);
			g_fade = Spring(std::min(1.0f, t * 1.6f));
			break;
		}
		default: break;
		}
	}

	// banner slide: 0 = hidden above, 1 = in place; < 0 = not shown
	float BannerPos(std::chrono::steady_clock::time_point a_now)
	{
		if (a_now >= g_bannerUntil) return -1.0f;
		const float left = std::chrono::duration<float>(g_bannerUntil - a_now).count();
		const float age = 4.0f - left;  // PushMessage shows a banner for 4 s
		if (left < kBannerOut) return Spring(left / kBannerOut);
		return SpringBouncy(age / kBannerIn);
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
		const int       before = st.page;
		const int       n = int(std::size(kApps));
		const int       cols = 4;
		Action          act = Action::kNone;
		switch (st.page) {
		case kHome:
			switch (a_key) {
			case Key::kLeft: st.homeSel = (st.homeSel + n - 1) % n; break;
			case Key::kRight: st.homeSel = (st.homeSel + 1) % n; break;
			case Key::kUp:
				if (st.homeSel >= cols) st.homeSel -= cols;
				break;
			case Key::kDown:
				if (st.homeSel + cols < n) st.homeSel += cols;
				else if (st.homeSel / cols < (n - 1) / cols) st.homeSel = n - 1;  // short last row
				break;
			case Key::kEnter:
				st.openedApp = st.homeSel;
				st.page = PageFor(st.homeSel);
				st.setSel = 0;
				st.questScroll = 0;
				st.detailScroll = 0;
				st.infoScroll = 0;
				st.msgScroll = 0;
				st.msgConfirm = false;
				if (st.page == kQuestsPage) {
					g_questsLoaded = false;
				}
				if (st.page == kInfoPage) {
					g_infoPage = Ui::Info::kNone;  // "loading" until the game thread fills it
					g_infoRows.clear();
				}
				if (st.page == kMessagesPage) {
					g_unread = 0;
				}
				if (st.page == kGalleryPage) {
					ReloadPhotos(st.galFilter);
					st.albumSel = 0;
					st.galIndex = 0;
					st.galConfirm = false;
				}
				if (st.page == kContactsPage) {
					g_contactsLoaded = false;
					st.contactSel = 0;
				}
				if (st.page == kRadarPage) {
					g_radarLoaded = false;
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
			case Key::kEnter:
				if (g_quests.empty()) {
					return Action::kNone;
				}
				st.page = kQuestDetailPage;
				st.detailScroll = 0;
				break;
			case Key::kBack: st.page = kHome; break;
			default: return Action::kNone;
			}
			break;
		case kQuestDetailPage: {
			const int nd = g_quests.empty() ? 0 : int(g_quests[std::clamp(st.questScroll, 0, int(g_quests.size()) - 1)].details.size());
			switch (a_key) {
			case Key::kUp: st.detailScroll = std::max(0, st.detailScroll - 1); break;
			case Key::kDown: st.detailScroll = std::min(std::max(0, nd - 1), st.detailScroll + 1); break;
			case Key::kEnter:
				if (g_quests.empty()) {
					return Action::kNone;
				}
				act = Action::kToggleTrack;
				break;
			case Key::kBack: st.page = kQuestsPage; break;
			default: return Action::kNone;
			}
			break;
		}
		case kInfoPage:
			switch (a_key) {
			case Key::kUp: st.infoScroll = std::max(0, st.infoScroll - 1); break;
			case Key::kDown: st.infoScroll = std::min(std::max(0, int(g_infoRows.size()) - 1), st.infoScroll + 1); break;
			case Key::kBack: st.page = kHome; break;
			default: return Action::kNone;
			}
			break;
		case kMessagesPage: {
			const bool confirm = st.msgConfirm;
			st.msgConfirm = false;
			switch (a_key) {
			case Key::kUp: st.msgScroll = std::max(0, st.msgScroll - 1); break;
			case Key::kDown: st.msgScroll = std::min(std::max(0, int(g_messages.size()) - 1), st.msgScroll + 1); break;
			case Key::kEnter:
				if (g_messages.empty()) {
					return Action::kNone;
				}
				if (!confirm) {
					st.msgConfirm = true;
				} else {
					g_messages.clear();
					st.msgScroll = 0;
				}
				break;
			case Key::kBack: st.page = kHome; break;
			default: return Action::kNone;
			}
			break;
		}
		case kCameraPage:
			if (a_key == Key::kEnter) {
				act = Action::kTakePhoto;
				std::lock_guard ck(g_camLock);
				g_camFlashUntil = std::chrono::steady_clock::now() + std::chrono::milliseconds(150);
			} else if (a_key == Key::kUp) {
				act = Action::kOpenPhotoMode;
			} else if (a_key == Key::kBack) {
				st.page = kHome;
			} else {
				return Action::kNone;
			}
			break;
		case kGalleryPage: {  // album list
			const int n = int(g_albums.size());
			switch (a_key) {
			case Key::kUp:
				if (n) st.albumSel = (st.albumSel + n - 1) % n;
				break;
			case Key::kDown:
				if (n) st.albumSel = (st.albumSel + 1) % n;
				break;
			case Key::kLeft:
			case Key::kRight:
				st.galFilter = (st.galFilter + (a_key == Key::kRight ? 1 : 2)) % 3;
				BuildAlbums(st.galFilter);
				st.albumSel = 0;
				break;
			case Key::kEnter:
				if (!n) {
					return Action::kNone;
				}
				OpenAlbum(st);
				st.galIndex = 0;
				st.galConfirm = false;
				st.page = kPhotoPage;
				break;
			case Key::kBack: st.page = kHome; break;
			default: return Action::kNone;
			}
			break;
		}
		case kPhotoPage: {
			const int n = int(g_galFiles.size());
			const bool confirm = st.galConfirm;
			st.galConfirm = false;  // any other key cancels a pending delete
			switch (a_key) {
			case Key::kLeft:
			case Key::kUp:
				if (n) st.galIndex = (st.galIndex + n - 1) % n;
				break;
			case Key::kRight:
			case Key::kDown:
				if (n) st.galIndex = (st.galIndex + 1) % n;
				break;
			case Key::kEnter: {
				if (!n || g_albums.empty()) {
					return Action::kNone;
				}
				if (!confirm) {
					st.galConfirm = true;
					break;
				}
				const auto& album = g_albums[std::clamp(st.albumSel, 0, int(g_albums.size()) - 1)];
				const auto  idx = album.photos[std::clamp(st.galIndex, 0, int(album.photos.size()) - 1)];
				const auto  title = album.title;
				const bool  ok = Gallery::Delete(g_photos[idx]);
				Log("gallery: %s", ok ? "photo moved to the Recycle Bin" : "delete FAILED");
				ReloadPhotos(st.galFilter);
				// stay in the same album if it still has photos
				const auto it = std::find_if(g_albums.begin(), g_albums.end(), [&](const Album& a) { return a.title == title; });
				if (it == g_albums.end()) {
					st.albumSel = 0;
					st.page = kGalleryPage;
				} else {
					st.albumSel = int(it - g_albums.begin());
					OpenAlbum(st);
					st.galIndex = std::clamp(st.galIndex, 0, std::max(0, int(g_galFiles.size()) - 1));
				}
				break;
			}
			case Key::kBack: st.page = kGalleryPage; break;
			default: return Action::kNone;
			}
			break;
		}
		case kContactsPage: {
			const int n = int(g_contacts.size());
			switch (a_key) {
			case Key::kUp:
				if (n) st.contactSel = (st.contactSel + n - 1) % n;
				break;
			case Key::kDown:
				if (n) st.contactSel = (st.contactSel + 1) % n;
				break;
			case Key::kEnter:
				if (!n) {
					return Action::kNone;
				}
				g_callee = g_contacts[std::clamp(st.contactSel, 0, n - 1)];
				g_callResult.clear();
				st.page = kCallPage;
				break;
			case Key::kBack: st.page = kHome; break;
			default: return Action::kNone;
			}
			break;
		}
		case kCallPage:
			if (a_key == Key::kEnter) {
				if (g_callee.dead || !g_callee.formID) {
					return Action::kNone;
				}
				act = Action::kSummonContact;
				g_callResult = "正在请求…";
			} else if (a_key == Key::kBack) {
				st.page = kContactsPage;
			} else {
				return Action::kNone;
			}
			break;
		case kRadarPage:
			switch (a_key) {
			case Key::kLeft: st.radarRange = std::max(0, st.radarRange - 1); break;
			case Key::kRight: st.radarRange = std::min(int(std::size(kRadarRanges)) - 1, st.radarRange + 1); break;
			case Key::kBack: st.page = kHome; break;
			default: return Action::kNone;
			}
			break;
		default:
			if (a_key != Key::kBack) {
				return Action::kNone;
			}
			st.page = kHome;
			break;
		}
		if (st.page != before) {
			g_pending = before == kHome ? Anim::kLaunch :
			            st.page == kHome ? Anim::kClose :
			            Depth(st.page) > Depth(before) ? Anim::kPush :
			                                              Anim::kPop;
			g_pendingTile = st.openedApp;
			g_animating = true;  // raise the screen writer's frame rate before the next Render
		}
		g_camOpen = st.page == kCameraPage;
		++g_version;
		return act;
	}

	void OnOpen()
	{
		std::lock_guard lk(g_lock);
		g_state.page = kHome;
		g_camOpen = false;
		g_anim.kind = Anim::kNone;
		g_pending = Anim::kWake;
		g_animating = true;
		++g_version;
	}

	bool Animating() { return g_animating.load(); }

	bool CameraTarget(int& a_w, int& a_h)
	{
		a_w = g_camW.load();
		a_h = g_camH.load();
		return g_camOpen.load() && a_w > 0 && a_h > 0;
	}

	void SetCameraFrame(std::vector<std::uint8_t> a_rgba, int a_w, int a_h)
	{
		{
			std::lock_guard lk(g_camLock);
			g_camFrame = std::move(a_rgba);
			g_camFrameW = a_w;
			g_camFrameH = a_h;
		}
		++g_camSeq;
	}

	void SetCameraStatus(std::string a_text)
	{
		{
			std::lock_guard lk(g_camLock);
			g_camStatus = std::move(a_text);
			g_camStatusUntil = std::chrono::steady_clock::now() + std::chrono::milliseconds(2500);
		}
		++g_camSeq;
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
		return g_state.page == kQuestsPage || g_state.page == kQuestDetailPage;
	}

	bool WantsContacts()
	{
		std::lock_guard lk(g_lock);
		return g_state.page == kContactsPage;
	}

	void SetContacts(std::vector<Contact> a_list)
	{
		std::lock_guard lk(g_lock);
		g_contacts = std::move(a_list);
		g_contactsLoaded = true;
		g_state.contactSel = std::clamp(g_state.contactSel, 0, std::max(0, int(g_contacts.size()) - 1));
		++g_version;
	}

	Contact SelectedContact()
	{
		std::lock_guard lk(g_lock);
		return g_state.page == kCallPage ? g_callee : Contact{};
	}

	void SetCallResult(std::string a_text)
	{
		std::lock_guard lk(g_lock);
		g_callResult = std::move(a_text);
		++g_version;
	}

	bool WantsRadar(float& a_range)
	{
		std::lock_guard lk(g_lock);
		a_range = kRadarRanges[std::clamp(g_state.radarRange, 0, int(std::size(kRadarRanges)) - 1)];
		return g_state.page == kRadarPage;
	}

	void SetRadar(std::vector<Blip> a_blips)
	{
		std::lock_guard lk(g_lock);
		g_blips = std::move(a_blips);
		g_radarLoaded = true;
		++g_version;
	}

	Info WantsInfo()
	{
		std::lock_guard lk(g_lock);
		return g_state.page == kInfoPage ? InfoFor(g_state.openedApp) : Info::kNone;
	}

	void SetInfo(Info a_page, std::vector<InfoRow> a_rows)
	{
		std::lock_guard lk(g_lock);
		if (g_state.page != kInfoPage || InfoFor(g_state.openedApp) != a_page) {
			return;  // the page changed meanwhile
		}
		g_infoPage = a_page;
		g_infoRows = std::move(a_rows);
		g_state.infoScroll = std::clamp(g_state.infoScroll, 0, std::max(0, int(g_infoRows.size()) - 1));
		++g_version;
	}

	void PushMessage(std::string a_title, std::string a_text)
	{
		std::lock_guard lk(g_lock);
		char            stamp[48];
		char            clock[16];
		ClockText(g_state, clock);
		std::snprintf(stamp, sizeof(stamp), "%u月%u日 %s", g_state.month + 1, g_state.day, clock);
		g_bannerTitle = a_title;
		g_bannerText = a_text;
		g_bannerUntil = std::chrono::steady_clock::now() + std::chrono::seconds(4);
		g_messages.insert(g_messages.begin(), Message{ std::move(a_title), std::move(a_text), stamp });
		if (g_messages.size() > 60) {
			g_messages.resize(60);
		}
		if (g_state.page == kMessagesPage) {
			g_state.msgScroll = 0;
		} else {
			++g_unread;
		}
		++g_version;
	}

	int UnreadMessages()
	{
		std::lock_guard lk(g_lock);
		return g_unread;
	}

	QuestEntry SelectedQuest()
	{
		std::lock_guard lk(g_lock);
		if ((g_state.page != kQuestsPage && g_state.page != kQuestDetailPage) || g_quests.empty()) {
			return {};
		}
		return g_quests[std::clamp(g_state.questScroll, 0, int(g_quests.size()) - 1)];
	}

	void SetQuests(std::vector<QuestEntry> a_quests)
	{
		std::lock_guard lk(g_lock);
		const std::uint32_t selected = (!g_quests.empty() && g_state.questScroll < int(g_quests.size())) ?
		                                   g_quests[g_state.questScroll].formID :
		                                   0;
		g_quests = std::move(a_quests);
		for (int i = 0; selected && i < int(g_quests.size()); ++i) {
			if (g_quests[i].formID == selected) {
				g_state.questScroll = i;
				break;
			}
		}
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
		const auto  now = std::chrono::steady_clock::now();
		const int   ix = std::clamp(int(cw * s.insetL), 0, cw - 16), iy = std::clamp(int(ch * s.insetT), 0, ch - 16);
		const int   iw = std::max(16, cw - ix - int(cw * s.insetR)), ih = std::max(16, ch - iy - int(ch * s.insetB));
		bool        compose = false;
		if (g_bg.w != cw || g_bg.h != ch) {
			g_bg.Resize(cw, ch);
			FillBackground(g_bg);
		}
		if (g_canvas.w != cw || g_canvas.h != ch || g_inner.w != iw || g_inner.h != ih || minute != g_drawnMinute ||
			s.day != g_drawn.day || g_version != g_drawnVersion ||
			((s.page == kCameraPage || s.page == kPhotoPage) && g_camSeq.load() != g_drawnCamSeq)) {
			g_drawnCamSeq = g_camSeq.load();
			// a page change: animate from what is on screen now
			if (g_pending != Anim::kNone) {
				if (g_pending == Anim::kWake || (g_shown.w == iw && g_shown.h == ih)) {
					if (g_pending != Anim::kWake) {
						g_from.w = g_shown.w, g_from.h = g_shown.h, g_from.px = g_shown.px;
					}
					const bool wake = g_pending == Anim::kWake;
					const bool open = g_pending == Anim::kLaunch || g_pending == Anim::kClose;
					g_anim = { g_pending, g_pendingTile, now + std::chrono::milliseconds(wake ? 120 : 0), wake ? 0.5f : open ? 0.42f : 0.38f };
				}
				g_pending = Anim::kNone;
			}
			// draw the page on the safe area, over the background under it
			if (g_inner.w != iw || g_inner.h != ih) {
				g_inner.Resize(iw, ih);
			}
			for (int y = 0; y < ih; ++y) {
				std::memcpy(&g_inner.px[std::size_t(y) * iw * 4], &g_bg.px[(std::size_t(iy + y) * cw + ix) * 4], std::size_t(iw) * 4);
			}
			DrawPage(g_inner, s);
			g_drawnMinute = minute;
			g_drawnVersion = g_version;
			compose = true;
		}
		const float bannerPos = s.page == kMessagesPage ? -1.0f : BannerPos(now);
		const bool  bannerOn = bannerPos >= 0.0f;
		if (g_anim.kind != Anim::kNone || bannerOn || bannerOn != g_bannerWasOn || g_canvas.w != cw || g_canvas.h != ch) {
			compose = true;
		}
		if (compose) {
			Compose(now);
			const Canvas* out = &g_shown;
			if (bannerOn) {
				// the banner drops from above the screen edge (bouncy), and slides back up before it goes
				g_view.w = g_shown.w, g_view.h = g_shown.h, g_view.px = g_shown.px;
				const float f = std::max(12.0f, ih * 0.036f);
				const int   rest = g_statusBottom;
				const int   hidden = -(g_statusBottom + int(ih * 0.01f) + int(f * 3.1f) + 4);
				DrawBanner(g_view, int(std::lround(hidden + (rest - hidden) * bannerPos)));
				out = &g_view;
			}
			if (g_canvas.w != cw || g_canvas.h != ch) {
				g_canvas.Resize(cw, ch);
			}
			g_canvas.px = g_bg.px;
			for (int y = 0; y < ih; ++y) {
				std::memcpy(&g_canvas.px[(std::size_t(iy + y) * cw + ix) * 4], &out->px[std::size_t(y) * iw * 4], std::size_t(iw) * 4);
			}
			g_bannerWasOn = bannerOn;
		}
		// fast frames while something moves: a transition, or the banner sliding in / out
		const float bannerAge = std::chrono::duration<float>(now - (g_bannerUntil - std::chrono::seconds(4))).count();
		const bool  bannerMoving = bannerOn && (bannerAge < kBannerIn || bannerAge > 4.0f - kBannerOut - 0.1f);
		g_animating = g_anim.kind != Anim::kNone || g_pending != Anim::kNone || bannerMoving;
		const float bright = s.brightness / 100.0f * g_fade;
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
