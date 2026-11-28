#pragma once

#include <string>

// UI language. Game text (quest, place, character names) always comes from the game in its own language; this only
// translates the phone's own strings. Source strings are Chinese; Tr() returns the English text when English is on.
// This file does not use CommonLibSF.
namespace Lang
{
	// 0 auto (follow the game's text language), 1 Chinese, 2 English
	void SetMode(int a_mode);
	int  Mode();
	const char* ModeName(int a_mode);  // "自动" / "中文" / "English" (already in the current language)

	// result of the auto detection (the game's text language is not Chinese)
	void SetGameEnglish(bool a_english);
	bool English();

	// dates of the in-game calendar (a_m0 = 0-based month)
	std::string Date(unsigned a_year, unsigned a_m0, unsigned a_day);  // "2330年11月28日" / "Nov 28, 2330"
	std::string MonthDay(unsigned a_m0, unsigned a_day);              // "11月28日" / "Nov 28"
	std::string Month(unsigned a_m0);                                 // "11月" / "November"
	std::string Year(unsigned a_year);                                // "2330年" / "2330"
	const char* WeekdayShort(int a_wd);                               // 0 = Sunday: "日" / "Su"
	const char* WeekdayLong(int a_wd);                                // "星期日" / "Sunday"
}

// translate a phone UI string (Chinese source text); unknown strings are returned unchanged
const char* Tr(const char* a_zh);
