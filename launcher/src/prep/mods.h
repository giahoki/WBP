// SPDX-License-Identifier: GPL-2.0-or-later
// Successor of scripts/mods.py: loose-file mods merged over the game through links, without
// changing the game or the mods.
#pragma once
#include <string>
#include <vector>

#include "../util.h"

namespace prep {

// Named mods: folders of `root` with an accepted layout, sorted by name.
std::vector<std::string> DiscoverMods(const fs::path& root);

struct ModOptions {
    fs::path game, out, mods_dir, config;  // config: mods.json {"order": [...], "disabled": [...]}
    bool enabled = true;
    std::vector<fs::path> extra;  // the port's own layers (key icons), below the player's mods
};
// The folder to run the game from: the game itself, or a new overlay under out/ (or beside
// the game) that RemoveOverlay deletes afterwards.
fs::path BuildOverlay(const ModOptions& options);
// Deletes an overlay without ever following its links into the game or the mods.
void RemoveOverlay(const fs::path& overlay);

}  // namespace prep
