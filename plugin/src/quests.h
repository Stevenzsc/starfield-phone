#pragma once

#include "ui.h"

#include <vector>

// Active quests and their in-progress objectives, read from the game's quest forms (see quests.cpp for the layout).
// Call from the game's task thread.
namespace Quests
{
	std::vector<Ui::QuestEntry> CollectActive(std::size_t a_maxQuests);
}
