#pragma once

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
}
