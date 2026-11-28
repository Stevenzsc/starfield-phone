// Photo folders, location tags and decoding for the Camera and Gallery apps.

#include "gallery.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <knownfolders.h>
#include <shellapi.h>
#include <shlobj.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#if __has_include(<stb_image.h>)
#	include <stb_image.h>
#else
#	include <stb/stb_image.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwctype>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

namespace
{
	std::mutex                          g_lock;  // guards everything below
	Gallery::Geo                        g_geo;
	std::map<std::string, Gallery::Geo> g_tags;  // key: "phone|name" / "mode|name" (lower case)
	bool                                g_tagsLoaded = false;

	std::string Utf8(const std::wstring& a_w)
	{
		if (a_w.empty()) return {};
		const int   n = WideCharToMultiByte(CP_UTF8, 0, a_w.c_str(), int(a_w.size()), nullptr, 0, nullptr, nullptr);
		std::string s(std::size_t(n), '\0');
		WideCharToMultiByte(CP_UTF8, 0, a_w.c_str(), int(a_w.size()), s.data(), n, nullptr, nullptr);
		return s;
	}

	std::wstring Docs()
	{
		PWSTR docs = nullptr;
		if (FAILED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs))) {
			return {};
		}
		std::wstring dir = docs;
		CoTaskMemFree(docs);
		return dir;
	}

	bool IsDir(const std::wstring& a_path)
	{
		const DWORD a = GetFileAttributesW(a_path.c_str());
		return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
	}

	bool IsFile(const std::wstring& a_path)
	{
		const DWORD a = GetFileAttributesW(a_path.c_str());
		return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
	}

	std::wstring FileName(const std::wstring& a_path)
	{
		const auto slash = a_path.find_last_of(L"\\/");
		return slash == std::wstring::npos ? a_path : a_path.substr(slash + 1);
	}

	bool EndsWith(const std::wstring& a_s, const wchar_t* a_tail)
	{
		const std::wstring t = a_tail;
		if (a_s.size() < t.size()) return false;
		for (std::size_t i = 0; i < t.size(); ++i) {
			if (std::towlower(a_s[a_s.size() - t.size() + i]) != std::towlower(t[i])) return false;
		}
		return true;
	}

	std::wstring ThumbnailOf(const std::wstring& a_path)
	{
		return a_path.substr(0, a_path.size() - 4) + L"-thumbnail.png";  // Photo_X.png -> Photo_X-thumbnail.png
	}

	std::string KeyOf(const std::wstring& a_path)
	{
		std::wstring name = FileName(a_path);
		for (auto& c : name) c = wchar_t(std::towlower(c));
		const auto dir = a_path.substr(0, a_path.size() - FileName(a_path).size());
		const bool phone = EndsWith(dir.empty() ? dir : dir.substr(0, dir.size() - 1), L"PhoneCamera");
		return (phone ? "phone|" : "mode|") + Utf8(name);
	}

	std::wstring TagFile() { return Gallery::Dir() + L"\\locations.tsv"; }

	std::vector<std::string> Split(const std::string& a_line)
	{
		std::vector<std::string> out;
		std::string              cur;
		for (char c : a_line) {
			if (c == '\t') {
				out.push_back(cur);
				cur.clear();
			} else if (c != '\r') {
				cur += c;
			}
		}
		out.push_back(cur);
		return out;
	}

	std::string Sanitize(std::string a_s)
	{
		for (auto& c : a_s) {
			if (c == '\t' || c == '\n' || c == '\r') c = ' ';
		}
		return a_s;
	}

	// g_lock held
	void LoadTags()
	{
		if (g_tagsLoaded) return;
		g_tagsLoaded = true;
		std::ifstream in(TagFile(), std::ios::binary);
		std::string   line;
		while (std::getline(in, line)) {
			const auto t = Split(line);
			if (t.size() < 8 || t[0].empty()) continue;
			Gallery::Geo g;
			g.system = t[1], g.planet = t[2], g.place = t[3];
			g.gps = t[4] == "1";
			g.lat = std::atof(t[5].c_str());
			g.lon = std::atof(t[6].c_str());
			g.date = t[7];
			g_tags[t[0]] = g;
		}
	}

	// g_lock held
	void SaveTags()
	{
		std::ofstream out(TagFile(), std::ios::binary | std::ios::trunc);
		for (const auto& [k, g] : g_tags) {
			char ll[64];
			std::snprintf(ll, sizeof(ll), "%.6f\t%.6f", g.lat, g.lon);
			out << k << '\t' << Sanitize(g.system) << '\t' << Sanitize(g.planet) << '\t' << Sanitize(g.place) << '\t'
				<< (g.gps ? "1" : "0") << '\t' << ll << '\t' << Sanitize(g.date) << '\n';
		}
	}

	std::string CaptionOf(const FILETIME& a_ft)
	{
		FILETIME   local{};
		SYSTEMTIME st{};
		if (!FileTimeToLocalFileTime(&a_ft, &local) || !FileTimeToSystemTime(&local, &st)) return {};
		char buf[32];
		std::snprintf(buf, sizeof(buf), "%04u-%02u-%02u %02u:%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
		return buf;
	}

	// photo mode names in a folder (no thumbnails)
	void Scan(const std::wstring& a_dir, const wchar_t* a_pattern, Gallery::Source a_src, std::vector<Gallery::Photo>& a_out)
	{
		if (a_dir.empty()) return;
		WIN32_FIND_DATAW fd{};
		HANDLE           h = FindFirstFileW((a_dir + L"\\" + a_pattern).c_str(), &fd);
		if (h == INVALID_HANDLE_VALUE) return;
		do {
			if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
			const std::wstring name = fd.cFileName;
			if (EndsWith(name, L"-thumbnail.png") || !EndsWith(name, L".png")) continue;
			Gallery::Photo p;
			p.path = a_dir + L"\\" + name;
			p.display = p.path;
			if (a_src == Gallery::Source::kPhotoMode && IsFile(ThumbnailOf(p.path))) {
				p.display = ThumbnailOf(p.path);
			}
			p.source = a_src;
			p.time = (std::int64_t(fd.ftLastWriteTime.dwHighDateTime) << 32) | fd.ftLastWriteTime.dwLowDateTime;
			p.caption = CaptionOf(fd.ftLastWriteTime);
			a_out.push_back(std::move(p));
		} while (FindNextFileW(h, &fd));
		FindClose(h);
	}
}

namespace Gallery
{
	void SetCurrentGeo(const Geo& a_geo)
	{
		std::lock_guard lk(g_lock);
		g_geo = a_geo;
	}

	Geo CurrentGeo()
	{
		std::lock_guard lk(g_lock);
		return g_geo;
	}

	std::string PlaceTitle(const Geo& a_geo)
	{
		if (a_geo.planet.empty()) return a_geo.place;
		if (a_geo.place.empty() || a_geo.place == a_geo.planet) return a_geo.planet;
		return a_geo.place + " · " + a_geo.planet;
	}

	std::string GpsText(const Geo& a_geo)
	{
		if (!a_geo.gps) return {};
		char buf[96];
		std::snprintf(buf, sizeof(buf), "%s %.3f°  %s %.3f°", a_geo.lat >= 0 ? "北纬" : "南纬", std::fabs(a_geo.lat),
			a_geo.lon >= 0 ? "东经" : "西经", std::fabs(a_geo.lon));
		return buf;
	}

	std::wstring Dir()
	{
		std::wstring dir = Docs();
		if (dir.empty()) return {};
		for (const wchar_t* part : { L"\\My Games", L"\\Starfield", L"\\PhoneCamera" }) {
			dir += part;
			CreateDirectoryW(dir.c_str(), nullptr);
		}
		return dir;
	}

	std::wstring PhotoModeDir()
	{
		const auto docs = Docs();
		if (!docs.empty() && IsDir(docs + L"\\My Games\\Starfield\\Data\\Textures\\Photos")) {
			return docs + L"\\My Games\\Starfield\\Data\\Textures\\Photos";
		}
		wchar_t full[MAX_PATH];
		if (GetFullPathNameW(L"Data\\Textures\\Photos", MAX_PATH, full, nullptr) && IsDir(full)) {
			return full;
		}
		return {};
	}

	std::vector<Photo> List()
	{
		std::vector<Photo> out;
		Scan(Dir(), L"*.png", Source::kPhone, out);
		Scan(PhotoModeDir(), L"Photo_*.png", Source::kPhotoMode, out);
		std::sort(out.begin(), out.end(), [](const Photo& a, const Photo& b) { return a.time > b.time; });
		std::lock_guard lk(g_lock);
		LoadTags();
		for (auto& p : out) {
			if (auto it = g_tags.find(KeyOf(p.path)); it != g_tags.end()) {
				p.tagged = true;
				p.geo = it->second;
			}
		}
		return out;
	}

	void TagCurrent(const std::wstring& a_path)
	{
		std::lock_guard lk(g_lock);
		LoadTags();
		g_tags[KeyOf(a_path)] = g_geo;
		SaveTags();
	}

	std::vector<std::wstring> PollNewPhotoModePhotos()
	{
		static std::set<std::wstring> known;
		static bool                   first = true;
		static std::wstring           dir;
		std::vector<std::wstring>     fresh;
		if (dir.empty()) {
			dir = PhotoModeDir();
			if (dir.empty()) return fresh;
		}
		std::vector<Photo> now;
		Scan(dir, L"Photo_*.png", Source::kPhotoMode, now);
		for (const auto& p : now) {
			if (known.insert(p.path).second && !first) {
				TagCurrent(p.path);
				fresh.push_back(p.path);
			}
		}
		first = false;
		return fresh;
	}

	bool Load(const std::wstring& a_path, int a_maxW, int a_maxH, std::vector<std::uint8_t>& a_rgba, int& a_w, int& a_h)
	{
		if (a_maxW <= 0 || a_maxH <= 0) {
			return false;
		}
		FILE* f = _wfopen(a_path.c_str(), L"rb");
		if (!f) {
			return false;
		}
		std::vector<unsigned char> file;
		std::fseek(f, 0, SEEK_END);
		const long size = std::ftell(f);
		std::fseek(f, 0, SEEK_SET);
		if (size > 0) {
			file.resize(std::size_t(size));
			file.resize(std::fread(file.data(), 1, file.size(), f));
		}
		std::fclose(f);
		int   sw = 0, sh = 0, n = 0;
		auto* src = stbi_load_from_memory(file.data(), int(file.size()), &sw, &sh, &n, 4);
		if (!src) {
			return false;
		}
		const float scale = std::min(float(a_maxW) / float(sw), float(a_maxH) / float(sh));
		a_w = std::max(1, int(float(sw) * scale));
		a_h = std::max(1, int(float(sh) * scale));
		a_rgba.assign(std::size_t(a_w) * a_h * 4, 255);
		const float step = 1.0f / scale;
		for (int y = 0; y < a_h; ++y) {
			for (int x = 0; x < a_w; ++x) {
				unsigned sum[3]{};
				for (int k = 0; k < 4; ++k) {  // 2x2 samples
					const int px = std::min(sw - 1, int((float(x) + 0.25f + 0.5f * float(k & 1)) * step));
					const int py = std::min(sh - 1, int((float(y) + 0.25f + 0.5f * float(k >> 1)) * step));
					const auto* p = src + (std::size_t(py) * sw + px) * 4;
					sum[0] += p[0], sum[1] += p[1], sum[2] += p[2];
				}
				auto* d = &a_rgba[(std::size_t(y) * a_w + x) * 4];
				d[0] = std::uint8_t(sum[0] / 4), d[1] = std::uint8_t(sum[1] / 4), d[2] = std::uint8_t(sum[2] / 4);
			}
		}
		stbi_image_free(src);
		return true;
	}

	bool Delete(const Photo& a_photo)
	{
		std::wstring from = a_photo.path;
		from.push_back(L'\0');
		if (a_photo.source == Source::kPhotoMode && IsFile(ThumbnailOf(a_photo.path))) {
			from += ThumbnailOf(a_photo.path);
			from.push_back(L'\0');
		}
		from.push_back(L'\0');  // double-null terminated list
		SHFILEOPSTRUCTW op{};
		op.wFunc = FO_DELETE;
		op.pFrom = from.c_str();
		op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
		const bool ok = SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted;
		if (ok) {
			std::lock_guard lk(g_lock);
			LoadTags();
			if (g_tags.erase(KeyOf(a_photo.path))) {
				SaveTags();
			}
		}
		return ok;
	}
}
