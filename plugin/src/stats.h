#pragma once

#include "gallery.h"
#include "ui.h"

#include <string>
#include <vector>

// Player, home ship and location data for the Character / Ship / Star map apps, read through the game's own
// Papyrus native implementations (their VM arguments are unused, so they are called directly). Game thread only.
namespace Stats
{
	std::vector<Ui::InfoRow> Character();
	std::vector<Ui::InfoRow> Ship();
	std::vector<Ui::InfoRow> Location();

	// player level (0 if the player is not ready); used for the level-up message
	int PlayerLevel();

	// where the player is: star system, planet, location, and planet lat / lon on a surface ("GPS")
	Gallery::Geo CurrentGeo();

	std::vector<Ui::Contact> Contacts();             // home ship crew
	bool                     Summon(std::uint32_t a_formID);  // console "<id>.moveto player"
	std::vector<Ui::Blip>    Radar(float a_range);    // loaded actors within a_range metres
	std::vector<Ui::InfoRow> Outposts();              // the player's outpost beacons

	// photo mode: record the location (photo camera position) at the shutter press (Gallery::QueueShotGeo)
	void InstallPhotoModeHook();

	// show a line in the game's HUD notification area (Papyrus Debug.Notification)
	void HudNotify(const std::string& a_utf8);

	// the player's weapon state: 0 sheathed, 1-2 drawing, 3 drawn, 4+ sheathing (Actor +0xFC & 7)
	int WeaponState();
	// put the player's weapon away (Papyrus Actor.SheatheWeapon)
	void SheatheWeapon();

	// play a game UI sound by its Wwise event name ("UIMenuGeneralFocus"; Papyrus WwiseEvent.PlayMenuSound)
	void PlayUiSound(const char* a_id);

	// controller rumble (Papyrus Game.ShakeController); strengths 0..1
	void Rumble(float a_small, float a_big, float a_seconds);
}
