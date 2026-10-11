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

// Native input with dodge and menu back on different keys: the circle icon (KG_OK) shows menu
// back, and dodge gets its own icon, KG_Start, which the floor-message movie imports but no text
// uses. The game's <?CancelIcon?> (only in the dodge and run tutorials) is then pointed at
// KG_Start by the runtime (BB_DODGE_ICON, src/runtime_input.c).
constexpr const char* DODGE_ICON = "KG_Start";
bool DodgeIconSplit(const Bindings& bindings, bool native);

// The layer folder (out/key_icons/<hash>, cached by the bindings), or nothing to add. native:
// input_mode=native.
std::optional<fs::path> BuildKeyIcons(const fs::path& game, const fs::path& out, const Bindings& bindings,
                                      bool native);

}  // namespace prep
