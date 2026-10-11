// SPDX-License-Identifier: GPL-2.0-or-later
// Settings: bbport.ini (the game reads it, the in-game menu edits it) and the launcher's own
// settings.json (passed to the game as environment variables).
#pragma once
#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "util.h"

constexpr const char* VERSION = "1.0";  // GitHub release tags are v<VERSION>
constexpr const char* PATCH_VERSION = "01.09";
constexpr const char* COMMUNITY_SERVER = "https://thehuntersdream.com";
constexpr const char* RELEASES_API = "https://api.github.com/repos/giahoki/WBP/releases/latest";
constexpr const char* RELEASES_PAGE = "https://github.com/giahoki/WBP/releases/latest";

// A label in English and Russian (other languages through i18n).
struct Text {
    const char* en;
    const char* ru = nullptr;
    std::string operator()() const;
};

struct Flag {
    const char* key;
    Text title;
    bool on;
};
extern const std::vector<Flag> EFFECTS, EXTRAS, CHEATS, TWEAKS;

struct Choice {
    const char* value;
    Text title;
};
extern const std::vector<Choice> UPSCALERS, PRESETS, OUTPUTS, LIVE, INPUT_MODES, LODS, FPS_MODES, PRESENT_MODES, LANGUAGES,
    DRAW_PIPE, READBACKS, FRAME_CAPS, FRAMES_AHEAD;

struct KeyAction {
    const char* action;
    Text title;
    const char* defaults;  // bindings separated by '|'
};
struct KeyGroup {
    Text title;
    std::vector<KeyAction> actions;
};
extern const std::vector<KeyGroup> KEY_BINDINGS;
struct KeyLayout {
    Text title;
    std::vector<std::pair<const char*, const char*>> bindings;  // action -> bindings, over the defaults
};
extern const std::vector<KeyLayout> KEY_LAYOUTS;
std::map<std::string, std::vector<std::string>> LoadBindings(const std::map<std::string, std::string>& ini);

// bbport.ini: values over the defaults; saving rewrites the known keys in place and keeps
// comments and unknown keys.
struct Ini {
    std::map<std::string, std::string> values;
    std::vector<std::string> lines;
    static fs::path Path();
    static const std::map<std::string, std::string>& Defaults();
    static bool IsFlag(const std::string& key);
    void Load();
    void Save();
    std::string Get(const std::string& key) const;
    bool On(const std::string& key) const { return Get(key) == "1"; }
};

// settings.json in %APPDATA%/bbport-launcher.
struct App {
    nlohmann::json values;
    static const nlohmann::json& Defaults();
    static fs::path Path();
    void Load();
    void Save() const;
    std::string Str(const char* key) const;
    bool On(const char* key) const;
    void Set(const char* key, nlohmann::json value) { values[key] = std::move(value); }
};

// The game folder's title and version, or nothing without eboot.bin.
struct GameInfo {
    std::string title, version;
};
std::optional<GameInfo> ReadGameInfo(const fs::path& folder);

fs::path UserDir(const App& app);       // saves, caches, last_run.log
fs::path ModsDir(const App& app);
fs::path PatchesDir(const App& app);

// Online helpers (run.py / the launcher).
std::string WebapiAddress(const std::string& server);
std::string CommunityAddress(std::string value);
std::string WebapiSetting(const App& app);

// Environment of a launch: the saved settings as BB_* variables.
EnvBlock GameEnvironment(const App& app);

// FSR 4 v07 assets (tools/fetch_fsr4_assets.sh): file names and what is missing.
constexpr const char* FSR4_BASE =
    "https://raw.githubusercontent.com/FireBurn/Q2RTX/ae8d628fae208813172446d1e49ed94150b04658/baseq2/fsr4_shaders";
std::vector<std::string> Fsr4Files();
std::vector<std::string> Fsr4Missing();
