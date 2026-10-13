// SPDX-License-Identifier: GPL-2.0-or-later
#include "patches.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <set>

#include <nlohmann/json.hpp>
#include <pugixml.hpp>

#include "image.h"

namespace prep {
namespace {

constexpr uint64_t EBOOT_BASE = 0x400000;
constexpr const char* APP_VERSION = "01.09";
const Size OUTPUT_SIZE{1920, 1080};
const double PRESET_SCALES[] = {1.0, 1.5, 1.7, 2.0, 3.0};
constexpr const char* RESOLUTION_TEMPLATE = "Resolution Patch 1280x720 (16:9)";

// BB_FPS presets: patch names from patches/Bloodborne.xml (app version 01.09).
const std::map<std::string, std::vector<std::string>> FPS_PRESETS = {
    {"30", {}}, {"60", {"60 FPS++"}}, {"90", {"90 FPS++"}}, {"uncap", {"Uncap FPS++"}}};

// wbp.ini key -> (patch when the key is 0, patch when it is 1).
struct Effect {
    const char *key, *off, *on;
};
const Effect EFFECTS[] = {
    {"effect_chromatic_aberration", "Disable Chromatic Aberration", nullptr},
    {"effect_dof", "Disable DoF", nullptr},
    {"effect_motion_blur", "Disable Motion Blur (perf increase)", nullptr},
    {"effect_ssao", "Disable SSAO", nullptr},
    {"effect_game_aa", "Disable AA", nullptr},
    {"effect_dynamic_shadows", "Disable Dynamic Light Shadows (perf increase)", nullptr},
    {"effect_ssr", nullptr, "Enable Screen Space Reflections (READ NOTE)"},
    {"skip_intro", nullptr, "Skip Intro"},
    {"debug_camera", nullptr, "Restore Debug Camera"},
    {"debug_menu", nullptr, "Restore Debug Menu (READ NOTES)"},
    {"cheat_no_death", nullptr, "Player No Dead (Read note)"},
    {"cheat_stealth", nullptr, "Player Stealth (Read note)"},
    {"cheat_silent", nullptr, "Player Silent (Read note)"},
    {"cheat_rally_no_decay", nullptr, "No Rally Decay"},
    {"cheat_enemy_control", nullptr, "Enemy Control"},
    {"tweak_no_rally", nullptr, "Disable Rally (HP Regain)"},
    {"tweak_camera_distance", nullptr, "Increased camera distance"},
    {"tweak_no_camera_rotation", nullptr, "Disable Camera Auto Rotation via Movement"},
    {"tweak_easy_run", nullptr, "Sensitive Analog Input (easier to run)"},
    {"tweak_ragdoll", nullptr, "DS1-like physics"},
    {"tweak_no_wind", nullptr, "Disable Wind (cloth, hair)"},
};
const std::map<std::string, std::string> MODEL_LOD = {
    {"-2", "Model LOD -2 (Highest)"}, {"1", "Model LOD 1 (Lower)"}, {"2", "Model LOD 2 (Lowest)"}};

constexpr uint64_t SCENE_WIDTH = 0x02196A6B - EBOOT_BASE, SCENE_HEIGHT = 0x02196A7A - EBOOT_BASE,
                   UI_WIDTH = 0x02358554 - EBOOT_BASE, UI_HEIGHT = 0x0235855D - EBOOT_BASE;

// The retail releases (US EU UK JP AS), then the editions with The Old Hunters (US EU JP AS): all
// run the same 1.09 executable.
const std::set<std::string> BLOODBORNE_IDS = {"CUSA00900", "CUSA00207", "CUSA00208", "CUSA00299", "CUSA01363",
                                              "CUSA03179", "CUSA03173", "CUSA03014", "CUSA03023"};

using Ranges = std::vector<std::pair<uint64_t, uint64_t>>;
using Writes = std::vector<std::pair<uint64_t, Bytes>>;

uint64_t ParseInt(const std::string& text) {
    std::string t = Trim(text);
    if (t.empty()) throw Failure("empty number");
    size_t used = 0;
    unsigned long long value;
    try {
        value = std::stoull(t, &used, 0);
    } catch (...) {
        throw Failure("bad number " + t);
    }
    if (used != t.size()) throw Failure("bad number " + t);
    return value;
}

Bytes Encode(const pugi::xml_node& line) {
    const std::string kind = line.attribute("Type").as_string(), value = line.attribute("Value").as_string();
    Bytes out;
    if (kind == "bytes") {
        std::string hex;
        for (char c : value)
            if (c != ' ') hex += c;
        if (hex.size() % 2) throw Failure("odd hex string");
        for (size_t i = 0; i < hex.size(); i += 2) {
            try {
                size_t used = 0;
                int b = std::stoi(hex.substr(i, 2), &used, 16);
                if (used != 2) throw 0;
                out.push_back(uint8_t(b));
            } catch (...) {
                throw Failure("bad hex " + value);
            }
        }
        return out;
    }
    if (kind == "bytes16" || kind == "bytes32" || kind == "bytes64") {
        const int width = std::stoi(kind.substr(5)) / 8;
        uint64_t v = ParseInt(value);
        if (width < 8 && (v >> (width * 8))) throw Failure("value too large: " + value);
        for (int i = 0; i < width; ++i) out.push_back(uint8_t(v >> (8 * i)));
        return out;
    }
    if (kind == "float32") {
        float f = std::stof(value);
        AppendBytes(out, &f, 4);
        return out;
    }
    if (kind == "float64") {
        double d = std::stod(value);
        AppendBytes(out, &d, 8);
        return out;
    }
    if (kind == "utf8") {
        out.assign(value.begin(), value.end());
        out.push_back(0);
        return out;
    }
    if (kind == "utf16") {
        std::wstring w = Wide(value);
        AppendBytes(out, w.data(), w.size() * 2);
        out.push_back(0);
        out.push_back(0);
        return out;
    }
    throw Failure("unsupported patch type '" + kind + "'");
}

bool Inside(const Ranges& segments, uint64_t offset, size_t size) {
    return std::any_of(segments.begin(), segments.end(),
                       [&](auto& s) { return s.first <= offset && offset + size <= s.second; });
}

template <typename F>
void EachDescendant(const pugi::xml_node& node, const char* name, F&& f) {
    for (pugi::xml_node child : node.children()) {
        if (std::strcmp(child.name(), name) == 0) f(child);
        EachDescendant(child, name, f);
    }
}

pugi::xml_document LoadXml(const fs::path& path) {
    pugi::xml_document doc;
    pugi::xml_parse_result result = doc.load_file(path.c_str());
    if (!result) throw Failure(PathText(path.filename()) + ": " + result.description());
    return doc;
}

Writes Compile(const pugi::xml_document& doc, const std::vector<std::string>& names, const Ranges& segments) {
    std::map<std::string, pugi::xml_node> found;
    EachDescendant(doc, "Metadata", [&](pugi::xml_node meta) {
        const std::string name = meta.attribute("Name").as_string();
        if (std::find(names.begin(), names.end(), name) != names.end() &&
            std::string(meta.attribute("AppVer").as_string()) == APP_VERSION &&
            std::string(meta.attribute("AppElf").as_string("eboot.bin")) == "eboot.bin")
            found[name] = meta;
    });
    std::string missing;
    for (auto& n : names)
        if (!found.count(n)) missing += (missing.empty() ? "" : ", ") + n;
    if (!missing.empty()) throw Failure(std::string("patches not found for app version ") + APP_VERSION + ": " + missing);
    Writes writes;
    for (auto& n : names) {
        EachDescendant(found[n], "Line", [&](pugi::xml_node line) {
            const uint64_t offset = ParseInt(line.attribute("Address").as_string()) - EBOOT_BASE;
            Bytes data = Encode(line);
            if (!Inside(segments, offset, data.size()))
                throw Failure(n + ": address " + line.attribute("Address").as_string() + " is outside the eboot");
            writes.push_back({offset, std::move(data)});
        });
    }
    return writes;
}

Writes ResolutionWrites(const pugi::xml_document& doc, Size size, const Ranges& segments, Size ui) {
    Writes writes = Compile(doc, {RESOLUTION_TEMPLATE}, segments);
    struct R {
        uint8_t opcode;
        uint32_t old, value;
    };
    std::map<uint64_t, R> replacements = {{SCENE_WIDTH, {0xB8, 0x500, uint32_t(size.w)}},
                                          {SCENE_HEIGHT, {0xB8, 0x2D0, uint32_t(size.h)}},
                                          {UI_WIDTH, {0xB8, 0x500, uint32_t(ui.w)}},
                                          {UI_HEIGHT, {0xB9, 0x2D0, uint32_t(ui.h)}}};
    std::set<uint64_t> seen;
    for (auto& [offset, data] : writes) {
        auto it = replacements.find(offset);
        if (it == replacements.end()) continue;
        const R& r = it->second;
        Bytes expected = {r.opcode, uint8_t(r.old), uint8_t(r.old >> 8), uint8_t(r.old >> 16)};
        if (data != expected || seen.count(offset))
            throw Failure("unexpected resolution patch at " + Hex(offset + EBOOT_BASE));
        data = {r.opcode, uint8_t(r.value), uint8_t(r.value >> 8), uint8_t(r.value >> 16)};
        seen.insert(offset);
    }
    if (seen.size() != replacements.size()) throw Failure("resolution patch is missing scene/UI viewport instructions");
    return writes;
}

std::vector<std::string> EffectPatches(const Settings& settings) {
    std::vector<std::string> names;
    for (const Effect& e : EFFECTS) {
        auto it = settings.find(e.key);
        if (it == settings.end()) continue;
        const char* name = it->second == "1" ? e.on : e.off;
        if (name) names.push_back(name);
    }
    auto has = [&](const char* n) { return std::find(names.begin(), names.end(), n) != names.end(); };
    auto drop = [&](const char* n) { names.erase(std::remove(names.begin(), names.end(), n), names.end()); };
    // Enemy Control and the free camera use the same buttons: the free camera wins.
    if (has("Enemy Control") && has("Restore Debug Camera")) {
        drop("Enemy Control");
        Say("Patches: Enemy Control skipped (it conflicts with the free camera)");
    }
    // Rally that never decays means nothing without Rally.
    if (has("No Rally Decay") && has("Disable Rally (HP Regain)")) drop("No Rally Decay");
    auto lod = settings.find("model_lod");
    auto name = MODEL_LOD.find(lod == settings.end() ? "0" : lod->second);
    if (name != MODEL_LOD.end()) names.push_back(name->second);
    auto shadows = settings.find("shadow_quality");
    if (shadows != settings.end() && shadows->second == "low") names.push_back("Shadow Resolution 1024 (perf increase)");
    return names;
}

void ValidateRequirements(const std::vector<std::string>& names, const fs::path& game) {
    auto has = [&](const char* n) { return std::find(names.begin(), names.end(), n) != names.end(); };
    if (has("Restore Debug Camera") && has("Enemy Control"))
        throw Failure("Restore Debug Camera conflicts with Enemy Control; enable only one");
    if (has("Restore Debug Menu (READ NOTES)")) {
        const fs::path font = game / L"dvdroot_ps4" / L"font";
        std::string missing;
        for (const wchar_t* name : {L"DbgFont14h.ccm", L"DbgFont14h.tpf"}) {
            std::error_code ec;
            if (!fs::is_regular_file(font / name, ec) || fs::file_size(font / name, ec) == 0)
                missing += (missing.empty() ? "" : ", ") + Utf8(name);
        }
        if (!missing.empty())
            throw Failure("Debug menu needs non-empty font files in " + PathText(font) + ": " + missing +
                          ". Install the fonts from https://www.nexusmods.com/bloodborne/mods/253 first; "
                          "or disable debug_menu in wbp.ini");
    }
}

struct External {
    std::string key, author;
    bool enabled_by_file;
    std::shared_ptr<pugi::xml_document> doc;
    pugi::xml_node meta;
};

std::vector<External> FindExternal(const fs::path& directory, const fs::path& builtin) {
    std::vector<External> found;
    std::error_code ec;
    if (!fs::is_directory(directory, ec)) return found;
    std::vector<fs::path> files;
    for (auto& entry : fs::directory_iterator(directory, ec))
        if (entry.is_regular_file() && Lower(PathText(entry.path().extension())) == ".xml") files.push_back(entry.path());
    std::sort(files.begin(), files.end());
    for (const fs::path& path : files) {
        if (fs::equivalent(path, builtin, ec)) continue;  // the built-in file (patches/ in a checkout)
        auto doc = std::make_shared<pugi::xml_document>();
        pugi::xml_parse_result result = doc->load_file(path.c_str());
        if (!result) {
            SayError("Patches: " + PathText(path.filename()) + ": " + result.description());
            continue;
        }
        std::set<std::string> ids;
        EachDescendant(*doc, "ID", [&](pugi::xml_node id) {
            std::string text = Trim(id.text().as_string());
            if (!text.empty()) ids.insert(text);
        });
        if (!ids.empty() && std::none_of(ids.begin(), ids.end(), [](auto& i) { return BLOODBORNE_IDS.count(i) > 0; }))
            continue;
        EachDescendant(*doc, "Metadata", [&](pugi::xml_node meta) {
            if (std::string(meta.attribute("AppVer").as_string()) == APP_VERSION &&
                std::string(meta.attribute("AppElf").as_string("eboot.bin")) == "eboot.bin")
                found.push_back({PathText(path.filename()) + "/" + meta.attribute("Name").as_string(),
                                 meta.attribute("Author").as_string(),
                                 Lower(meta.attribute("isEnabled").as_string("false")) == "true", doc, meta});
        });
    }
    return found;
}

}  // namespace

Settings ReadSettings(const fs::path& ini) {
    Settings settings;
    auto text = ReadText(ini);
    if (!text) return settings;
    for (std::string line : Split(*text, '\n')) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t eq = line.find('=');
        if (eq == std::string::npos || StartsWith(line, "#")) continue;
        settings[Trim(line.substr(0, eq))] = Trim(line.substr(eq + 1));
    }
    return settings;
}

std::optional<Size> ParseSize(std::string_view text) {
    auto parts = Split(Lower(Trim(text)), 'x');
    if (parts.size() != 2) return std::nullopt;
    try {
        Size s{std::stoi(parts[0]), std::stoi(parts[1])};
        if (s.w > 0 && s.h > 0) return s;
    } catch (...) {
    }
    return std::nullopt;
}

std::optional<std::pair<Size, Size>> ScaledSizes(const Settings& settings) {
    auto get = [&](const char* k, const char* d) {
        auto it = settings.find(k);
        return it == settings.end() ? std::string(d) : it->second;
    };
    Size out = ParseSize(get("output_res", "")).value_or(OUTPUT_SIZE);
    if (out == OUTPUT_SIZE || get("upscaler", "") == "taa") return std::nullopt;
    double scale = 1.0;
    if (get("upscaler", "fsr3") != "off") {
        int preset = 0;
        try {
            preset = std::stoi(get("preset", "0").empty() ? "0" : get("preset", "0"));
        } catch (...) {
        }
        scale = PRESET_SCALES[std::clamp(preset, 0, 4)];
    }
    // Python's round(): half to even.
    auto even = [&](int v) { return std::max(2, int(std::nearbyint(v / scale / 2)) * 2); };
    Size render{even(out.w), even(out.h)};
    // A scene of exactly 1920x1080 (4K Performance) looks like the game's UI space.
    if (render == OUTPUT_SIZE) render = {1916, 1078};
    return std::pair{render, out};
}

void CompilePatches(const PatchOptions& o) {
    const fs::path target = o.out / L"patches.bin";
    auto blob = [](const Writes& writes) {
        Bytes data;
        AppendBytes(data, "BBPATCH2", 8);
        Append64(data, EBOOT_BASE);
        Append64(data, writes.size());
        for (auto& [offset, bytes] : writes) {
            Append64(data, offset);
            Append64(data, bytes.size());
            AppendBytes(data, bytes.data(), bytes.size());
        }
        return data;
    };
    // The patches are byte writes at the addresses of one game version: on another version they
    // would corrupt code. Such a game runs unpatched at 30 FPS.
    const std::string version = GameVersion(o.game);
    if (!version.empty() && version != APP_VERSION && !HasEnv("BB_FORCE_PATCHES")) {
        WriteFile(target, blob({}));
        Say("Patches: game version " + version + ", patches are for " + APP_VERSION +
            ": none applied (30 FPS, no effect or resolution patches)");
        return;
    }
    auto preset = FPS_PRESETS.find(o.fps);
    if (preset == FPS_PRESETS.end()) throw Failure("unknown frame rate mode " + o.fps);
    std::vector<std::string> names = preset->second;
    for (auto& n : Split(o.extra, ';'))
        if (!Trim(n).empty()) names.push_back(Trim(n));
    const Settings settings = ReadSettings(o.settings);
    for (auto& n : EffectPatches(settings))
        if (std::find(names.begin(), names.end(), n) == names.end()) names.push_back(n);
    auto add = [&](const char* n) {
        if (std::find(names.begin(), names.end(), n) == names.end()) names.push_back(n);
    };
    auto env = [](const char* name) {
        const char* v = std::getenv(name);
        return std::string(v ? v : "");
    };
    // Above ~90 FPS the game's wall detector (a per-frame distance rule) halves sprint speed.
    if (o.fps == "90" || o.fps == "uncap") add("Sprint Fix (High FPS)");
    // Intel CPUs: the game code runs natively and Intel's approximate float instructions differ
    // from the PS4's AMD CPU, so a tone mapping block gated on a computed flag never runs (black
    // colors, mostly in DLC areas). On by default on Intel; BB_INTEL_TONEMAP_FIX=0/1 forces it.
    const std::string intel_fix = env("BB_INTEL_TONEMAP_FIX");
    if (intel_fix == "1" ||
        (intel_fix != "0" && env("PROCESSOR_IDENTIFIER").find("GenuineIntel") != std::string::npos))
        add("Intel Black Tonemap Fix");
    // The title's PLAY ONLINE / PLAY OFFLINE dialog: offline play goes straight to the main menu.
    // Kept for co-op (BB_ONLINE=1); BB_SKIP_NETWORK_CHOICE=0 always shows it.
    if (env("BB_ONLINE") != "1" && env("BB_SKIP_NETWORK_CHOICE") != "0")
        add("Skip Online/Offline Choice");
    ValidateRequirements(names, o.game);
    const Ranges segments = EbootLoadRanges(o.game);
    const pugi::xml_document doc = LoadXml(o.xml);
    Writes writes = Compile(doc, names, segments);
    if (!o.render_res.empty()) {
        auto size = ParseSize(o.render_res);
        if (!size) throw Failure("bad render resolution " + o.render_res);
        // The UI keeps the game's 1920x1080 coordinates even for a larger output.
        Writes res = ResolutionWrites(doc, *size, segments, OUTPUT_SIZE);
        writes.insert(writes.end(), res.begin(), res.end());
        if (size->w * size->h > OUTPUT_SIZE.w * OUTPUT_SIZE.h) {
            const std::string heap = "Increased Graphics Heap Sizes";
            Writes more = Compile(doc, {heap}, segments);
            writes.insert(writes.end(), more.begin(), more.end());
            names.push_back(heap);
        }
        Say("Patches: scene " + size->Text() + "; UI 1920x1080");
    }
    if (!o.patches_dir.empty()) {
        // After the built-in ones: an external patch of the same bytes wins.
        nlohmann::json config = nlohmann::json::object();
        if (auto text = ReadText(o.patches_config)) {
            try {
                config = nlohmann::json::parse(*text);
            } catch (...) {
                SayError("Patches: patches.json is not valid JSON; ignored");
            }
        }
        std::set<std::string> enabled, disabled;
        if (config.is_object()) {
            for (auto& v : config.value("enabled", nlohmann::json::array())) if (v.is_string()) enabled.insert(v.get<std::string>());
            for (auto& v : config.value("disabled", nlohmann::json::array())) if (v.is_string()) disabled.insert(v.get<std::string>());
        }
        for (External& e : FindExternal(o.patches_dir, o.xml)) {
            if (!(enabled.count(e.key) || (!disabled.count(e.key) && e.enabled_by_file))) continue;
            try {
                Writes ours;
                EachDescendant(e.meta, "Line", [&](pugi::xml_node line) {
                    const uint64_t offset = ParseInt(line.attribute("Address").as_string()) - EBOOT_BASE;
                    Bytes data = Encode(line);
                    if (!Inside(segments, offset, data.size()))
                        throw Failure(std::string("address ") + line.attribute("Address").as_string() + " is outside the eboot");
                    ours.push_back({offset, std::move(data)});
                });
                Say("Patches: external " + e.key + " (" + std::to_string(ours.size()) + " writes)");
                writes.insert(writes.end(), ours.begin(), ours.end());
            } catch (const std::exception& error) {
                SayError("Patches: skipped " + e.key + ": " + error.what());
            }
        }
    }
    WriteFile(target, blob(writes));
    std::string list;
    for (auto& n : names) list += (list.empty() ? "'" : ", '") + n + "'";
    Say("Patches: FPS preset " + o.fps + "; " + std::to_string(writes.size()) + " writes from " +
        (names.empty() ? std::string("none") : "[" + list + "]"));
}

std::vector<ExternalPatch> ListExternalPatches(const fs::path& directory, const fs::path& builtin_xml) {
    std::vector<ExternalPatch> out;
    for (External& e : FindExternal(directory, builtin_xml)) out.push_back({e.key, e.author, e.enabled_by_file});
    return out;
}

}  // namespace prep
