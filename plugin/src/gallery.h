#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Photos for the Camera and Gallery apps: the phone's own (PNG files in Documents\My Games\Starfield\PhoneCamera)
// and the game's photo mode (Documents\My Games\Starfield\Data\Textures\Photos, Photo_YYYY-MM-DD-HHMMSS.png plus a
// 960x540 -thumbnail.png). Every photo can carry a location tag ("GPS"), kept in PhoneCamera\locations.tsv.
// This file does not use CommonLibSF. Thread-safe.
namespace Gallery
{
	// where the player is; filled from the game thread, read when a photo is tagged
	struct Geo
	{
		std::string system, planet, place;  // empty = unknown
		bool        gps = false;            // lat / lon valid (on a planet surface, outside)
		double      lat = 0, lon = 0;       // degrees
		std::string date;                   // in-game date and time, "2330年11月28日 14:05"
	};
	void SetCurrentGeo(const Geo& a_geo);
	Geo  CurrentGeo();

	// "着陆区域 · 霍金I" (album title; "" = unknown)
	std::string PlaceTitle(const Geo& a_geo);
	// "北纬 12.34° 东经 56.78°" ("" without GPS)
	std::string GpsText(const Geo& a_geo);

	enum class Source
	{
		kPhone,
		kPhotoMode,
	};
	struct Photo
	{
		std::wstring path;     // full picture
		std::wstring display;  // what the viewer decodes (photo mode: the thumbnail when it exists)
		Source       source = Source::kPhone;
		std::int64_t time = 0;  // file write time (100 ns since 1601), for sorting
		std::string  caption;   // real-world capture time "2026-11-28 15:30"
		bool         tagged = false;
		Geo          geo;
	};

	std::wstring       Dir();           // the phone's photo folder (created if missing); empty if Documents is unknown
	std::wstring       PhotoModeDir();  // the game's photo mode folder ("" if missing)
	std::vector<Photo> List();          // both sources, newest first, with their tags

	// tag a photo with the current location (phone photos when saved, photo mode photos when they appear)
	void TagCurrent(const std::wstring& a_path);

	// photo mode photos that appeared since the last call, already tagged with the current location.
	// The first call only records what is there.
	std::vector<std::wstring> PollNewPhotoModePhotos();

	// Decode a photo and scale it to fit inside a_maxW x a_maxH (aspect kept). RGBA8 out.
	bool Load(const std::wstring& a_path, int a_maxW, int a_maxH, std::vector<std::uint8_t>& a_rgba, int& a_w, int& a_h);

	// move the photo (and its photo mode thumbnail) to the Recycle Bin and forget its tag
	bool Delete(const Photo& a_photo);
}
