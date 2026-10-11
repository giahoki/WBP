// SPDX-License-Identifier: GPL-2.0-or-later
// Successor of scripts/patches.py: compiles the selected shadPS4/GoldHEN XML patches into
// out/patches.bin (BBPATCH2, read by src/probe.c). Only literal writes are supported.
#pragma once
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "../util.h"

namespace prep {

using Settings = std::map<std::string, std::string>;
Settings ReadSettings(const fs::path& ini);

struct Size {
    int w = 0, h = 0;
    std::string Text() const { return std::to_string(w) + "x" + std::to_string(h); }
    bool operator==(const Size&) const = default;
};
std::optional<Size> ParseSize(std::string_view text);

// (render, output) for an output other than 1080p (patches.py scaled_sizes), or nothing.
std::optional<std::pair<Size, Size>> ScaledSizes(const Settings& settings);

struct PatchOptions {
    fs::path xml;            // patches/Bloodborne.xml
    fs::path out;            // out/ (patches.bin)
    fs::path game;           // game folder (version, debug fonts, eboot segments)
    fs::path settings;       // wbp.ini
    std::string fps = "uncap";
    std::string extra;       // more patch names separated by ';'
    std::string render_res;  // WxH, overrides the preset
    fs::path patches_dir;    // third-party XML files
    fs::path patches_config; // patches.json
};
void CompilePatches(const PatchOptions& options);

// Third-party patches for 1.09 in a folder: key "<file>/<name>", author, isEnabled.
struct ExternalPatch {
    std::string key, author;
    bool enabled_by_file = false;
};
std::vector<ExternalPatch> ListExternalPatches(const fs::path& directory, const fs::path& builtin_xml);

}  // namespace prep
