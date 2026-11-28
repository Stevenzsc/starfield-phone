// Music app playback: MCI ("mpegvideo" = DirectShow, so whatever codecs Windows has) on a worker thread.

#include "music.h"
#include "lang.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <knownfolders.h>
#include <mmsystem.h>
#include <shlobj.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cwctype>
#include <deque>
#include <filesystem>
#include <mutex>
#include <thread>

#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

namespace
{
	enum class Cmd
	{
		kRescan,
		kToggle,
		kNext,
		kPrev,
		kVolume,
	};

	std::mutex                g_lock;  // guards everything below
	std::condition_variable   g_wake;
	std::deque<Cmd>           g_queue;
	Music::Status             g_status;
	std::vector<std::wstring> g_files;  // same order as g_status.titles
	bool                      g_started = false;
	bool                      g_scanned = false;
	std::atomic_uint32_t      g_seq{ 1 };

	constexpr const wchar_t* kAlias = L"phonemusic";

	std::string Utf8(const std::wstring& a_w)
	{
		if (a_w.empty()) return {};
		const int   n = WideCharToMultiByte(CP_UTF8, 0, a_w.c_str(), int(a_w.size()), nullptr, 0, nullptr, nullptr);
		std::string s(std::size_t(std::max(0, n)), '\0');
		WideCharToMultiByte(CP_UTF8, 0, a_w.c_str(), int(a_w.size()), s.data(), n, nullptr, nullptr);
		return s;
	}

	std::wstring Folder()
	{
		PWSTR docs = nullptr;
		if (FAILED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs))) {
			return {};
		}
		std::wstring dir = std::wstring(docs) + L"\\My Games\\Starfield\\PhoneMusic";
		CoTaskMemFree(docs);
		std::error_code ec;
		std::filesystem::create_directories(dir, ec);
		return dir;
	}

	// MCI command; returns the reply text, or "" on error (a_ok = false)
	std::wstring Mci(const std::wstring& a_cmd, bool* a_ok = nullptr)
	{
		wchar_t    reply[256] = {};
		const auto err = mciSendStringW(a_cmd.c_str(), reply, 256, nullptr);
		if (a_ok) *a_ok = err == 0;
		return err == 0 ? reply : L"";
	}

	int MciInt(const std::wstring& a_cmd)
	{
		const auto r = Mci(a_cmd);
		return r.empty() ? 0 : _wtoi(r.c_str());
	}

	// ---- worker thread only ----
	bool g_open = false;

	void Close()
	{
		if (g_open) {
			Mci(std::wstring(L"close ") + kAlias);
			g_open = false;
		}
	}

	void ApplyVolume(int a_percent)
	{
		if (g_open) {
			Mci(std::wstring(L"setaudio ") + kAlias + L" volume to " + std::to_wstring(std::clamp(a_percent, 0, 100) * 10));
		}
	}

	// open and play track a_index (wraps); skips files that cannot be opened, at most one round
	void PlayTrack(int a_index)
	{
		Close();
		std::vector<std::wstring> files;
		int                       volume = 60;
		{
			std::lock_guard lk(g_lock);
			files = g_files;
			volume = g_status.volume;
		}
		const int n = int(files.size());
		if (!n) {
			std::lock_guard lk(g_lock);
			g_status.index = -1;
			g_status.playing = false;
			return;
		}
		std::string error;
		for (int tries = 0; tries < n; ++tries) {
			const int i = ((a_index + tries) % n + n) % n;
			bool      ok = false;
			Mci(L"open \"" + files[i] + L"\" type mpegvideo alias " + kAlias, &ok);
			if (!ok) {
				error = Tr("无法播放 ") + Utf8(std::filesystem::path(files[i]).filename().wstring());
				continue;
			}
			g_open = true;
			Mci(std::wstring(L"set ") + kAlias + L" time format milliseconds");
			ApplyVolume(volume);
			Mci(std::wstring(L"play ") + kAlias, &ok);
			std::lock_guard lk(g_lock);
			g_status.index = i;
			g_status.playing = ok;
			g_status.posMs = 0;
			g_status.lenMs = MciInt(std::wstring(L"status ") + kAlias + L" length");
			g_status.error = error;
			return;
		}
		std::lock_guard lk(g_lock);
		g_status.index = -1;
		g_status.playing = false;
		g_status.error = error;
	}

	void Scan()
	{
		const auto                dir = Folder();
		std::vector<std::wstring> files;
		std::error_code           ec;
		for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
			if (!e.is_regular_file(ec)) continue;
			auto ext = e.path().extension().wstring();
			for (auto& ch : ext) ch = wchar_t(std::towlower(ch));
			if (ext == L".mp3" || ext == L".wav" || ext == L".wma" || ext == L".m4a" || ext == L".aac" || ext == L".flac") {
				files.push_back(e.path().wstring());
			}
		}
		std::sort(files.begin(), files.end());
		std::lock_guard lk(g_lock);
		// keep the current track's index if it is still there
		const std::wstring current = g_status.index >= 0 && g_status.index < int(g_files.size()) ? g_files[g_status.index] : L"";
		g_files = std::move(files);
		g_status.titles.clear();
		g_status.index = -1;
		for (int i = 0; i < int(g_files.size()); ++i) {
			g_status.titles.push_back(Utf8(std::filesystem::path(g_files[i]).stem().wstring()));
			if (g_files[i] == current) g_status.index = i;
		}
		g_status.folder = Utf8(dir);
		g_scanned = true;
	}

	void Worker()
	{
		auto nextPoll = std::chrono::steady_clock::now();
		for (;;) {
			Cmd  cmd{};
			bool has = false;
			{
				std::unique_lock lk(g_lock);
				g_wake.wait_for(lk, std::chrono::milliseconds(250), [] { return !g_queue.empty(); });
				if (!g_queue.empty()) {
					cmd = g_queue.front();
					g_queue.pop_front();
					has = true;
				}
			}
			if (has) {
				int index = -1, volume = 60;
				bool playing = false;
				{
					std::lock_guard lk(g_lock);
					index = g_status.index;
					volume = g_status.volume;
					playing = g_status.playing;
				}
				switch (cmd) {
				case Cmd::kRescan:
					Scan();
					if (index >= 0 && Music::Get().index < 0) Close();  // the playing file is gone
					break;
				case Cmd::kToggle:
					if (!g_open) {
						if (!g_scanned) Scan();
						PlayTrack(std::max(0, index));
					} else {
						Mci(std::wstring(playing ? L"pause " : L"resume ") + kAlias);
						std::lock_guard lk(g_lock);
						g_status.playing = !playing;
					}
					break;
				case Cmd::kNext:
					if (!g_scanned) Scan();
					PlayTrack(index + 1);
					break;
				case Cmd::kPrev:
					if (!g_scanned) Scan();
					if (g_open && MciInt(std::wstring(L"status ") + kAlias + L" position") > 3000) {
						Mci(std::wstring(L"seek ") + kAlias + L" to start");
						Mci(std::wstring(L"play ") + kAlias);
						std::lock_guard lk(g_lock);
						g_status.playing = true;
						g_status.posMs = 0;
					} else {
						PlayTrack(index < 0 ? 0 : index - 1);
					}
					break;
				case Cmd::kVolume:
					ApplyVolume(volume);
					break;
				}
				++g_seq;
				continue;  // drain the queue before polling
			}
			// progress, and the next track when this one ends
			if (g_open && std::chrono::steady_clock::now() >= nextPoll) {
				nextPoll = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
				const auto mode = Mci(std::wstring(L"status ") + kAlias + L" mode");
				const int  pos = MciInt(std::wstring(L"status ") + kAlias + L" position");
				bool       playing = false;
				int        index = -1, len = 0;
				{
					std::lock_guard lk(g_lock);
					g_status.posMs = pos;
					playing = g_status.playing;
					index = g_status.index;
					len = g_status.lenMs;
				}
				if (playing && mode == L"stopped" && (len <= 0 || pos >= len - 500)) {
					PlayTrack(index + 1);
				}
				if (playing) ++g_seq;
			}
		}
	}

	void Post(Cmd a_cmd)
	{
		{
			std::lock_guard lk(g_lock);
			if (!g_started) {
				g_started = true;
				std::thread(Worker).detach();
			}
			g_queue.push_back(a_cmd);
		}
		g_wake.notify_one();
	}
}

namespace Music
{
	Status Get()
	{
		std::lock_guard lk(g_lock);
		return g_status;
	}

	unsigned Seq() { return g_seq.load(); }

	void Rescan() { Post(Cmd::kRescan); }
	void Toggle() { Post(Cmd::kToggle); }
	void Next() { Post(Cmd::kNext); }
	void Prev() { Post(Cmd::kPrev); }

	void SetVolume(int a_percent)
	{
		{
			std::lock_guard lk(g_lock);
			const int v = std::clamp(a_percent, 0, 100);
			if (v == g_status.volume && g_started) return;
			g_status.volume = v;
		}
		++g_seq;
		std::lock_guard lk(g_lock);
		if (g_started) {
			g_queue.push_back(Cmd::kVolume);
			g_wake.notify_one();
		}
	}
}
