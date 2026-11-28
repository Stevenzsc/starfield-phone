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

	// show a line in the game's HUD notification area (Papyrus Debug.Notification)
	void HudNotify(const std::string& a_utf8);
}
