// SPDX-License-Identifier: GPL-2.0-or-later
#include "settings.h"

#include <algorithm>
#include <optional>

#include "i18n.h"
#include "prep/image.h"

std::string Text::operator()() const { return T(en, ru); }

const std::vector<Flag> EFFECTS = {
    {"effect_chromatic_aberration", {"Chromatic aberration", "Хроматическая аберрация"}, true},
    {"effect_dof", {"Depth of field", "Глубина резкости (DoF)"}, true},
    {"effect_motion_blur", {"Motion blur", "Размытие в движении"}, true},
    {"effect_ssao", {"Ambient occlusion (SSAO)", "Затенение SSAO"}, true},
    {"effect_game_aa", {"The game's own anti-aliasing", "Собственное сглаживание игры"}, true},
    {"effect_dynamic_shadows", {"Shadows of dynamic lights", "Тени от динамических источников"}, true},
    {"effect_ssr", {"Screen-space reflections (not in the original)", "Отражения SSR (не было в игре)"}, false},
};
const std::vector<Flag> EXTRAS = {
    {"skip_intro", {"Skip the intro logos and movie", "Пропуск заставок при запуске"}, false},
    {"debug_camera", {"Free camera (hold Interact and press L3: E + F)",
                      "Свободная камера (удерживать «Взаимодействие» и нажать L3: E + F)"}, false},
    {"debug_menu", {"Game debug menu (the Gestures key, G; needs the debug fonts)",
                    "Debug menu игры (клавиша жестов, G; нужны шрифты)"}, false},
};
const std::vector<Flag> CHEATS = {
    {"cheat_no_death", {"Never die (health stops at 1 HP)", "Бессмертие (здоровье не ниже 1 HP)"}, false},
    {"cheat_stealth", {"Enemies do not see you (unless attacked)", "Враги не видят вас (пока не атакованы)"}, false},
    {"cheat_silent", {"Enemies do not hear you", "Враги не слышат вас"}, false},
    {"cheat_rally_no_decay", {"Rally never fades", "Rally не угасает"}, false},
    {"cheat_enemy_control", {"Control the targeted enemy (Lock on, Q, takes control; L3, F, goes back; not with the free camera)",
                             "Управление выбранным врагом («Захват цели», Q — взять; L3, F — вернуться; не вместе со свободной камерой)"},
     false},
};
const std::vector<Flag> TWEAKS = {
    {"tweak_no_rally", {"No Rally (hits do not give health back)", "Без Rally (удары не возвращают здоровье)"}, false},
    {"tweak_camera_distance", {"Camera further from the character", "Камера дальше от персонажа"}, false},
    {"tweak_no_camera_rotation", {"No camera auto-rotation while moving", "Без автоповорота камеры при движении"}, false},
    {"tweak_easy_run", {"Run with less stick tilt (about 70%)", "Бег при меньшем наклоне стика (около 70%)"}, false},
    {"tweak_ragdoll", {"Dark Souls-style ragdoll physics (corpses fly further)", "Физика тел как в Dark Souls (тела отлетают дальше)"}, false},
};

const std::vector<Choice> UPSCALERS = {{"dlss", {"DLSS (NVIDIA GeForce RTX)"}},
                                       {"fsr4", {"FSR 4 (best quality)", "FSR 4 (лучшее качество)"}},
                                       {"fsr411", {"FSR 4.1.1 (needs fsr4_411 assets)", "FSR 4.1.1 (нужны ассеты fsr4_411)"}},
                                       {"fsr3", {"FSR 3.1 (every GPU)", "FSR 3.1 (любая видеокарта)"}},
                                       {"taa", {"TAA (native resolution anti-aliasing)", "TAA (нативное сглаживание)"}},
                                       {"off", {"Off", "Выключен"}}};
const std::vector<Choice> PRESETS = {{"0", {"Native AA (×1.0)"}}, {"1", {"Quality (×1.5)"}}, {"2", {"Balanced (×1.7)"}},
                                     {"3", {"Performance (×2)"}}, {"4", {"Ultra Performance (×3)"}}};
const std::vector<Choice> OUTPUTS = {{"1280x720", {"1280 × 720 (Steam Deck)"}}, {"1920x1080", {"1920 × 1080"}},
                                     {"2560x1440", {"2560 × 1440"}}, {"3840x2160", {"3840 × 2160 (4K)"}}};
const std::vector<Choice> LIVE = {{"auto", {"Auto (by graphics card)", "Авто (по видеокарте)"}},
                                  {"0", {"Off (faster)", "Выключена (быстрее)"}},
                                  {"1", {"On (change without restarting)", "Включена (без перезапуска)"}}};
const std::vector<Choice> INPUT_MODES = {
    {"native", {"Native", "Нативный"}},
    {"emulation", {"Gamepad emulation", "Эмуляция геймпада"}}};
const std::vector<Choice> LODS = {{"0", {"As in the game", "Как в игре"}}, {"-2", {"Highest (−2)", "Максимальная (−2)"}},
                                  {"1", {"Lower (1)", "Ниже (1)"}}, {"2", {"Lowest (2)", "Минимальная (2)"}}};
const std::vector<Choice> FPS_MODES = {{"uncap", {"Unlocked (frame-time patch)", "Без ограничения (патч)"}},
                                       {"60", {"60 FPS"}}, {"90", {"90 FPS"}},
                                       {"30", {"30 FPS (as on PS4)", "30 FPS (как на PS4)"}}};
const std::vector<Choice> PRESENT_MODES = {{"Mailbox", {"Mailbox (low latency, no tearing)", "Mailbox (без разрывов)"}},
                                           {"Fifo", {"FIFO (VSync)"}}, {"FifoRelaxed", {"FIFO Relaxed"}},
                                           {"Immediate", {"Immediate (tearing)", "Immediate (с разрывами)"}}};
const std::vector<Choice> LANGUAGES = {{"1", {"English", "Английский"}}, {"8", {"Russian", "Русский"}},
                                       {"0", {"Japanese", "Японский"}},  {"2", {"French", "Французский"}},
                                       {"3", {"Spanish", "Испанский"}},  {"4", {"German", "Немецкий"}},
                                       {"5", {"Italian", "Итальянский"}}};
const std::vector<Choice> DRAW_PIPE = {{"", {"Auto (8+ threads)", "Авто (8+ потоков)"}}, {"1", {"On", "Включён"}},
                                       {"0", {"Off (more stable)", "Выключен (стабильнее)"}}};
const std::vector<Choice> READBACKS = {{"", {"Relaxed (default)", "Relaxed (по умолчанию)"}}, {"0", {"Off", "Выключены"}},
                                       {"2", {"Precise"}}};
// Frame cap of the unlocked mode (BB_FPS_LIMIT). '' leaves the port's own: the display refresh,
// at most 120, because the game's movement timing breaks above about 120 FPS.
const std::vector<Choice> FRAME_CAPS = {
    {"", {"Auto: display refresh, max 120 (recommended)", "Авто: частота монитора, макс. 120 (рекомендуется)"}},
    {"60", {"60"}}, {"90", {"90"}}, {"120", {"120"}}, {"144", {"144  ⚠"}}, {"165", {"165  ⚠"}},
    {"240", {"240  ⚠"}}, {"0", {"No limit  ⚠", "Без ограничения  ⚠"}}};
const std::vector<Choice> FRAMES_AHEAD = {{"", {"1 (default)", "1 (по умолчанию)"}}, {"2", {"2"}},
                                          {"0", {"Unbounded", "Без ограничения"}}};

// Layouts the Controls page offers: the bindings that differ from the defaults (Souls).
const std::vector<KeyLayout> KEY_LAYOUTS = {
    {{"Souls (as on a gamepad)", "Souls (как на геймпаде)"}, {}},
    {{"PC: run on Shift", "PC: бег на Shift"},
     {{"run", "Left Shift"}, {"dodge_run", "Space"}, {"strong_attack", "Ctrl+Mouse1|Mouse5"},
      {"transform", "Ctrl+Mouse2|Mouse4"}}},
};

// src/runtime_pad.c holds the same defaults (actions[]): keep them in step.
const std::vector<KeyGroup> KEY_BINDINGS = {
    {{"Movement", "Движение"},
     {{"move_forward", {"Forward", "Вперёд"}, "W"},
      {"move_back", {"Back", "Назад"}, "S"},
      {"move_left", {"Left", "Влево"}, "A"},
      {"move_right", {"Right", "Вправо"}, "D"},
      {"dodge_run", {"Dodge / run (hold), jump (press again while running)",
                     "Уклонение / бег (удерживать), прыжок (нажать ещё раз на бегу)"}, "Space"},
      {"run", {"Run (hold); dodge while running jumps. Native input", "Бег (удерживать); уклонение на бегу — прыжок. Нативный ввод"},
       ""},
      {"jump", {"L3 (free camera, enemy control)", "L3 (свободная камера, управление врагом)"}, "F"}}},
    {{"Combat", "Бой"},
     {{"attack", {"Attack", "Атака"}, "Mouse1"},
      {"strong_attack", {"Strong attack", "Сильная атака"}, "Shift+Mouse1|Mouse5"},
      {"gun", {"Firearm / parry", "Огнестрел / парирование"}, "Mouse2"},
      {"transform", {"Transform weapon", "Трансформация оружия"}, "Shift+Mouse2|Mouse4"},
      {"lock_on", {"Lock on", "Захват цели"}, "Q|Mouse3"}}},
    {{"Items", "Предметы"},
     {{"blood_vial", {"Blood vial", "Флакон крови"}, "R"},
      {"use_item", {"Use item", "Использовать предмет"}, "C"},
      {"dpad_up", {"D-pad up", "Крестовина вверх"}, "Up|WheelUp"},
      {"dpad_down", {"D-pad down", "Крестовина вниз"}, "Down|WheelDown"},
      {"dpad_left", {"D-pad left (switch firearm)", "Крестовина влево (смена огнестрела)"}, "Left"},
      {"dpad_right", {"D-pad right (switch weapon)", "Крестовина вправо (смена оружия)"}, "Right"}}},
    {{"Menus", "Меню"},
     {{"interact", {"Interact / confirm", "Взаимодействие / подтвердить"}, "E|Return"},
      {"back", {"Back (in menus)", "Назад (в меню)"}, "Backspace"},
      {"menu", {"Game menu", "Меню игры"}, "Escape"},
      {"gestures", {"Gestures", "Жесты"}, "G"},
      {"key_items", {"Key items", "Ключевые предметы"}, "T"}}},
    {{"Camera keys (when the mouse camera is off)", "Камера клавишами (если мышь выключена)"},
     {{"camera_up", {"Camera up", "Камера вверх"}, ""},
      {"camera_down", {"Camera down", "Камера вниз"}, ""},
      {"camera_left", {"Camera left", "Камера влево"}, ""},
      {"camera_right", {"Camera right", "Камера вправо"}, ""}}},
};

std::map<std::string, std::vector<std::string>> LoadBindings(const std::map<std::string, std::string>& ini) {
    std::map<std::string, std::vector<std::string>> out;
    for (const KeyGroup& g : KEY_BINDINGS)
        for (const KeyAction& a : g.actions) {
            auto it = ini.find(std::string("key_") + a.action);
            const std::string value = it == ini.end() ? a.defaults : it->second;
            std::vector<std::string> list;
            for (auto& b : Split(value, '|'))
                if (!Trim(b).empty()) list.push_back(Trim(b));
            out[a.action] = list;
        }
    return out;
}

// ---- bbport.ini --------------------------------------------------------------------------

fs::path Ini::Path() { return HasEnv("BB_CONFIG") ? PathFrom(Env("BB_CONFIG")) : DataDir() / L"bbport.ini"; }

const std::map<std::string, std::string>& Ini::Defaults() {
    static const std::map<std::string, std::string> defaults = [] {
        std::map<std::string, std::string> d = {
            {"upscaler", "fsr3"}, {"preset", "1"}, {"sharpen", "1"}, {"sharpness", "0.50"},
            {"object_motion", "1"}, {"show_fps", "1"}, {"output_res", "1920x1080"}, {"model_lod", "0"},
            {"live_resolution", "auto"}, {"mouse_sensitivity", "1.00"}, {"key_icons", "1"},
            {"input_mode", "native"}};
        for (auto* list : {&EFFECTS, &EXTRAS, &CHEATS, &TWEAKS})
            for (const Flag& f : *list) d[f.key] = f.on ? "1" : "0";
        for (const KeyGroup& g : KEY_BINDINGS)
            for (const KeyAction& a : g.actions) d[std::string("key_") + a.action] = a.defaults;
        return d;
    }();
    return defaults;
}

bool Ini::IsFlag(const std::string& key) {
    static const std::vector<std::string> plain = {"sharpen", "object_motion", "show_fps", "key_icons"};
    if (std::find(plain.begin(), plain.end(), key) != plain.end()) return true;
    for (auto* list : {&EFFECTS, &EXTRAS, &CHEATS, &TWEAKS})
        for (const Flag& f : *list)
            if (key == f.key) return true;
    return false;
}

void Ini::Load() {
    values = Defaults();
    lines.clear();
    if (auto text = ReadText(Path())) {
        lines = Split(*text, '\n');
        for (auto& l : lines)
            if (!l.empty() && l.back() == '\r') l.pop_back();
        if (!lines.empty() && lines.back().empty()) lines.pop_back();
    }
    for (const std::string& line : lines) {
        size_t eq = line.find('=');
        if (eq == std::string::npos || StartsWith(Trim(line), "#")) continue;
        values[Trim(line.substr(0, eq))] = Trim(line.substr(eq + 1));
    }
}

void Ini::Save() {
    std::vector<std::string> out;
    std::map<std::string, bool> written;
    for (const std::string& line : lines) {
        size_t eq = line.find('=');
        if (eq != std::string::npos && !StartsWith(Trim(line), "#")) {
            const std::string key = Trim(line.substr(0, eq));
            if (Defaults().count(key) && values.count(key)) {
                if (written[key]) continue;
                out.push_back(key + "=" + values[key]);
                written[key] = true;
                continue;
            }
        }
        out.push_back(line);
    }
    if (lines.empty()) out.push_back("# bbport settings (in-game menu: Insert / L3+R3)");
    for (auto& [key, value] : Defaults())
        if (!written[key]) out.push_back(key + "=" + Get(key));
    std::string text;
    for (auto& l : out) text += l + "\n";
    WriteText(Path(), text);
    Load();
}

std::string Ini::Get(const std::string& key) const {
    auto it = values.find(key);
    if (it != values.end()) return it->second;
    auto d = Defaults().find(key);
    return d == Defaults().end() ? std::string() : d->second;
}

// ---- settings.json -----------------------------------------------------------------------

const nlohmann::json& App::Defaults() {
    static const nlohmann::json defaults = {
        {"ui_language", ""}, {"game_dir", PathText(PortDir().parent_path() / L"CUSA03173")}, {"user_dir", ""},
        {"mods_dir", ""}, {"mods_enabled", true}, {"patches_dir", ""}, {"language", "1"},
        {"player_name", ""}, {"fullscreen", false}, {"hdr", false}, {"present_mode", "Mailbox"},
        {"fps_mode", "uncap"}, {"frame_cap", ""}, {"draw_pipe", ""}, {"readbacks", ""},
        {"frames_ahead", ""}, {"frame_stats", false}, {"gpu_profile", false},
        {"vk_validation", false}, {"extra_env", ""}, {"close_on_play", false},
        {"check_updates", true}, {"online", false}, {"online_server", "srv.shadps4.net:31313"},
        {"online_webapi", ""}, {"online_npid", ""}, {"online_password", ""}, {"online_upnp", true},
        {"online_community", COMMUNITY_SERVER}};
    return defaults;
}

fs::path App::Path() { return ConfigDir() / L"settings.json"; }

void App::Load() {
    values = Defaults();
    if (auto text = ReadText(Path())) {
        try {
            auto saved = nlohmann::json::parse(*text);
            if (saved.is_object())
                for (auto& [k, v] : saved.items()) values[k] = v;
        } catch (...) {
        }
    }
}

void App::Save() const {
    std::error_code ec;
    fs::create_directories(ConfigDir(), ec);
    WriteText(Path(), values.dump(2));
}

std::string App::Str(const char* key) const {
    auto it = values.find(key);
    if (it == values.end()) return {};
    if (it->is_string()) return it->get<std::string>();
    if (it->is_boolean()) return it->get<bool>() ? "1" : "0";
    if (it->is_number()) return it->dump();
    return {};
}

bool App::On(const char* key) const {
    auto it = values.find(key);
    if (it == values.end()) return false;
    if (it->is_boolean()) return it->get<bool>();
    if (it->is_string()) return !it->get<std::string>().empty() && it->get<std::string>() != "0";
    return false;
}

std::optional<GameInfo> ReadGameInfo(const fs::path& folder) {
    std::error_code ec;
    if (folder.empty() || !fs::is_regular_file(folder / L"eboot.bin", ec)) return std::nullopt;
    try {
        auto sfo = prep::ReadSfo(folder / L"sce_sys" / L"param.sfo");
        std::string title = Trim(Replace(sfo.count("TITLE") ? sfo["TITLE"] : "Bloodborne", "™", ""));
        return GameInfo{title, sfo.count("APP_VER") ? sfo["APP_VER"] : "?"};
    } catch (const Failure&) {
        return GameInfo{"Bloodborne", "?"};
    }
}

fs::path UserDir(const App& app) {
    const std::string dir = app.Str("user_dir");
    return dir.empty() ? DataDir() / L"user" : PathFrom(dir);
}
fs::path ModsDir(const App& app) {
    const std::string dir = app.Str("mods_dir");
    return dir.empty() ? DataDir() / L"mods" : PathFrom(dir);
}
fs::path PatchesDir(const App& app) {
    const std::string dir = app.Str("patches_dir");
    return dir.empty() ? DataDir() / L"patches" : PathFrom(dir);
}

std::string WebapiAddress(const std::string& server) {
    std::string host = server;
    if (std::count(server.begin(), server.end(), ':') == 1) host = server.substr(0, server.rfind(':'));
    return "http://" + host + ":31315";
}

std::string CommunityAddress(std::string value) {
    value = Trim(value);
    if (value.empty()) value = COMMUNITY_SERVER;
    std::string scheme = "https", rest = value;
    size_t sep = value.rfind("://");
    if (sep != std::string::npos) {
        scheme = value.substr(0, sep);
        rest = value.substr(sep + 3);
        if (scheme.empty()) scheme = "https";
    }
    std::string host = Split(Split(rest, '/')[0], ':')[0];  // the game picks the ports
    return scheme + "://" + host;
}

std::string WebapiSetting(const App& app) {
    std::string webapi = Trim(app.Str("online_webapi"));
    const std::string community = CommunityAddress(app.Str("online_community"));
    const std::string community_host = Split(community.substr(community.find("://") + 3), ':')[0];
    if (!webapi.empty() && webapi.find(community_host) != std::string::npos) webapi.clear();
    if (!webapi.empty()) return webapi;
    std::string server = Trim(app.Str("online_server"));
    if (server.empty()) server = App::Defaults()["online_server"].get<std::string>();
    return WebapiAddress(server);
}

EnvBlock GameEnvironment(const App& app) {
    EnvBlock env = EnvBlock::Current();
    env.Set("BB_GAME_DIR", app.Str("game_dir"));
    if (!app.Str("user_dir").empty()) env.Set("BB_USER_DIR", app.Str("user_dir"));
    env.Set("BB_MODS_DIR", PathText(ModsDir(app)));
    env.Set("BB_MODS_CONFIG", PathText(DataDir() / L"mods.json"));
    env.Set("BB_MODS_ENABLED", app.On("mods_enabled") ? "1" : "0");
    env.Set("BB_PATCHES_DIR", PathText(PatchesDir(app)));
    env.Set("BB_PATCHES_CONFIG", PathText(DataDir() / L"patches.json"));
    env.Set("BB_LANGUAGE", app.Str("language"));
    if (!Trim(app.Str("player_name")).empty()) env.Set("BB_USER_NAME", Trim(app.Str("player_name")));
    env.Set("BB_FULLSCREEN", app.On("fullscreen") ? "1" : "0");
    env.Set("BB_PRESENT_MODE", app.Str("present_mode"));
    if (app.On("hdr")) env.Set("BB_HDR", "1");
    env.Set("BB_FPS", app.Str("fps_mode"));
    if (!app.Str("frame_cap").empty()) env.Set("BB_FPS_LIMIT", app.Str("frame_cap"));
    for (auto [key, name] : {std::pair{"draw_pipe", "BB_DRAW_PIPE"}, std::pair{"readbacks", "BB_READBACKS"},
                             std::pair{"frames_ahead", "BB_FRAMES_AHEAD"}})
        if (!app.Str(key).empty()) env.Set(name, app.Str(key));
    for (auto [key, name] : {std::pair{"frame_stats", "BB_FRAME_STATS"}, std::pair{"gpu_profile", "BB_GPU_PROFILE"},
                             std::pair{"vk_validation", "BB_VK_VALIDATION"}})
        if (app.On(key)) env.Set(name, "1");
    for (const std::string& item : Split(app.Str("extra_env"), ' ')) {
        size_t eq = item.find('=');
        if (eq != std::string::npos && eq > 0) env.Set(item.substr(0, eq), item.substr(eq + 1));
    }
    if (app.On("online")) {
        std::string server = Trim(app.Str("online_server"));
        if (server.empty()) server = App::Defaults()["online_server"].get<std::string>();
        env.Set("BB_ONLINE", "1");
        env.Set("BB_SHADNET_SERVER", server);
        env.Set("BB_SHADNET_WEBAPI", WebapiSetting(app));
        env.Set("BB_SHADNET_NPID", Trim(app.Str("online_npid")));
        env.Set("BB_SHADNET_PASSWORD", app.Str("online_password"));
        env.Set("BB_UPNP", app.On("online_upnp") ? "1" : "0");
        env.Set("BB_COMMUNITY_SERVER", CommunityAddress(app.Str("online_community")));
    }
    return env;
}

std::vector<std::string> Fsr4Files() {
    std::vector<std::string> files = {"LICENSE-FSR4-v07.txt", "rcas.spv", "spd_auto_exposure.spv"};
    for (const char* model : {"native", "quality", "balanced", "performance", "ultraperf", "drs"}) {
        const std::string base = std::string("fsr4_model_v07_i8_") + model;
        files.push_back(base + "_initializers.bin");
        files.push_back(base + "_pre_weights.bin");
        files.push_back(base + "_shader_manifest.json");
        for (const char* tier : {"1080", "2160"}) {
            files.push_back(base + "_" + tier + "_pre.spv");
            files.push_back(base + "_" + tier + "_post.spv");
            for (int n = 1; n <= 12; ++n) files.push_back(base + "_" + tier + "_pass" + std::to_string(n) + ".spv");
        }
    }
    return files;
}

std::vector<std::string> Fsr4Missing() {
    std::vector<std::string> missing;
    const fs::path folder = PortDir() / L"fsr4_shaders";
    for (const std::string& name : Fsr4Files()) {
        std::error_code ec;
        const fs::path p = folder / PathFrom(name);
        if (!fs::is_regular_file(p, ec) || fs::file_size(p, ec) == 0) missing.push_back(name);
    }
    return missing;
}
