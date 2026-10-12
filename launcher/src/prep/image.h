// SPDX-License-Identifier: GPL-2.0-or-later
// The game image for wbp-game: a plaintext PS4 SELF (eboot.bin) linked with the game's own
// libc.prx and libSceFios2.prx into out/boot-linked.bin (format BBPROBE5, read by src/probe.c).
#pragma once
#include <map>
#include <string>

#include "../util.h"

namespace prep {

// param.sfo values: numbers as decimal text.
std::map<std::string, std::string> ReadSfo(const fs::path& path);
// APP_VER of a game folder ("01.09"), or empty when it cannot be read.
std::string GameVersion(const fs::path& game);

// Writes out/boot-linked.bin. Skipped when the inputs have not changed since the last time
// (out/boot-linked.key); prints what it did.
void BuildImage(const fs::path& game, const fs::path& out);

// [start, end) of the eboot's PT_LOAD segments (virtual addresses), from its headers only.
std::vector<std::pair<uint64_t, uint64_t>> EbootLoadRanges(const fs::path& game);

// What keeps a game folder from running even though it is version 1.09, or empty:
// "patched_eboot" (the 1.09 eboot.bin with a 60 FPS patch baked in: the gestures menu crashes)
// or "damaged_files" (the shaders do not unpack: a broken extraction; the game would hang while
// loading them). BB_SKIP_GAME_CHECK=1: always empty.
std::string GameProblem(const fs::path& game);

// out/content.bin: the AppContent profile (SKU and the four user-defined parameters).
void WriteContentProfile(const fs::path& game, const fs::path& out, const std::string& sku);

}  // namespace prep
