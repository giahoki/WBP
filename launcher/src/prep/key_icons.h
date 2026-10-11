// SPDX-License-Identifier: GPL-2.0-or-later
// Successor of scripts/key_icons.py: keyboard and mouse button prompts in the game. Redraws the
// key-guide icons (KG_*) of menu/common.tpf.dcx and the world prompts of the HUD atlas from the
// bindings and writes the archive as a mod layer. The game files are not changed.
#pragma once
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "../util.h"

namespace prep {

using Bindings = std::map<std::string, std::vector<std::string>>;  // action -> bindings

// The layer folder (out/key_icons/<hash>, cached by the bindings), or nothing to add.
std::optional<fs::path> BuildKeyIcons(const fs::path& game, const fs::path& out, const Bindings& bindings);

}  // namespace prep
