#pragma once

#include <string>
#include <vector>

// Music app: plays the user's own audio files (mp3 / wav / wma, also m4a / aac / flac where Windows has a decoder)
// from Documents\My Games\Starfield\PhoneMusic through MCI (winmm). The music keeps playing while the phone is put
// away. Every call is non-blocking: commands go to a worker thread, which owns the MCI device.
// This file does not use CommonLibSF. Thread-safe.
namespace Music
{
	struct Status
	{
		std::vector<std::string> titles;  // file names without extension, sorted
		int                      index = -1;  // current track, -1 = none loaded
		bool                     playing = false;
		int                      posMs = 0, lenMs = 0;
		int                      volume = 60;  // percent
		std::string              folder;       // display path of the music folder
		std::string              error;        // last error ("无法播放 xxx"), empty if none
	};

	Status   Get();
	unsigned Seq();  // bumped on every change and twice a second while playing (redraw trigger)

	void Rescan();          // re-read the folder (the Music page calls it when it opens)
	void Toggle();          // play / pause; starts the first track if none is loaded
	void Next();
	void Prev();            // back to the start of the track, or the previous one in the first 3 s
	void SetVolume(int a_percent);

	// play quieter for a while (1 = normal, 0.1 = a tenth, e.g. during dialogue); fades over about half a second
	void SetDuck(float a_level);
}
