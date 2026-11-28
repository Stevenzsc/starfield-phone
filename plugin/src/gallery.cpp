// Photo folder access for the Camera and Gallery apps.

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
#include <cstdio>
#include <cwctype>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

namespace Gallery
{
	std::wstring Dir()
	{
		PWSTR docs = nullptr;
		if (FAILED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs))) {
			return {};
		}
		std::wstring dir = docs;
		CoTaskMemFree(docs);
		for (const wchar_t* part : { L"\\My Games", L"\\Starfield", L"\\PhoneCamera" }) {
			dir += part;
			CreateDirectoryW(dir.c_str(), nullptr);
		}
		return dir;
	}

	std::vector<std::wstring> List()
	{
		std::vector<std::wstring> out;
		const auto                dir = Dir();
		if (dir.empty()) {
			return out;
		}
		WIN32_FIND_DATAW fd{};
		HANDLE           h = FindFirstFileW((dir + L"\\*.png").c_str(), &fd);
		if (h == INVALID_HANDLE_VALUE) {
			return out;
		}
		do {
			if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
				out.push_back(dir + L"\\" + fd.cFileName);
			}
		} while (FindNextFileW(h, &fd));
		FindClose(h);
		// names carry the time (Photo_YYYYMMDD_HHMMSS): reverse name order = newest first
		std::sort(out.begin(), out.end(), std::greater<>());
		return out;
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

	bool Recycle(const std::wstring& a_path)
	{
		std::wstring from = a_path;
		from.push_back(L'\0');  // double-null terminated list
		SHFILEOPSTRUCTW op{};
		op.wFunc = FO_DELETE;
		op.pFrom = from.c_str();
		op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
		return SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted;
	}

	std::string Caption(const std::wstring& a_path)
	{
		const auto   slash = a_path.find_last_of(L"\\/");
		std::wstring name = slash == std::wstring::npos ? a_path : a_path.substr(slash + 1);
		// Photo_YYYYMMDD_HHMMSS.png
		if (name.size() < 21 || name.compare(0, 6, L"Photo_") != 0) {
			return {};
		}
		std::string d;
		for (std::size_t i = 6; i < 21; ++i) {
			if (i != 14 && !std::iswdigit(name[i])) {
				return {};
			}
			d.push_back(char(name[i]));
		}
		// d = YYYYMMDD_HHMMSS
		return d.substr(0, 4) + "-" + d.substr(4, 2) + "-" + d.substr(6, 2) + " " + d.substr(9, 2) + ":" + d.substr(11, 2);
	}
}
