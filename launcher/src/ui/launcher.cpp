// SPDX-License-Identifier: GPL-2.0-or-later
#include "launcher.h"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <random>
#include <set>

#include "../i18n.h"
#include "../prep/image.h"
#include "../prep/mods.h"
#include "../prep/patches.h"

using namespace theme;

namespace {

// Graphics presets: every key a preset sets. Defaults (Ini::Defaults) equal "high". The
// upscaler is not part of a preset (it depends on the card, see Recommend).
const std::vector<Choice> GRAPHICS_PRESETS = {
    {"low", {"Low: weak GPUs (GTX 1650, laptops)", "Низкие: слабые видеокарты (GTX 1650, ноутбуки)"}},
    {"medium", {"Medium", "Средние"}},
    {"high", {"High: upscaled from ×1.5", "Высокие: апскейл с ×1.5"}},
    {"ultra", {"Ultra: native resolution", "Ультра: нативное разрешение"}},
    {"max", {"Maximum: native, highest model detail", "Максимум: натив, максимальная детализация"}},
    {"custom", {"Custom", "Свои"}}};
using Values = std::vector<std::pair<const char*, const char*>>;
const Values& PresetValues(const std::string& id) {
    static const Values low = {{"preset", "3"}, {"live_resolution", "0"}, {"frame_generation", "0"},
                               {"object_motion", "0"}, {"model_lod", "1"}, {"shadow_quality", "low"},
                               {"effect_ssao", "0"}, {"effect_dof", "0"}, {"effect_motion_blur", "0"},
                               {"effect_dynamic_shadows", "0"}, {"sharpen", "1"}};
    static const Values medium = {{"preset", "2"}, {"live_resolution", "auto"}, {"frame_generation", "0"},
                                  {"object_motion", "0"}, {"model_lod", "0"}, {"shadow_quality", "high"},
                                  {"effect_ssao", "1"}, {"effect_dof", "1"}, {"effect_motion_blur", "0"},
                                  {"effect_dynamic_shadows", "1"}, {"sharpen", "1"}};
    static const Values high = {{"preset", "1"}, {"live_resolution", "auto"}, {"frame_generation", "0"},
                                {"object_motion", "1"}, {"model_lod", "0"}, {"shadow_quality", "high"},
                                {"effect_ssao", "1"}, {"effect_dof", "1"}, {"effect_motion_blur", "1"},
                                {"effect_dynamic_shadows", "1"}, {"sharpen", "1"}};
    static const Values ultra = {{"preset", "0"}, {"live_resolution", "auto"}, {"frame_generation", "0"},
                                 {"object_motion", "1"}, {"model_lod", "0"}, {"shadow_quality", "high"},
                                 {"effect_ssao", "1"}, {"effect_dof", "1"}, {"effect_motion_blur", "1"},
                                 {"effect_dynamic_shadows", "1"}, {"sharpen", "1"}};
    static const Values max = {{"preset", "0"}, {"live_resolution", "auto"}, {"frame_generation", "0"},
                               {"object_motion", "1"}, {"model_lod", "-2"}, {"shadow_quality", "high"},
                               {"effect_ssao", "1"}, {"effect_dof", "1"}, {"effect_motion_blur", "1"},
                               {"effect_dynamic_shadows", "1"}, {"sharpen", "1"}};
    static const Values none;
    return id == "low" ? low : id == "medium" ? medium : id == "high" ? high : id == "ultra" ? ultra : id == "max" ? max : none;
}

// Relative speed of graphics cards in this port (RTX 4060 Ti = 100; the port is limited by the
// GPU, so this decides). Names are matched lower-case with "(tm)"/"(r)" removed, longest first,
// as whole words ("rtx 4060" does not match "rtx 4060 ti", "rx 7900 xt" not "rx 7900 xtx").
const std::vector<std::pair<const char*, int>> GPU_SPEED = {
    // NVIDIA GeForce RTX 50 / 40 / 30 / 20
    {"rtx 5090", 300}, {"rtx 5080", 230}, {"rtx 5070 ti", 205}, {"rtx 5070", 165}, {"rtx 5060 ti", 120},
    {"rtx 5060", 105}, {"rtx 5050", 80},
    {"rtx 4090", 260}, {"rtx 4080 super", 215}, {"rtx 4080", 210}, {"rtx 4070 ti super", 185}, {"rtx 4070 ti", 175},
    {"rtx 4070 super", 160}, {"rtx 4070", 140}, {"rtx 4060 ti", 100}, {"rtx 4060", 82}, {"rtx 4050", 65},
    {"rtx 3090 ti", 170}, {"rtx 3090", 155}, {"rtx 3080 ti", 152}, {"rtx 3080", 140}, {"rtx 3070 ti", 118},
    {"rtx 3070", 110}, {"rtx 3060 ti", 98}, {"rtx 3060", 75}, {"rtx 3050", 52},
    {"rtx 2080 ti", 115}, {"rtx 2080 super", 100}, {"rtx 2080", 95}, {"rtx 2070 super", 90}, {"rtx 2070", 80},
    {"rtx 2060 super", 75}, {"rtx 2060", 65},
    {"rtx a6000", 155}, {"rtx a5000", 130}, {"rtx a4000", 100}, {"rtx a2000", 60},
    // NVIDIA GeForce GTX 16 / 10
    {"gtx 1660 ti", 55}, {"gtx 1660 super", 55}, {"gtx 1660", 50}, {"gtx 1650 super", 42}, {"gtx 1650", 33},
    {"gtx 1080 ti", 85}, {"gtx 1080", 68}, {"gtx 1070 ti", 62}, {"gtx 1070", 55}, {"gtx 1060", 40},
    {"gtx 1050 ti", 25}, {"gtx 1050", 20}, {"gtx titan x", 50}, {"gtx 980 ti", 48}, {"gtx 980", 38}, {"gtx 970", 32},
    // AMD Radeon RX 9000 / 7000 / 6000 / 5000 / older
    {"rx 9070 xt", 190}, {"rx 9070 gre", 150}, {"rx 9070", 165}, {"rx 9060 xt", 110}, {"rx 9060", 95},
    {"rx 7900 xtx", 210}, {"rx 7900 xt", 185}, {"rx 7900 gre", 160}, {"rx 7800 xt", 150}, {"rx 7700 xt", 125},
    {"rx 7700", 115}, {"rx 7650 gre", 100}, {"rx 7600 xt", 95}, {"rx 7600", 90},
    {"rx 6950 xt", 165}, {"rx 6900 xt", 155}, {"rx 6800 xt", 145}, {"rx 6800", 125}, {"rx 6750 xt", 110},
    {"rx 6750 gre", 105}, {"rx 6700 xt", 105}, {"rx 6700", 95}, {"rx 6650 xt", 90}, {"rx 6600 xt", 85},
    {"rx 6600", 75}, {"rx 6500 xt", 38}, {"rx 6400", 28},
    {"rx 5700 xt", 80}, {"rx 5700", 72}, {"rx 5600 xt", 65}, {"rx 5600", 58}, {"rx 5500 xt", 45},
    {"rx vega 64", 60}, {"rx vega 56", 52}, {"radeon vii", 80}, {"rx 590", 42}, {"rx 580", 38}, {"rx 570", 32},
    {"rx 480", 36}, {"rx 470", 30},
    // Intel Arc
    {"arc b580", 95}, {"arc b570", 82}, {"arc a770", 85}, {"arc a750", 78}, {"arc a580", 65}, {"arc a380", 30},
    {"arc a310", 20},
    // integrated: Radeon 890M/880M/780M/760M/680M/660M, Intel Arc (Core Ultra), Iris Xe
    {"890m", 32}, {"880m", 28}, {"8060s", 75}, {"8050s", 60}, {"780m", 25}, {"760m", 20}, {"740m", 13},
    {"680m", 20}, {"660m", 14}, {"arc 140v", 25}, {"arc 130v", 20}, {"arc graphics", 18}, {"iris xe", 10}, {"radeon graphics", 9},
    {"uhd graphics", 5}};

bool WordChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0; }

// Speed of the detected card on the GPU_SPEED scale, and whether it was found in the table.
std::pair<int, bool> GpuSpeed(const actions::GpuInfo& gpu) {
    std::string n = Lower(gpu.name);
    for (const char* noise : {"(tm)", "(r)", "nvidia ", "geforce ", "amd ", "intel "}) n = Replace(n, noise, " ");
    std::string clean;  // single spaces
    for (char c : n)
        if (c != ' ' || (!clean.empty() && clean.back() != ' ')) clean += c;
    const char* best = nullptr;
    int speed = 0;
    for (const auto& [key, value] : GPU_SPEED) {
        const size_t len = std::strlen(key);
        if (best && len <= std::strlen(best)) continue;
        for (size_t at = clean.find(key); at != std::string::npos; at = clean.find(key, at + 1)) {
            const bool left = at == 0 || !WordChar(clean[at - 1]), right = at + len >= clean.size() || !WordChar(clean[at + len]);
            if (left && right) {
                best = key, speed = value;
                break;
            }
        }
    }
    // Laptop chips with desktop names run at lower clocks and power.
    const bool laptop = clean.find("laptop") != std::string::npos || clean.find("mobile") != std::string::npos ||
                        clean.find("max-q") != std::string::npos;
    if (best) return {laptop && !gpu.integrated ? speed * 3 / 4 : speed, true};
    if (gpu.integrated) return {10, false};
    const int vram = gpu.vram_mib;
    return {vram >= 16000 ? 150 : vram >= 12000 ? 110 : vram >= 8000 ? 80 : vram >= 6000 ? 55 : 35, false};
}

// What "Pick for my graphics card" chose: a preset, the upscaler and the frame rate it expects.
struct Recommendation {
    std::string preset, upscaler = "auto";
    int fps = 0, target = 60, speed = 0;
    bool known = false;
};

// Frame time model from the RTX 4060 Ti at 1080p: a fixed part (~140 small passes, mostly
// independent of the resolution) plus a part per rendered megapixel; object motion vectors and
// the highest LOD add a share. The first preset that reaches the target (with 5% to spare) wins.
Recommendation Recommend(const actions::GpuInfo& gpu, const std::string& output_res, const std::string& fps_mode) {
    Recommendation r;
    std::tie(r.speed, r.known) = GpuSpeed(gpu);
    r.target = fps_mode == "30" ? 30 : fps_mode == "90" ? 90 : 60;
    int ow = 1920, oh = 1080;
    if (std::sscanf(output_res.c_str(), "%dx%d", &ow, &oh) != 2 || ow <= 0 || oh <= 0) ow = 1920, oh = 1080;
    const double out_mp = ow * double(oh) / 1e6;
    struct Step {
        const char* id;
        double scale, fixed, cost;  // render scale per axis, share of the fixed part, overall factor
    };
    static const Step steps[] = {{"max", 1.0, 1.0, 1.15}, {"ultra", 1.0, 1.0, 1.10}, {"high", 1.5, 1.0, 1.10},
                                 {"medium", 1.7, 1.0, 1.0}, {"low", 2.0, 0.85, 1.0}};
    constexpr double FIXED_MS = 7.9, MS_PER_MP = 1.78;
    const double budget = 1000.0 / (r.target * 1.05);
    for (const Step& s : steps) {
        const double ms = (FIXED_MS * s.fixed + MS_PER_MP * out_mp / (s.scale * s.scale)) * s.cost * 100.0 / std::max(1, r.speed);
        r.preset = s.id, r.fps = int(1000.0 / ms);
        if (ms <= budget) break;
    }
    // FSR 4 on RDNA 4 (Radeon RX 9000) when its assets are there; otherwise Auto (DLSS / FSR 3.1).
    const std::string n = Lower(gpu.name);
    if (n.find("rx 90") != std::string::npos && Fsr4Missing().empty()) r.upscaler = "fsr4";
    return r;
}
// DLSS: wbp_dlss.dll and nvngx_dlss.dll next to wbp-game.exe (bin\ in a package, out\ in a build).
bool DlssInstalled() {
    for (const wchar_t* dir : {L"bin", L"out"}) {
        std::error_code ec;
        const fs::path base = PortDir() / dir;
        if (fs::is_regular_file(base / L"wbp_dlss.dll", ec) && fs::is_regular_file(base / L"nvngx_dlss.dll", ec))
            return true;
    }
    return false;
}

// The upscaler list for this graphics card: "Auto" names what it will pick, and DLSS is only
// offered on GeForce RTX. `gpu` is the checked card ("✓ ..."), empty until the check is done.
const std::vector<Choice>& UpscalerChoices(const std::string& gpu) {
    static const std::vector<Choice> unknown = UPSCALERS;
    const auto with_auto = [](Text title, bool keep_dlss) {
        std::vector<Choice> list;
        for (const Choice& c : UPSCALERS) {
            if (std::string_view(c.value) == "dlss" && !keep_dlss) continue;
            list.push_back(std::string_view(c.value) == "auto" ? Choice{"auto", title} : c);
        }
        return list;
    };
    static const std::vector<Choice> rtx_dlss = with_auto({"Auto (DLSS)", "Авто (DLSS)"}, true);
    static const std::vector<Choice> rtx_no_dlss = with_auto(
        {"Auto (FSR 3.1; DLSS is not installed)", "Авто (FSR 3.1; DLSS не установлен)"}, true);
    static const std::vector<Choice> other = with_auto({"Auto (FSR 3.1)", "Авто (FSR 3.1)"}, false);
    if (gpu.rfind("\u2713", 0) != 0) return unknown;  // not checked yet
    if (Lower(gpu).find("rtx") == std::string::npos) return other;
    static const bool dlss = DlssInstalled();
    return dlss ? rtx_dlss : rtx_no_dlss;
}

constexpr float HEADER = 128, BAR = 92;
constexpr float MOUSE_MIN = 0.1f, MOUSE_MAX = 5.0f;
constexpr size_t MAX_LOG = 6000;
const std::vector<std::string> RESERVED = {"Insert", "F9", "F10", "Left Alt"};

TextStyle Style(float size, Color color, Align align = Align::Left, Font font = Font::Body) {
    TextStyle s;
    s.size = size, s.color = color, s.align = align, s.font = font;
    return s;
}

float Rand() {
    static std::mt19937 rng(1234);
    return std::uniform_real_distribution<float>(0, 1)(rng);
}

std::string Upper(const std::string& text) {
    std::wstring w = Wide(text);
    CharUpperBuffW(w.data(), DWORD(w.size()));
    return Utf8(w);
}

}  // namespace

Launcher::Launcher(Ui& ui) : ui_(ui) {
    app_.Load();
    ini_.Load();
    gpu_text_ = T("• Checking the graphics card…", "• Проверка видеокарты…");
    Background([this] {
        std::string text = actions::DetectGpu();
        std::lock_guard g(lock_);
        gpu_text_ = text;
    });
    if (app_.On("check_updates"))
        Background([this] {
            try {
                actions::Release r = actions::LatestRelease();
                if (actions::Newer(r.version, VERSION)) {
                    std::lock_guard g(lock_);
                    update_ = r;
                    update_state_ = Fmt(T("Version {} is available.", "Доступна версия {}."), r.version);
                }
            } catch (const std::exception&) {
            }
        });
}

Launcher::~Launcher() {
    for (auto& w : workers_)
        if (w.joinable()) w.detach();
}

void Launcher::Background(std::function<void()> work) {
    workers_.emplace_back([work = std::move(work)] {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        try {
            work();
        } catch (...) {
        }
        if (SUCCEEDED(hr)) CoUninitialize();
    });
}

void Launcher::Notify(const std::string& text, bool error) {
    if (text.empty()) return;
    std::lock_guard g(lock_);
    notices_.push_back({text, error});
}

void Launcher::Show(const std::string& page) {
    if (page_ == page) return;
    auto index = [](const std::string& id) {
        const auto& tabs = Tabs();
        for (size_t i = 0; i < tabs.size(); ++i)
            if (tabs[i].first == id) return int(i);
        return 0;
    };
    tab_dir_ = index(page) >= index(page_) ? 1 : -1;
    page_ = page;
    page_since_ = ui_.in.time;
    if (page == "mods") lists_at_ = -10;
}

Options Launcher::Opts(const std::vector<Choice>& choices) {
    Options o;
    for (const Choice& c : choices) o.push_back({c.value, c.title()});
    return o;
}

void Launcher::Changed() {
    dirty_ = true;
    dirty_since_ = ui_.in.time;
}

void Launcher::Save() {
    dirty_ = false;
    try {
        app_.Save();
        // Every known key is written, so the game and the in-game menu see the launcher's values.
        ini_.Save();
    } catch (const std::exception& error) {
        Notify(error.what(), true);
    }
}

void Launcher::Closing() {
    try {
        Save();
        SaveLists();
    } catch (...) {
    }
}

void Launcher::IniChanged(const std::string& key) {
    // Enemy control and the free camera share their buttons: one at a time.
    if (key == "cheat_enemy_control" && ini_.On(key)) ini_.values["debug_camera"] = "0";
    if (key == "debug_camera" && ini_.On(key)) ini_.values["cheat_enemy_control"] = "0";
    Changed();
}

bool Launcher::IniFlag(Form& f, const std::string& key, const std::string& label, const std::string& hint) {
    bool v = ini_.On(key);
    if (!f.CheckRow("ini." + key, label, v, hint)) return false;
    ini_.values[key] = v ? "1" : "0";
    IniChanged(key);
    return true;
}

bool Launcher::IniChoice(Form& f, const std::string& key, const std::string& label, const std::vector<Choice>& choices,
                         const std::string& hint) {
    std::string v = ini_.Get(key);
    // A value the list does not know (an older version's, a typo) is shown as the default the
    // game falls back to, not as the list's first entry (which was DLSS for the upscaler).
    const auto known = [&](const std::string& value) {
        return std::any_of(choices.begin(), choices.end(), [&](const Choice& c) { return value == c.value; });
    };
    if (!known(v) && Ini::Defaults().count(key) && known(Ini::Defaults().at(key))) v = Ini::Defaults().at(key);
    if (!f.Choice("ini." + key, label, v, Opts(choices), hint)) return false;
    ini_.values[key] = v;
    IniChanged(key);
    return true;
}

bool Launcher::IniSlider(Form& f, const std::string& key, const std::string& label, float lo, float hi, float step,
                         const std::string& hint) {
    float v = 0;
    try {
        v = std::stof(ini_.Get(key));
    } catch (...) {
        v = std::stof(Ini::Defaults().at(key));
    }
    if (!f.SliderRow("ini." + key, label, v, lo, hi, step, hint)) return false;
    char text[32];
    std::snprintf(text, sizeof(text), "%.2f", v);
    ini_.values[key] = text;
    Changed();
    return true;
}

bool Launcher::AppFlag(Form& f, const char* key, const std::string& label, const std::string& hint) {
    bool v = app_.On(key);
    if (!f.CheckRow(std::string("app.") + key, label, v, hint)) return false;
    app_.Set(key, v);
    Changed();
    return true;
}

bool Launcher::AppChoice(Form& f, const char* key, const std::string& label, const std::vector<Choice>& choices,
                         const std::string& hint) {
    std::string v = app_.Str(key);
    if (!f.Choice(std::string("app.") + key, label, v, Opts(choices), hint)) return false;
    app_.Set(key, v);
    Changed();
    return true;
}

bool Launcher::AppEntry(Form& f, const char* key, const std::string& label, const std::string& hint, bool secret) {
    std::string v = app_.Str(key);
    if (!f.EntryRow(std::string("app.") + key, label, v, hint, secret)) return false;
    app_.Set(key, v);
    Changed();
    return true;
}

void Launcher::AppFolder(Form& f, const char* key, const std::string& label, const std::string& hint, const fs::path& fallback) {
    std::string v = app_.Str(key);
    const int r = f.FolderRow(std::string("app.") + key, label, v, hint);
    if (r == 1) {
        app_.Set(key, v);
        Changed();
        if (std::string(key) == "mods_dir" || std::string(key) == "patches_dir") lists_at_ = -10;
    } else if (r == 2) {
        fs::path path = v.empty() ? fallback : PathFrom(v);
        std::error_code ec;
        if (!fs::exists(path, ec)) fs::create_directories(path, ec);
        actions::Open(path);
    }
}

// ---- state -------------------------------------------------------------------------------------

std::vector<Launcher::Check> Launcher::Checks() {
    std::vector<Check> out;
    auto info = ReadGameInfo(PathFrom(app_.Str("game_dir")));
    if (!info) out.push_back({"bad", T("No eboot.bin in the game folder (Game & effects)", "В папке игры нет eboot.bin («Игра и эффекты»)")});
    else if (info->version == PATCH_VERSION)
        out.push_back({"ok", Fmt(T("Game version {}: every patch available", "Версия игры {}: доступны все патчи"), info->version)});
    else
        out.push_back({"warn", Fmt(T("Game version {}: runs at 30 FPS without the community patches (they are for 1.09)",
                                     "Версия игры {}: 30 FPS без патчей сообщества (они для 1.09)"), info->version)});
    const fs::path user = UserDir(app_);
    bool saves = false;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(user / L"savedata", ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        if (it.depth() > 2) it.disable_recursion_pending();
        if (it.depth() == 2 && StartsWith(PathText(it->path().filename()), "SPRJ")) {
            saves = true;
            break;
        }
    }
    out.push_back(saves ? Check{"ok", Fmt(T("Saves found in {}", "Найдены сохранения в {}"), PathText(user))}
                        : Check{"info", Fmt(T("No saves yet: the game creates them in {}", "Сохранений пока нет: игра создаст их в {}"),
                                            PathText(user))});
    std::string gpu;
    {
        std::lock_guard g(lock_);
        gpu = gpu_text_;
    }
    std::string kind = StartsWith(gpu, "✓") ? "ok" : StartsWith(gpu, "✗") ? "bad" : "info";
    for (const char* mark : {"✓ ", "✗ ", "• "})
        if (StartsWith(gpu, mark)) gpu = gpu.substr(std::strlen(mark));
    out.push_back({kind, gpu});
    return out;
}

std::string Launcher::UpscalerWarning() {
    const std::string upscaler = ini_.Get("upscaler");
    if (upscaler != "fsr4" && upscaler != "fsr411" && upscaler != "dlss") return {};
    std::string gpu;
    {
        std::lock_guard g(lock_);
        gpu = gpu_text_;
    }
    if (!StartsWith(gpu, "\u2713")) return {};  // not checked yet
    const std::string lower = Lower(gpu);
    if (upscaler == "dlss") {
        if (lower.find("rtx") != std::string::npos) return {};
        return T("DLSS needs a GeForce RTX card; on this graphics card the game uses FSR 3.1 instead. Choose FSR 3.1 "
                 "or Auto.",
                 "DLSS \u0440\u0430\u0431\u043e\u0442\u0430\u0435\u0442 \u0442\u043e\u043b\u044c\u043a\u043e \u043d\u0430 \u0432\u0438\u0434\u0435\u043e\u043a\u0430\u0440\u0442\u0430\u0445 GeForce RTX; \u043d\u0430 \u044d\u0442\u043e\u0439 \u0432\u0438\u0434\u0435\u043e\u043a\u0430\u0440\u0442\u0435 \u0438\u0433\u0440\u0430 \u0432\u043a\u043b\u044e\u0447\u0438\u0442 FSR 3.1. \u0412\u044b\u0431\u0435\u0440\u0438\u0442\u0435 "
                 "FSR 3.1 \u0438\u043b\u0438 \u00ab\u0410\u0432\u0442\u043e\u00bb.");
    }
    if (lower.find("amd") != std::string::npos || lower.find("radeon") != std::string::npos) return {};
    return T("FSR 4 is built for AMD Radeon cards. On this graphics card it runs as ordinary compute shaders and "
             "costs a lot of frame time: FSR 3.1 (or DLSS on GeForce RTX) is much faster.",
             "FSR 4 сделан для видеокарт AMD Radeon. На этой видеокарте он работает обычными вычислительными "
             "шейдерами и сильно нагружает кадр: FSR 3.1 (или DLSS на GeForce RTX) намного быстрее.");
}

std::string Launcher::VersionWarning() {
    const fs::path game = PathFrom(app_.Str("game_dir"));
    auto info = ReadGameInfo(game);
    if (!info) return {};
    if (info->version == PATCH_VERSION) {
        // The files of a 1.09 game are checked once per folder (under a second).
        static std::map<fs::path, std::string> checked;
        auto it = checked.find(game);
        if (it == checked.end()) it = checked.emplace(game, prep::GameProblem(game)).first;
        if (it->second == "patched_eboot")
            return T("eboot.bin is the 1.09 executable with a 60 FPS patch baked in: the gestures menu crashes the game. "
                     "Use the clean 1.09 eboot.bin (the port has its own frame rate patches).",
                     "eboot.bin — исполняемый файл 1.09 со вшитым патчем 60 FPS: меню жестов роняет игру. Нужен чистый "
                     "eboot.bin 1.09 (у порта свои патчи частоты кадров).");
        if (it->second == "damaged_files")
            return T("The game files are damaged: the shaders in dvdroot_ps4/shader do not unpack, and the game would hang "
                     "while loading them. Extract the game and the 1.09 update again with another tool.",
                     "Файлы игры повреждены: шейдеры в dvdroot_ps4/shader не распаковываются, игра зависнет на их загрузке. "
                     "Распакуйте игру и обновление 1.09 заново другим инструментом.");
        return {};
    }
    return Fmt(T("Your game is version {}: these options are patches for 1.09 and are not applied; the game runs at 30 FPS. "
                 "Update the dump to 1.09 to use them.",
                 "Ваша игра версии {}: эти настройки — патчи для 1.09 и не применяются; игра работает в 30 FPS. "
                 "Обновите дамп до 1.09, чтобы их использовать."),
               info->version);
}

std::string Launcher::Summary() {
    auto info = ReadGameInfo(PathFrom(app_.Str("game_dir")));
    if (!info) return T("Choose the game folder first.", "Сначала выберите папку игры.");
    std::string fps = "?";
    for (const Choice& c : FPS_MODES)
        if (app_.Str("fps_mode") == c.value) fps = c.title();
    if (info->version != PATCH_VERSION) fps = "30 FPS";
    std::string upscaler = "?";
    for (const Choice& c : UpscalerChoices(GpuText()))
        if (ini_.Get("upscaler") == c.value) upscaler = Split(c.title(), '(')[0];
    upscaler = Trim(upscaler);
    const std::string output = Replace(ini_.Get("output_res"), "x", " × ");
    const std::string online = app_.On("online") ? T("Online") : T("Offline");
    return fps + "   ·   " + upscaler + "   ·   " + output + "   ·   " + online;
}

void Launcher::RefreshLists() {
    const fs::path root = ModsDir(app_);
    nlohmann::json profile = nlohmann::json::object();
    if (auto text = ReadText(DataDir() / L"mods.json")) try {
            profile = nlohmann::json::parse(*text);
        } catch (...) {
        }
    std::vector<std::string> available = prep::DiscoverMods(root);
    std::vector<std::string> known, saved_order, disabled;
    if (profile.is_object()) {
        for (auto& v : profile.value("order", nlohmann::json::array())) if (v.is_string()) saved_order.push_back(v.get<std::string>());
        for (auto& v : profile.value("disabled", nlohmann::json::array())) if (v.is_string()) disabled.push_back(v.get<std::string>());
    }
    std::vector<std::string> sorted_current = mod_order_, sorted_available = available;
    std::sort(sorted_current.begin(), sorted_current.end());
    std::sort(sorted_available.begin(), sorted_available.end());
    if (sorted_current != sorted_available) {
        mod_order_.clear();
        for (auto& n : saved_order)
            if (std::find(available.begin(), available.end(), n) != available.end()) mod_order_.push_back(n);
        for (auto& n : available)
            if (std::find(mod_order_.begin(), mod_order_.end(), n) == mod_order_.end()) mod_order_.push_back(n);
    }
    for (auto& n : mod_order_)
        mod_enabled_.try_emplace(n, std::find(disabled.begin(), disabled.end(), n) == disabled.end());
    nlohmann::json chosen = nlohmann::json::object();
    if (auto text = ReadText(DataDir() / L"patches.json")) try {
            chosen = nlohmann::json::parse(*text);
        } catch (...) {
        }
    auto listed = [&](const char* name, const std::string& key) {
        if (!chosen.is_object()) return false;
        for (auto& v : chosen.value(name, nlohmann::json::array()))
            if (v.is_string() && v.get<std::string>() == key) return true;
        return false;
    };
    patches_.clear();
    for (auto& p : prep::ListExternalPatches(PatchesDir(app_), PortDir() / L"patches" / L"Bloodborne.xml")) {
        const bool on = listed("enabled", p.key) || (!listed("disabled", p.key) && p.enabled_by_file);
        patch_enabled_.try_emplace(p.key, on);
        patches_.push_back({p.key, p.author});
    }
}

void Launcher::SaveLists() {
    if (!mod_order_.empty()) {
        nlohmann::json disabled = nlohmann::json::array();
        for (auto& n : mod_order_)
            if (!mod_enabled_[n]) disabled.push_back(n);
        WriteText(DataDir() / L"mods.json", nlohmann::json{{"order", mod_order_}, {"disabled", disabled}}.dump(2));
    }
    if (!patch_enabled_.empty()) {
        nlohmann::json chosen = nlohmann::json::object();
        if (auto text = ReadText(DataDir() / L"patches.json")) try {
                chosen = nlohmann::json::parse(*text);
            } catch (...) {
            }
        std::set<std::string> enabled, disabled;
        if (chosen.is_object()) {
            for (auto& v : chosen.value("enabled", nlohmann::json::array()))
                if (v.is_string() && !patch_enabled_.count(v.get<std::string>())) enabled.insert(v.get<std::string>());
            for (auto& v : chosen.value("disabled", nlohmann::json::array()))
                if (v.is_string() && !patch_enabled_.count(v.get<std::string>())) disabled.insert(v.get<std::string>());
        }
        for (auto& [k, on] : patch_enabled_) (on ? enabled : disabled).insert(k);
        WriteText(DataDir() / L"patches.json", nlohmann::json{{"enabled", enabled}, {"disabled", disabled}}.dump(2));
    }
}

void Launcher::Play() {
    if (game_.Running()) return;
    const std::string npid = Trim(app_.Str("online_npid"));
    if (!app_.On("online") && !npid.empty() &&
        actions::Confirm(ui_.window, T("An online account is filled in, but \"Play online\" is off.\n\nPlay online?",
                                       "Онлайн-аккаунт заполнен, но «Играть онлайн» выключено.\n\nИграть онлайн?")))
        app_.Set("online", true);
    if (app_.On("online") && npid.find('@') != std::string::npos) {
        Notify(T("The Online ID is the account name (NPID) you registered on the server, not your email address."), true);
        Show("online");
        return;
    }
    Save();
    SaveLists();
    if (!ReadGameInfo(PathFrom(app_.Str("game_dir")))) {
        Notify(T("Choose the game folder with eboot.bin (CUSA03173).", "Выберите папку игры с eboot.bin (CUSA03173)."), true);
        Show("game");
        return;
    }
    try {
        game_.Start(app_);
        log_.clear();
        log_partial_.clear();
        log_follow_ = true;
        if (app_.On("close_on_play")) close_at_ = ui_.in.time + 5;
    } catch (const std::exception& error) {
        Notify(error.what(), true);
    }
}

// ---- frame --------------------------------------------------------------------------------------

std::function<void()> Launcher::ArtLayer() {
    // The cover art is not used as a background any more: its big title fought with the rows.
    // Only the Blood Moon shader is drawn behind the window.
    if (!art_background_) return {};
    static double checked = -10;
    if (ui_.in.time - checked > 2) {
        checked = ui_.in.time;
        const fs::path art = PathFrom(app_.Str("game_dir")) / L"sce_sys" / L"pic1.png";
        std::error_code ec;
        art_ = fs::is_regular_file(art, ec) ? art : fs::path();
    }
    if (art_.empty()) return {};
    ID2D1Bitmap* bitmap = ui_.g.Image(art_);
    if (!bitmap) return {};
    return [this, bitmap] {
        Gfx& g = ui_.g;
        const float W = g.Width(), H = g.Height();
        const D2D1_SIZE_F size = bitmap->GetSize();
        // The cover fills the window behind the smoke, slowly drifting (a living painting).
        const float scale = std::max(W / size.width, H / size.height) * 1.08f;
        const float dw = size.width * scale, dh = size.height * scale;
        const float t = float(ui_.in.time);
        const float dx = (W - dw) / 2 + std::sin(t * 0.05f) * W * 0.015f + (mouse_sx_ - 0.5f) * -18;
        const float dy = (H - dh) / 2 - H * 0.04f + (mouse_sy_ - 0.5f) * 10;
        const float fade = ui_.Anim(Ui::Id("art.fade"), 1.f, 1.2f);
        g.Dc()->DrawBitmap(bitmap, D2D1::RectF(dx, dy, dx + dw, dy + dh), 0.55f * fade, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        // blood wash: deep red multiplied over the painting
        g.Fill(Rect{0, 0, W, H}, Rgb(0x2a0306, 0.32f));
    };
}

bool Launcher::Animating() const { return true; }  // the smoke moves; main.cpp lowers the rate when hidden

void Launcher::Frame() {
    // what background work and the game handed over
    {
        std::lock_guard g(lock_);
        for (auto& [text, error] : notices_) toasts_.push_back({text, error, ui_.in.time});
        notices_.clear();
    }
    std::string fresh = game_.TakeLog();
    if (!fresh.empty()) {
        log_partial_ += fresh;
        size_t start = 0;
        for (size_t nl; (nl = log_partial_.find('\n', start)) != std::string::npos; start = nl + 1)
            log_.push_back(log_partial_.substr(start, nl - start));
        log_partial_.erase(0, start);
        while (log_.size() > MAX_LOG) log_.pop_front();
    }
    if (dirty_ && !ui_.in.down[0] && ui_.in.time - dirty_since_ > 0.25) Save();
    if (ui_.in.x >= 0) {  // outside the window the art keeps its place
        mouse_sx_ += (ui_.in.x / std::max(1.f, ui_.g.Width()) - mouse_sx_) * 0.03f;
        mouse_sy_ += (1 - ui_.in.y / std::max(1.f, ui_.g.Height()) - mouse_sy_) * 0.03f;
    }
    // Q and E switch the tabs, as L1 and R1 do in the game's menu.
    if (!ui_.Busy())
        for (const KeyEvent& k : ui_.in.keys)
            if (k.key == GLFW_KEY_Q || k.key == GLFW_KEY_E) {
                const auto& tabs = Tabs();
                size_t i = 0;
                for (; i < tabs.size(); ++i)
                    if (page_ == tabs[i].first) break;
                i = (i + (k.key == GLFW_KEY_E ? 1 : tabs.size() - 1)) % tabs.size();
                Show(tabs[i].first);
            }

    const float W = ui_.g.Width(), H = ui_.g.Height();
    DrawAsh();
    ui_.Describe({}, {});
    const Rect body{0, HEADER, W, H - HEADER - BAR};
    if (page_ == "log") {
        LogPage(body);
    } else {
        const float pane_w = std::clamp(W * 0.33f, 320.f, 470.f);
        const Rect column{56, body.y, W - pane_w - 56 - 70, body.h};
        const Rect pane{W - pane_w - 40, body.y + 18, pane_w, body.h - 36};
        Page(column);
        Description(pane);
    }
    Header(Rect{0, 0, W, HEADER});
    BottomBar(Rect{0, H - BAR, W, BAR});
    Toasts();
    FpsCounter();
}

void Launcher::FpsCounter() {
    const double now = ui_.in.time;
    ++fps_frames_;
    if (now - fps_since_ >= 0.25) {  // four updates a second
        const double span = now - fps_since_;
        if (fps_since_ > 0 && fps_frames_ > 0) {
            char text[160];
            std::snprintf(text, sizeof text, "%.0f FPS", fps_frames_ / span);
            fps_text_ = text;
            std::snprintf(text, sizeof text, "%.1f ms   UI %.2f   draw %.2f", span * 1000 / fps_frames_,
                          build_ms_ / fps_frames_, draw_ms_ / fps_frames_);
            fps_detail_ = text;
        }
        fps_since_ = now, fps_frames_ = 0, build_ms_ = draw_ms_ = 0;
    }
    if (!(app_.On("launcher_fps") || HasEnv("BB_LAUNCHER_FPS")) || fps_text_.empty()) return;
    // An overlay in the top left corner, as in a game: the frame rate large, the times below.
    TextStyle big = Style(22, Rgb(0x9cff9c));
    big.bold = true, big.wrap = false;
    TextStyle small = Style(11, Rgb(0xc8e6c8));
    small.wrap = false;
    const float w = std::max(ui_.g.Measure(fps_text_, big).width, ui_.g.Measure(fps_detail_, small).width) + 20;
    ui_.g.FillRound(Rect{6, 6, w, 48}, 4, Rgb(0x000000, 0.72f));
    ui_.g.Text(fps_text_, Rect{16, 8, w, 28}, big, true);
    ui_.g.Text(fps_detail_, Rect{16, 35, w, 16}, small, true);
}

// The game's quick save key copies <user>/savedata to savedata_quick (runtime_savedata.c).
void Launcher::RestoreQuickSave() {
    if (GameRunning()) {
        Notify(T("Close the game first: it keeps its progress in memory and would save over the restored files.",
                 "Сначала закройте игру: она держит прогресс в памяти и сохранит поверх восстановленного."),
               true);
        return;
    }
    const fs::path user = UserDir(app_), saves = user / L"savedata", quick = user / L"savedata_quick",
                   before = user / L"savedata_before_quick_load";
    std::error_code ec;
    if (!fs::is_directory(quick, ec)) {
        Notify(T("No quick save yet: press F6 in the game.", "Быстрого сохранения ещё нет: нажмите F6 в игре."), true);
        return;
    }
    fs::remove_all(before, ec);
    if (fs::exists(saves, ec)) fs::rename(saves, before, ec);
    if (ec) {
        Notify(Fmt(T("Could not move the saves aside: {}", "Не удалось отложить сейвы: {}"), ec.message()), true);
        return;
    }
    fs::copy(quick, saves, fs::copy_options::recursive, ec);
    if (ec) {
        fs::remove_all(saves, ec);
        fs::rename(before, saves, ec);
        Notify(T("Could not restore the quick save; the saves are unchanged.",
                 "Не удалось восстановить быстрое сохранение; сейвы не изменены."),
               true);
        return;
    }
    const auto when = fs::last_write_time(quick, ec);
    std::string stamp;
    if (!ec) {
        // (libc++ has no clock_cast: the file time relative to now)
        const auto sys = std::chrono::system_clock::now() +
                         std::chrono::duration_cast<std::chrono::system_clock::duration>(
                             when - fs::file_time_type::clock::now());
        const std::time_t t = std::chrono::system_clock::to_time_t(sys);
        char text[32];
        std::strftime(text, sizeof text, "%d.%m %H:%M", std::localtime(&t));
        stamp = text;
    }
    Notify(Fmt(T("Quick save from {} restored.", "Быстрое сохранение от {} восстановлено."), stamp));
}

const std::vector<std::pair<std::string, Text>>& Launcher::Tabs() {
    static const std::vector<std::pair<std::string, Text>> tabs = {
        {"play", {"Play", "Играть"}},           {"graphics", {"Graphics", "Графика"}},
        {"display", {"Display & FPS", "Экран и FPS"}}, {"controls", {"Controls", "Управление"}},
        {"game", {"Game & effects", "Игра и эффекты"}}, {"cheats", {"Cheats", "Читы"}},
        {"online", {"Online co-op"}},           {"mods", {"Mods & patches", "Моды и патчи"}},
        {"advanced", {"Advanced", "Дополнительно"}}, {"log", {"Log", "Журнал"}}};
    return tabs;
}

void Launcher::DrawAsh() {
    const float W = ui_.g.Width(), H = ui_.g.Height();
    const size_t count = size_t(W * H / 11000);
    auto spawn = [&](bool initial) {
        Ash a;
        a.ember = Rand() < 0.22f;
        a.x = Rand() * W, a.y = initial ? Rand() * H : -10;
        a.r = a.ember ? 0.7f + Rand() * 1.3f : 0.5f + Rand() * 1.5f;
        a.vx = -0.25f + Rand() * 0.3f, a.vy = 0.15f + Rand() * 0.45f;
        a.a = 0.15f + Rand() * 0.45f, a.phase = Rand() * 6.28f;
        if (a.ember && !initial) a.y = H + 5;
        return a;
    };
    while (ash_.size() < count) ash_.push_back(spawn(true));
    if (ash_.size() > count) ash_.resize(count);
    const float dt = std::min(0.05f, ui_.in.dt) / 0.0167f;
    for (Ash& f : ash_) {
        f.phase += 0.012f * dt;
        f.x += (f.vx + std::sin(f.phase) * 0.3f) * dt;
        f.y += f.vy * dt * (f.ember ? -0.7f : 1.f);
        if (f.y > H + 10 || f.y < -20 || f.x < -20 || f.x > W + 20) f = spawn(false);
        if (f.ember) {
            const float flicker = 0.6f + 0.4f * std::sin(f.phase * 9);
            ui_.g.Glow(Rect{f.x - f.r * 6, f.y - f.r * 6, f.r * 12, f.r * 12}, Color{1, 0.18f, 0.08f, 0.4f * f.a * flicker});
            ui_.g.Ellipse(f.x, f.y, f.r, f.r, Color{1, (70 + 60 * flicker) / 255.f, 40 / 255.f, f.a * flicker});
        } else {
            ui_.g.Ellipse(f.x, f.y, f.r, f.r, Color{190 / 255.f, 178 / 255.f, 166 / 255.f, f.a * 0.5f});
        }
    }
}

// The header: the title, and the tabs of the game's menu between "Q" and "E".
void Launcher::Header(const Rect& r) {
    Gfx& g = ui_.g;
    g.VGradient(Rect{r.x, r.Bottom() - 50, r.w, 50}, Rgb(0x030203, 0.0f), Rgb(0x030203, 0.55f));
    TextStyle title = Style(36, Rgb(0xf4e9d4), Align::Center, Font::Display);
    title.spacing = 14, title.wrap = false;
    const float tw = g.Measure("BLOODBORNE", title).width, cx = r.w / 2, ty = 10;
    // a dark halo so the letters stand in front of the moon
    g.Glow(Rect{cx - tw * 0.75f, ty - 20, tw * 1.5f, 90}, Rgb(0x000000, 0.7f));
    TextStyle shadow = title;
    shadow.color = Rgb(0x6a0a10, 0.9f);
    g.Text("BLOODBORNE", Rect{0, ty + 3, r.w, 48}, shadow, true);
    g.Text("BLOODBORNE", Rect{0, ty, r.w, 48}, title, true);
    // flourishes from the title to the sides: a tapered line, a diamond, three small dots
    const float ly = ty + 26, gap = tw / 2 + 26, len = std::min(260.f, cx - gap - 40);
    for (int side = -1; side <= 1; side += 2) {
        const float x0 = cx + side * gap;
        const float x1 = x0 + side * len;
        if (side < 0) g.HGradient(Rect{x1, ly, len, 1.2f}, GOLD.Alpha(0), GOLD.Alpha(0.85f));
        else g.HGradient(Rect{x0, ly, len, 1.2f}, GOLD.Alpha(0.85f), GOLD.Alpha(0));
    }
    TextStyle sub = Style(12, Rgb(0x9a8b78), Align::Center);
    sub.spacing = 3, sub.wrap = false, sub.caps = true;
    g.Text(T("Windows Bloodborne Port", "Windows Bloodborne Port") + "   \00b7   v" + VERSION + "   \00b7   giahoki",
           Rect{0, 58, r.w, 18}, sub, true);

    // the tabs
    const auto& tabs = Tabs();
    float size = 15;
    std::vector<float> widths;
    float total = 0;
    for (;; size -= 0.5f) {
        TextStyle s = Style(size, TEXT);
        s.spacing = 1.4f, s.wrap = false, s.caps = true;
        widths.clear(), total = 0;
        for (auto& [id, label] : tabs) widths.push_back(g.Measure(label(), s).width), total += widths.back();
        total += 34.f * float(tabs.size() - 1);
        if (total < r.w - 200 || size <= 12) break;
    }
    const float tab_y = 84, th = 34;
    float x = (r.w - total) / 2;
    // L1 / R1: the keys that switch tabs
    TextStyle key = Style(12, GOLD.Alpha(0.6f), Align::Center);
    key.wrap = false;
    auto keycap = [&](float cx, const char* k) {
        Rect cap{cx - 11, tab_y + th / 2 - 11, 22, 22};
        g.Stroke(cap, GOLD.Alpha(0.35f));
        g.Text(k, cap, key, true);
    };
    keycap(x - 40, "Q");
    keycap(x + total + 40, "E");
    g.HGradient(Rect{r.w * 0.08f, tab_y + th + 6, r.w * 0.42f, 1}, GOLD.Alpha(0), GOLD.Alpha(0.35f));
    g.HGradient(Rect{r.w * 0.5f, tab_y + th + 6, r.w * 0.42f, 1}, GOLD.Alpha(0.35f), GOLD.Alpha(0));
    for (size_t i = 0; i < tabs.size(); ++i) {
        const auto& [id, label] = tabs[i];
        Rect item{x - 10, tab_y, widths[i] + 20, th};
        const bool active = page_ == id;
        const bool hover = ui_.Hovered(item);
        if (hover) ui_.cursor = Cursor::Hand;
        if (hover && ui_.in.pressed[0]) Show(id);
        const float a = ui_.Anim(Ui::Id("tab.a." + id), active ? 1.f : 0.f, 10);
        const float h = ui_.Anim(Ui::Id("tab.h." + id), hover ? 1.f : 0.f);
        if (a > 0.01f) {
            g.Glow(Rect{item.x - 16, item.y - 6, item.w + 32, item.h + 26}, BLOOD_GLOW.Alpha(0.55f * a));
            const float cx = item.x + item.w / 2, half = item.w / 2 * a;
            g.HGradient(Rect{cx - half, tab_y + th + 5, half, 2}, GOLD.Alpha(0), GOLD_HI);
            g.HGradient(Rect{cx, tab_y + th + 5, half, 2}, GOLD_HI, GOLD.Alpha(0));
        }
        TextStyle s = Style(size, active ? Rgb(0xfff4e2) : hover ? TEXT : MUTED.Alpha(0.85f + 0.15f * h), Align::Center);
        s.spacing = 1.4f, s.wrap = false, s.caps = true;
        g.Text(label(), item, s, true);
        x += widths[i];
        if (i + 1 < tabs.size()) {
            x += 34;
        }
    }
}

float Launcher::TabProgress() const {
    const float t = std::clamp(float(ui_.in.time - page_since_) / 0.32f, 0.f, 1.f);
    return 1 - (1 - t) * (1 - t) * (1 - t);  // ease out
}

float Launcher::GameCard(float x, float y, float w) {
    Gfx& g = ui_.g;
    const auto info = ReadGameInfo(PathFrom(app_.Str("game_dir")));
    TextStyle title = Style(24, Rgb(0xfff0d8), Align::Left, Font::Display);
    title.spacing = 1.5f, title.caps = true, title.wrap = false, title.ellipsis = true;
    g.Text(info ? info->title : T("Game folder not set", "Папка игры не выбрана"), Rect{x, y, w, 34}, title, true);
    y += 38;
    TextStyle meta = Style(13, MUTED);
    meta.caps = true, meta.spacing = 1.2f, meta.wrap = false;
    g.Text(info ? info->id + "   \u00b7   " + Fmt(T("version {}", "версия {}"), info->version) : T("Choose it on Game & effects",
                                                                                          "Выберите её на вкладке «Игра и эффекты»"),
           Rect{x, y, w, 20}, meta, true);
    y += 30;
    g.HGradient(Rect{x, y, w, 1}, GOLD.Alpha(0.55f), GOLD.Alpha(0));
    y += 20;
    TextStyle head = Style(13, GOLD);
    head.caps = true, head.spacing = 2, head.wrap = false;
    g.Text(T("Ready check", "Проверка"), Rect{x, y, w, 20}, head, true);
    y += 30;
    for (const Check& c : Checks()) {
        Color mark = c.kind == "ok" ? OK : c.kind == "warn" ? WARN : c.kind == "bad" ? BAD : MUTED;
        TextStyle s = Style(14, c.kind == "bad" ? Rgb(0xf0b3a8) : c.kind == "info" ? MUTED : TEXT);
        const float th = g.Measure(c.text, s, w - 22).height;
        if (c.kind != "info") g.Glow(Rect{x - 6, y + 2, 16, 16}, mark.Alpha(0.5f));
        g.Dot(x + 2, y + 10, 3.2f, c.kind == "info" ? Color{0, 0, 0, 0} : mark, mark);
        g.Text(c.text, Rect{x + 18, y, w - 22, th + 2}, s);
        y += std::max(22.f, th) + 12;
    }
    y += 10;
    g.HGradient(Rect{x, y, w, 1}, GOLD.Alpha(0.3f), GOLD.Alpha(0));
    y += 16;
    y += ui_.Paragraph(T("In the game: Insert opens the port's menu\n(gamepad: press both sticks).",
                         "В игре: Insert открывает меню порта\n(геймпад: нажать оба стика)."),
                       x, y, w, Style(13, DIM));
    return y;
}

// The description panel: what the hovered setting does, like an item's description in the game.
void Launcher::Description(const Rect& r) {
    Gfx& g = ui_.g;
    const bool art = page_ == "play";
    g.VGradient(r, Rgb(0x080506, 0.62f), Rgb(0x050304, 0.80f));
    g.Panel(r, Color{0, 0, 0, 0}, GOLD.Alpha(0.25f), GOLD.Alpha(0.8f));
    float y = r.y + 22;
    const float x = r.x + 26, w = r.w - 52;
    const float t = TabProgress();
    g.PushOpacity(t);
    if (art && ui_.desc_title.empty()) {
        GameCard(x, y, w);
        g.PopOpacity();
        y = r.Bottom();
    }
    std::string title = ui_.desc_title, text = ui_.desc_text;
    if (title.empty()) title = default_title_, text = default_text_;
    if (y >= r.Bottom()) title.clear();
    TextStyle ts = Style(19, Rgb(0xfff0d8), Align::Left, Font::Display);
    ts.spacing = 1.2f, ts.caps = true;
    if (!title.empty()) {
        const float th = g.Measure(title, ts, w).height;
        g.Text(title, Rect{x, y, w, th + 2}, ts);
        y += th + 10;
        g.HGradient(Rect{x, y, w * 0.6f, 1}, GOLD.Alpha(0.7f), GOLD.Alpha(0));
        y += 16;
    }
    TextStyle body = Style(14.5f, Rgb(0xcfc2ae));
    ui_.PushClip(Rect{r.x, y, r.w, std::max(0.f, r.Bottom() - y - 10)});
    if (!title.empty()) y += ui_.Paragraph(text, x, y, w, body) + 14;
    ui_.PopClip();
    if (!(art && ui_.desc_title.empty())) g.PopOpacity();

    // the update notice at the bottom of the panel
    std::optional<actions::Release> update;
    std::string state;
    {
        std::lock_guard l(lock_);
        update = update_;
        state = update_state_;
    }
    if (update) {
        const float box_h = 96;
        Rect box{x, r.Bottom() - 18 - box_h, w, box_h};
        g.HGradient(box, BLOOD.Alpha(0.45f), BLOOD.Alpha(0.1f));
        g.Fill(Rect{box.x, box.y, 2, box.h}, GOLD);
        g.Text(state, Rect{box.x + 16, box.y + 10, box.w - 28, 30}, Style(14.5f, GOLD_HI));
        const std::string install = T("Update", "Обновить"), news = T("What's new", "Что нового");
        const float bw1 = ui_.ButtonWidth(install, true), bw2 = ui_.ButtonWidth(news, true);
        if (ui_.Button("update.install", install, Rect{box.x + 16, box.Bottom() - 44, bw1, 32}, {true, false, update_busy_})) {
            if (game_.Running()) {
                Notify(T("Close the game before updating.", "Закройте игру перед обновлением."), true);
            } else if (update->zip.empty()) {
                actions::OpenUrl(update->page);
            } else if (actions::Confirm(ui_.window, Fmt(T("Install version {} now? The launcher closes, installs it and opens again. "
                                                          "Saves and settings are kept.",
                                                          "Установить версию {} сейчас? Лаунчер закроется, установит её и "
                                                          "откроется снова. Сохранения и настройки останутся."),
                                                        update->version))) {
                update_busy_ = true;
                Save();
                actions::Release release = *update;
                Background([this, release] {
                    try {
                        actions::InstallUpdate(release, [this, release](int p) {
                            std::lock_guard l(lock_);
                            update_state_ = Fmt(T("Downloading version {}… {}%", "Загрузка версии {}… {}%"), release.version,
                                                std::to_string(p));
                        });
                        close_at_ = 0.001;  // the new version takes over
                    } catch (const std::exception& error) {
                        Notify(Fmt(T("Update failed: {}", "Не удалось обновить: {}"), std::string(error.what())), true);
                        std::lock_guard l(lock_);
                        update_state_ = Fmt(T("Version {} is available.", "Доступна версия {}."), release.version);
                    }
                    update_busy_ = false;
                });
            }
        }
        if (ui_.Button("update.news", news, Rect{box.x + 26 + bw1, box.Bottom() - 44, bw2, 32}, {true, true}))
            actions::OpenUrl(update->page);
    }
}

void Launcher::BottomBar(const Rect& r) {
    Gfx& g = ui_.g;
    g.VGradient(r, Rgb(0x030203, 0.0f), Rgb(0x030203, 0.95f));
    g.HGradient(Rect{r.w * 0.05f, r.y + 6, r.w * 0.45f, 1}, GOLD.Alpha(0), GOLD.Alpha(0.3f));
    g.HGradient(Rect{r.w * 0.5f, r.y + 6, r.w * 0.45f, 1}, GOLD.Alpha(0.3f), GOLD.Alpha(0));

    const bool running = game_.Running();
    const auto info = ReadGameInfo(PathFrom(app_.Str("game_dir")));
    const bool can_play = info && !running;
    // PLAY: engraved letters between two flourishes, a blood gem beneath; it bleeds on hover.
    const std::string play = T("PLAY", "ИГРАТЬ");
    TextStyle ps = Style(30, Rgb(0xf3e6cf), Align::Center, Font::Display);
    ps.spacing = 12, ps.wrap = false;
    const float tw = g.Measure(play, ps).width;
    const float cx = r.Right() - 70 - tw / 2 - 70, cy = r.y + r.h / 2 + 2;
    Rect hit{cx - tw / 2 - 70, r.y + 10, tw + 140, r.h - 14};
    const bool hover = can_play && ui_.Hovered(hit);
    if (hover) ui_.cursor = Cursor::Hand;
    if (hover && ui_.in.pressed[0]) Play();
    const float h = ui_.Anim(Ui::Id("play"), hover ? 1.f : 0.f, 8);
    const float beat = 0.5f + 0.5f * std::sin(float(ui_.in.time) * (hover ? 4.2f : 1.6f));
    const float dim = can_play ? 1.f : 0.4f;
    g.Glow(Rect{cx - tw / 2 - 90, cy - 50, tw + 180, 100}, BLOOD_GLOW.Alpha((0.25f + 0.15f * beat + 0.5f * h) * dim));
    // flourishes: long tapered lines with a diamond at their inner ends
    const float span = 70 + 18 * h;
    g.HGradient(Rect{cx - tw / 2 - 18 - span, cy, span, 1.5f}, GOLD.Alpha(0), GOLD.Alpha(0.9f * dim));
    g.HGradient(Rect{cx + tw / 2 + 18, cy, span, 1.5f}, GOLD.Alpha(0.9f * dim), GOLD.Alpha(0));
    ps.color = Color{0.95f + 0.05f * h, 0.90f - 0.25f * h, 0.81f - 0.35f * h, dim};
    g.Text(play, Rect{cx - tw / 2 - 60, cy - 22, tw + 120, 40}, ps, true);
    // the gem
    g.Glow(Rect{cx - 14, cy + 22 - 14, 28, 28}, Rgb(0xff2030, (0.35f + 0.4f * beat) * dim));
    g.Dot(cx, cy + 22, 3.2f, Rgb(0xc0141e, dim));

    // Stop
    const std::string stop = T("Stop", "Остановить");
    const float sw = ui_.ButtonWidth(stop, true);
    const float stop_x = cx - tw / 2 - span - 50 - sw;
    if (running && ui_.Button("stop", stop, Rect{stop_x, cy - 17, sw, 34}, {true, false, false, true})) game_.Stop();

    // status, in small capitals at the left
    std::string status;
    bool live = false;
    if (running) {
        const std::string st = game_.Status();
        status = st == "running"      ? T("The game is running.", "Игра запущена.")
                 : st == "restarting" ? T("Restarting with the new settings…", "Перезапуск с новыми настройками…")
                                      : T("Preparing the game; it opens in its own window…",
                                          "Подготовка игры; она откроется в своём окне…");
        live = true;
    } else {
        status = Summary();
    }
    TextStyle ss = Style(14, live ? GOLD_HI : MUTED);
    ss.wrap = false, ss.ellipsis = true, ss.spacing = 1, ss.caps = true;
    const float sx = 56;
    if (live) g.Dot(sx - 16, cy, 3, BLOOD_HI.Alpha(0.5f + 0.5f * beat));
    g.Text(status, Rect{sx, cy - 16, (running ? stop_x : cx - tw / 2 - span - 40) - sx - 20, 32}, ss, true);
}

float Launcher::PageHead(float, float y, float, const std::string& title, const std::string& lead) {
    default_title_ = title, default_text_ = lead;
    return y;
}

void Launcher::Page(const Rect& area) {
    if (page_ == "play") {
        PlayPage(area);
        return;
    }
    float& scroll = scroll_[page_];
    float& target = scroll_target_[page_];
    const float pad_y = 26;
    const float t = TabProgress();
    const float slide = 46 * float(tab_dir_) * (1 - t);
    // a shadow behind the column so the rows read over the smoke and the art
    ui_.g.HGradient(Rect{0, area.y, area.Right() + 60, area.h}, Rgb(0x030203, 0.6f), Rgb(0x030203, 0.15f));
    ui_.PushClip(Rect{area.x - 20, area.y, area.w + 40, area.h});
    const float x = area.x + slide, w = std::min(area.w, 900.f);
    const float top = area.y + pad_y - scroll;
    ui_.g.PushOpacity(t);
    float content = 0;
    if (page_ == "graphics") content = GraphicsPage(x, top, w);
    else if (page_ == "display") content = DisplayPage(x, top, w);
    else if (page_ == "controls") content = ControlsPage(x, top, w);
    else if (page_ == "game") content = GamePage(x, top, w);
    else if (page_ == "cheats") content = CheatsPage(x, top, w);
    else if (page_ == "online") content = OnlinePage(x, top, w);
    else if (page_ == "mods") content = ModsPage(x, top, w);
    else if (page_ == "advanced") content = AdvancedPage(x, top, w);
    ui_.g.PopOpacity();
    ui_.PopClip();
    // fade the rows out at the top and bottom edges
    ui_.g.VGradient(Rect{area.x - 20, area.y, area.w + 40, 18}, Rgb(0x030203, 0.9f), Rgb(0x030203, 0));
    ui_.g.VGradient(Rect{area.x - 20, area.Bottom() - 24, area.w + 40, 24}, Rgb(0x030203, 0), Rgb(0x030203, 0.9f));
    const float max_scroll = std::max(0.f, content - top + pad_y + 30 - area.h);
    if (area.Contains(ui_.in.x, ui_.in.y) && ui_.in.wheel != 0 && !ui_.Busy()) {
        target -= ui_.in.wheel * 90;
        ui_.in.wheel = 0;
    }
    target = std::clamp(target, 0.f, max_scroll);
    scroll += (target - scroll) * std::min(1.f, ui_.in.dt * 14);
    if (std::fabs(target - scroll) < 0.3f) scroll = target;
    if (max_scroll > 0) {
        const float track_h = area.h - 40;
        const float bar_h = std::max(40.f, track_h * area.h / (area.h + max_scroll));
        const float bar_y = area.y + 20 + (track_h - bar_h) * (scroll / max_scroll);
        ui_.g.Fill(Rect{area.x - 30, area.y + 20, 1, track_h}, GOLD.Alpha(0.12f));
        ui_.g.Fill(Rect{area.x - 31, bar_y, 3, bar_h}, GOLD.Alpha(0.35f));
    }
}

float Launcher::PlayPage(const Rect& r) {
    Gfx& g = ui_.g;
    const auto info = ReadGameInfo(PathFrom(app_.Str("game_dir")));
    default_title_ = info ? info->title : "Bloodborne";
    default_text_ = (info ? info->id + Fmt(T(" · game version {}", " · версия игры {}"), info->version)
                          : T("Game folder not set", "Папка игры не выбрана")) +
                    "\n\n" + T("In the game: Insert opens the port's menu\n(gamepad: press both sticks).",
                               "В игре: Insert открывает меню порта\n(геймпад: нажать оба стика).");
    ui_.g.HGradient(Rect{0, r.y, r.Right() + 60, r.h}, Rgb(0x030203, 0.6f), Rgb(0x030203, 0.15f));
    const float t = TabProgress();
    Form f(ui_, r.x + 46 * float(tab_dir_) * (1 - t), r.y + 26, std::min(r.w, 900.f));
    ui_.g.PushOpacity(t);
    f.Warning(VersionWarning());
    f.Warning(UpscalerWarning());
    f.Section(T("Quick settings", "Основное"));
    AppChoice(f, "fps_mode", T("Frame rate", "Частота кадров"), FPS_MODES,
              T("Community patches for game version 1.09. Unlocked makes the game use the real frame time. Other game versions "
                "always run at 30 FPS (the patches would corrupt them).",
                "Патчи сообщества для версии 1.09. «Без ограничения» — игра использует реальное время кадра. Другие версии "
                "всегда работают в 30 FPS."));
    IniChoice(f, "upscaler", T("Upscaler", "Апскейлер"), UpscalerChoices(GpuText()),
              T("Temporal upscaling with the game's own motion vectors. FSR 4 needs a GPU with INT8 dot products; otherwise the "
                "game falls back to FSR 3.1 by itself.",
                "Временной апскейлинг с векторами движения игры. FSR 4 нужен GPU с INT8; иначе игра сама переключится на "
                "FSR 3.1."));
    IniChoice(f, "preset", T("Preset", "Пресет"), PRESETS,
              T("Render scale per axis: Quality renders at 1/1.5 of the output size.",
                "Масштаб рендера по каждой оси: Quality рисует в 1/1.5 размера вывода."));
    IniChoice(f, "output_res", T("Output", "Разрешение"), OUTPUTS,
              T("What the upscaler produces; the HUD is drawn at this size too.",
                "Что выдаёт апскейлер; интерфейс рисуется в этом же размере."));
    IniSlider(f, "mouse_sensitivity", T("Mouse", "Мышь"), MOUSE_MIN, MOUSE_MAX, 0.05f,
              T("The mouse turns the camera directly, without the stick's acceleration.",
                "Мышь поворачивает камеру напрямую, без ускорения стика."));
    AppFlag(f, "fullscreen", T("Fullscreen", "Полный экран"));
    AppFlag(f, "online", T("Play online"), T("Off: the game stays offline, as before."));
    ui_.g.PopOpacity();
    return f.y;
}

float Launcher::GraphicsPage(float x, float y, float w) {
    y = PageHead(x, y, w, T("Graphics", "Графика"),
                 T("Stored in wbp.ini; the in-game menu (Insert) changes the same values.",
                   "Хранится в wbp.ini; в игре меняется через меню (Insert)."));
    Form f(ui_, x, y, w);
    f.Warning(UpscalerWarning());
    f.Section(T("Graphics preset", "Пресет графики"));
    {
        std::string current = "custom";
        for (const char* id : {"low", "medium", "high", "ultra", "max"}) {
            bool all = true;
            for (const auto& [key, value] : PresetValues(id)) all = all && ini_.Get(key) == value;
            if (all) current = id;
        }
        const auto apply = [this](const std::string& id) {
            for (const auto& [key, value] : PresetValues(id)) ini_.values[key] = value;
            Changed();
        };
        std::string chosen = current;
        if (f.Choice("ini.graphics_preset", T("Preset", "Пресет"), chosen, Opts(GRAPHICS_PRESETS),
                     T("Sets every item below at once; changing one of them afterwards makes it Custom.",
                       "Выставляет все пункты ниже сразу; если потом изменить любой из них, пресет станет «Свои».")) &&
            chosen != "custom")
            apply(chosen);
        const actions::GpuInfo gpu = actions::DetectedGpu();
        const std::string label = T("Pick for my graphics card", "Подобрать под видеокарту");
        if (ui_.Button("gfx_detect", label, Rect{x, f.y, ui_.ButtonWidth(label, true), 32}, {true, false, gpu.name.empty()})) {
            const Recommendation rec = Recommend(gpu, ini_.Get("output_res"), app_.Str("fps_mode"));
            apply(rec.preset);
            ini_.values["upscaler"] = rec.upscaler;
            std::string name = rec.preset;
            for (const Choice& c : GRAPHICS_PRESETS)
                if (rec.preset == c.value) name = Split(c.title(), ':')[0];
            Notify(Fmt(T("{}: {} — about {} FPS at {} (target {}). Frame generation is off: turn it on yourself if you "
                         "want more FPS on screen.",
                         "{}: {} — около {} FPS в {} (цель {}). Генерация кадров выключена: включите сами, если нужно "
                         "больше FPS на экране."),
                       gpu.name, name, std::to_string(rec.fps), Replace(ini_.Get("output_res"), "x", "×"),
                       std::to_string(rec.target)) +
                   (rec.known ? "" : T(" The card is not in the list: estimated from its memory.",
                                       " Карты нет в списке: оценка по объёму памяти.")));
        }
        f.y += 44;
    }
    IniFlag(f, "frame_generation", T("Frame generation (FSR 3)", "Генерация кадров (FSR 3)"),
            T("One more frame is interpolated between every two game frames: nearly twice the frame rate on "
              "screen, with about half a frame more input delay. Works with every upscaler (not with it off). Best above 50 FPS "
              "without it.",
              "Между каждыми двумя кадрами игры дорисовывается ещё один: почти вдвое больше FPS на экране, "
              "задержка управления больше примерно на полкадра. Работает с любым апскейлером (кроме выключенного). "
              "Лучше всего, если без неё больше 50 FPS."));
    IniChoice(f, "frame_generation_mode", T("Frame cap with frame generation", "Лимит FPS с генерацией"), FG_MODES,
              T("Two frames are shown per game frame, one per display refresh, so the game is capped while frame "
                "generation is on. 60 / 90 FPS: 60 (90 from 180 Hz), the steadiest. Half the refresh rate: more real frames "
                "(82 at 165 Hz, 72 at 144 Hz).",
                "На каждый кадр игры показываются два, по одному за обновление монитора, поэтому с генерацией игра "
                "ограничивается. 60 / 90 FPS: 60 (90 от 180 Гц), ровнее всего. Половина частоты монитора: больше настоящих "
                "кадров (82 при 165 Гц, 72 при 144 Гц)."));
    f.Section(T("Upscaling", "Апскейлинг"));
    IniChoice(f, "upscaler", T("Upscaler", "Апскейлер"), UpscalerChoices(GpuText()),
              T("Temporal upscaling with the game's own motion vectors. FSR 4 needs a GPU with INT8 dot products; otherwise the "
                "game falls back to FSR 3.1 by itself.",
                "Временной апскейлинг с векторами движения игры. FSR 4 нужен GPU с INT8; иначе игра сама переключится на "
                "FSR 3.1."));
    IniChoice(f, "preset", T("Quality preset", "Пресет"), PRESETS,
              T("Render scale per axis: Quality renders at 1/1.5 of the output size.",
                "Масштаб рендера по каждой оси: Quality рисует в 1/1.5 размера вывода."));
    IniChoice(f, "output_res", T("Output resolution", "Разрешение вывода"), OUTPUTS,
              T("What the upscaler produces; the HUD is drawn at this size too.",
                "Что выдаёт апскейлер; интерфейс рисуется в этом же размере."));
    IniChoice(f, "live_resolution", T("Live resolution changes", "Смена разрешения на лету"), LIVE,
              T("Off: outputs other than 1080p are set by a patch at start (fastest; changing them in the game restarts it). "
                "On: change output and preset in the game without a restart, at a cost.",
                "Выкл.: разрешения кроме 1080p задаются патчем при запуске (быстрее). Вкл.: менять в игре без перезапуска, "
                "но медленнее."));
    IniFlag(f, "sharpen", T("Sharpening (RCAS)", "Резкость (RCAS)"));
    IniSlider(f, "sharpness", T("Sharpness", "Сила резкости"), 0.f, 2.f, 0.05f);
    IniFlag(f, "object_motion", T("Object motion vectors", "Векторы движения объектов"),
            T("Less ghosting on characters, cloth and weapons; costs about 10% FPS.",
              "Меньше гостинга на персонажах и одежде; стоит около 10% FPS."));
    f.Section(T("Detail", "Детализация"));
    IniChoice(f, "model_lod", T("Model detail (LOD)", "Детализация моделей"), LODS,
              T("A game patch (game version 1.09).", "Патч игры (версия 1.09)."));
    IniChoice(f, "shadow_quality", T("Shadow resolution", "Разрешение теней"), SHADOWS,
              T("Sun shadow maps. Low draws four times fewer shadow pixels: helps weak GPUs (GTX 1650 and the like), "
                "shadow edges get softer. A game patch (1.09).",
                "Карты теней от солнца. «Низкое» рисует в 4 раза меньше пикселей теней: помогает слабым видеокартам "
                "(GTX 1650 и подобным), края теней мягче. Патч игры (1.09)."));
    IniFlag(f, "show_fps", T("Show the FPS counter", "Показывать FPS"));
    return f.y;
}

float Launcher::DisplayPage(float x, float y, float w) {
    y = PageHead(x, y, w, T("Display & FPS", "Экран и FPS"), T("Applied when the game starts.", "Применяется при запуске игры."));
    Form f(ui_, x, y, w);
    f.Warning(VersionWarning());
    f.Section(T("Frame rate", "Частота кадров"));
    AppChoice(f, "fps_mode", T("Frame rate", "Режим"), FPS_MODES,
              T("Community patches for game version 1.09. Unlocked makes the game use the real frame time. Other game versions "
                "always run at 30 FPS (the patches would corrupt them).",
                "Патчи сообщества для версии 1.09. «Без ограничения» — игра использует реальное время кадра. Другие версии "
                "всегда работают в 30 FPS."));
    AppChoice(f, "frame_cap", T("Frame cap (unlocked mode)", "Ограничение FPS (режим без ограничения)"), FRAME_CAPS,
              T("Above about 120 FPS the game's movement timing breaks (running and rolling get slower, physics and "
                "animations can glitch): higher caps are at your own risk.",
                "Выше ~120 FPS ломается тайминг движения игры (бег и перекаты замедляются, возможны сбои физики и анимаций): "
                "более высокие значения — на ваш риск."));
    AppChoice(f, "frames_ahead", T("Frames ahead of the GPU", "Кадров впереди GPU"), FRAMES_AHEAD,
              T("1 keeps frame pacing even; more can raise FPS when the graphics card is the limit.",
                "1 — ровная подача кадров; больше может поднять FPS, если упирается в видеокарту."));
    f.Section(T("Window", "Окно"));
    AppFlag(f, "fullscreen", T("Fullscreen", "Полноэкранный режим"));
    AppChoice(f, "display", T("Monitor", "Монитор"), DISPLAYS,
              T("The monitor the game window (and fullscreen) opens on; numbered as Windows lists them, the list is in "
                "the log.",
                "Монитор, на котором открывается окно игры (и полный экран); номера — в порядке Windows, список в журнале."));
    AppChoice(f, "present_mode", T("Presentation", "Режим показа кадров"), PRESENT_MODES);
    AppFlag(f, "hdr", T("Allow HDR output", "Разрешить HDR"),
            T("When HDR is on in Windows and the display supports it.", "Если HDR включён в Windows и монитор его поддерживает."));
    return f.y;
}

float Launcher::ControlsPage(float x, float y, float w) {
    Gfx& g = ui_.g;
    y = PageHead(x, y, w, T("Controls", "Управление"),
                 T("Mouse and keyboard. The in-game menu (Insert) changes the mouse speed too.",
                   "Мышь и клавиатура. Скорость мыши меняется и в меню игры (Insert)."));
    Form f(ui_, x, y, w);
    f.Section(T("Mouse", "Мышь"));
    IniSlider(f, "mouse_sensitivity", T("Mouse sensitivity", "Чувствительность мыши"), MOUSE_MIN, MOUSE_MAX, 0.05f,
              T("The mouse turns the camera directly, without the stick's acceleration.",
                "Мышь поворачивает камеру напрямую, без ускорения стика."));
    f.Section(T("Cursor and port keys", "Курсор и клавиши порта"));
    const std::vector<std::pair<std::string, std::string>> keys = {
        {T("Left Alt (hold)", "Левый Alt (удерживать)"),
         T("Shows the mouse cursor and lets it leave the game window; release to aim again.",
           "Показывает курсор мыши, им можно выйти из окна игры; отпустите — и снова управляете камерой.")},
        {"F10", T("Turns the mouse camera off and on (the cursor stays free while it is off).",
                  "Выключает и включает камеру мышью (пока выключена, курсор свободен).")},
        {"Insert", T("Opens the port's settings menu in the game (gamepad: press both sticks).",
                     "Открывает меню настроек порта в игре (геймпад: нажать оба стика).")}};
    for (auto& [key, text] : keys) {
        bool hover = false;
        Rect row = f.Row("portkey." + key, key, text, 44, &hover);
        TextStyle ds = Style(13, hover ? TEXT : MUTED, Align::Right);
        ds.wrap = false, ds.ellipsis = true;
        g.Text(text, Rect{row.Right() - f.value_w - 60, row.y, f.value_w + 60, row.h}, ds, true);
        f.y += row.h;
    }
    f.Section(T("Keyboard and mouse", "Клавиатура и мышь"));
    IniChoice(f, "input_mode", T("Keyboard input", "Ввод с клавиатуры"), INPUT_MODES,
              T("Native: keys press the game's own actions, so dodge and menu back are separate keys although the "
                "gamepad has one button (circle) for both. Emulation: every key presses its gamepad button, as before.",
                "Нативный: клавиши нажимают действия самой игры, поэтому перекат и «назад» в меню — разные клавиши, хотя "
                "у геймпада это одна кнопка (кружок). Эмуляция: каждая клавиша нажимает свою кнопку геймпада, как раньше."));
    IniFlag(f, "key_icons", T("Keyboard and mouse keys in the game's button prompts", "Клавиши клавиатуры и мыши в подсказках игры"),
            T("Instead of the gamepad buttons (cross, circle, …) the game shows your bindings. Drawn when the game starts; "
              "turn it off if you play with a gamepad.",
              "Вместо кнопок геймпада (крестик, кружок, …) игра показывает ваши клавиши. Рисуются при запуске игры; выключите, "
              "если играете на геймпаде."));
    f.Note(T("Click a box and press a key or a mouse button; hold Shift, Ctrl or Alt for a combination. Each action takes two "
             "bindings; a binding with Shift wins over the same button alone. Changes apply at once, also while the game runs.",
             "Нажмите на поле и затем клавишу или кнопку мыши; с Shift, Ctrl или Alt получится сочетание. У каждого действия "
             "две привязки; привязка с Shift важнее той же кнопки без него. Изменения применяются сразу, даже во время игры."));
    for (const KeyGroup& group : KEY_BINDINGS) {
        f.Section(group.title());
        for (const KeyAction& a : group.actions) {
            const std::string key = std::string("key_") + a.action;
            std::vector<std::string> list;
            for (auto& b : Split(ini_.Get(key), '|'))
                if (!Trim(b).empty()) list.push_back(Trim(b));
            list.resize(2);
            const bool changed_from_default = ini_.Get(key) != a.defaults;
            std::string defaults;
            for (auto& b : Split(a.defaults, '|'))
                if (!b.empty()) defaults += (defaults.empty() ? "" : ", ") + Ui::PrettyKey(b);
            const std::string hint =
                T("Click a box and press a key or a mouse button; hold Shift, Ctrl or Alt for a combination. Right click clears.",
                  "Нажмите на поле и затем клавишу или кнопку мыши; с Shift, Ctrl или Alt получится сочетание. Правый клик "
                  "очищает.") +
                "\n\n" + T("Default: ", "По умолчанию: ") + (defaults.empty() ? "—" : defaults);
            Rect row = f.Row(key, a.title() + (changed_from_default ? "  •" : ""), hint, 44, nullptr);
            const float clear_w = 26, slot_w = (f.value_w - clear_w - 12) / 2;
            const float sx = row.Right() - f.value_w;
            bool changed = false;
            for (int i = 0; i < 2; ++i) {
                std::string refused;
                if (ui_.BindSlot(key + "." + std::to_string(i), Rect{sx + i * (slot_w + 6), row.y + 6, slot_w, row.h - 12},
                                 list[i], RESERVED, &refused))
                    changed = true;
                if (!refused.empty())
                    Notify(T("Reserved by the port: ", "Эта клавиша занята портом: ") + Ui::PrettyKey(refused), true);
            }
            Rect clear{sx + 2 * (slot_w + 6), row.y + 6, clear_w, row.h - 12};
            const bool hover = ui_.Hovered(clear);
            if (hover) ui_.cursor = Cursor::Hand;
            g.Text("✕", clear, Style(12, hover ? BAD : DIM, Align::Center), true);
            if (hover && ui_.in.pressed[0]) list = {"", ""}, changed = true;
            if (changed) {
                std::string joined;
                for (auto& b : list)
                    if (!b.empty()) joined += (joined.empty() ? "" : "|") + b;
                ini_.values[key] = joined;
                Changed();
            }
            f.y += row.h;
        }
    }
    f.y += 12;
    std::vector<std::string> layouts;
    for (const KeyLayout& l : KEY_LAYOUTS) layouts.push_back(l.title());
    const int layout = f.Buttons("keys", layouts);
    if (layout >= 0 && layout < int(KEY_LAYOUTS.size())) {
        for (const KeyGroup& group : KEY_BINDINGS)
            for (const KeyAction& a : group.actions) ini_.values[std::string("key_") + a.action] = a.defaults;
        for (auto& [action, bindings] : KEY_LAYOUTS[size_t(layout)].bindings)
            ini_.values[std::string("key_") + action] = bindings;
        if (layout > 0) ini_.values["input_mode"] = "native";  // only native input splits run from dodge
        Changed();
        Notify(T("Layout set: ", "Раскладка: ") + layouts[size_t(layout)]);
    }
    f.Note(T("Left Alt, F10, Insert and F9 are taken by the port and cannot be bound. A connected gamepad keeps working next "
             "to the keyboard.",
             "Левый Alt, F10, Insert и F9 заняты портом, назначить их нельзя. Подключённый геймпад работает вместе с "
             "клавиатурой."));
    return f.y;
}

float Launcher::GamePage(float x, float y, float w) {
    y = PageHead(x, y, w, T("Game & effects", "Игра и эффекты"),
                 T("Your game dump, saves and the game patches.", "Дамп игры, сохранения и патчи игры."));
    Form f(ui_, x, y, w);
    f.Section(T("Game", "Игра"));
    AppFolder(f, "game_dir", T("Game folder", "Папка игры"),
              T("Your own dump of CUSA03173 (eboot.bin, sce_module, sce_sys, dvdroot_ps4); version 1.09 for the community "
                "patches.",
                "Ваш дамп CUSA03173 (eboot.bin, sce_module, sce_sys, dvdroot_ps4); версия 1.09 для патчей сообщества."),
              PortDir());
    AppFolder(f, "user_dir", T("Saves folder", "Папка сохранений"),
              Fmt(T("Empty: {} (shader caches are kept there too).", "Пусто: {} (там же кэш шейдеров)."),
                  PathText(DataDir() / L"user")),
              DataDir() / L"user");
    AppChoice(f, "language", T("Game language", "Язык игры"), LANGUAGES);
    AppEntry(f, "player_name", T("Player name", "Имя игрока"),
             T("Where the game shows the PSN name; empty: the default.", "Где игра показывает имя PSN; пусто — по умолчанию."));
    if (f.Buttons("quick_save", {T("Restore quick save", "Восстановить быстрое сохранение")}) == 0) RestoreQuickSave();
    f.Note(T("The quick save key (F6, Controls page) copies the saves during play. Restoring puts that copy back; the "
             "saves it replaces are kept in savedata_before_quick_load.",
             "Клавиша быстрого сохранения (F6, вкладка «Управление») копирует сейвы во время игры. Восстановление "
             "возвращает эту копию; заменённые сейвы остаются в savedata_before_quick_load."));
    f.Section(T("Effects", "Эффекты"));
    f.Warning(VersionWarning());
    for (const Flag& e : EFFECTS) IniFlag(f, e.key, e.title());
    f.Section(T("Extras", "Дополнительно"));
    for (const Flag& e : EXTRAS) IniFlag(f, e.key, e.title());
    f.Note(T("Effects and extras are game patches for version 1.09, applied at start.",
             "Эффекты и дополнения — патчи игры для версии 1.09, применяются при запуске."));
    return f.y;
}

float Launcher::CheatsPage(float x, float y, float w) {
    y = PageHead(x, y, w, T("Cheats", "Читы"),
                 T("Game patches for version 1.09, applied at start. Leave them off for a normal play-through.",
                   "Патчи игры для версии 1.09, применяются при запуске. Для обычного прохождения оставьте их выключенными."));
    Form f(ui_, x, y, w);
    f.Warning(VersionWarning());
    f.Section(T("Cheats", "Читы"));
    for (const Flag& e : CHEATS) IniFlag(f, e.key, e.title());
    f.Section(T("Gameplay tweaks", "Изменения игрового процесса"));
    for (const Flag& e : TWEAKS) IniFlag(f, e.key, e.title());
    return f.y;
}

float Launcher::OnlinePage(float x, float y, float w) {
    y = PageHead(x, y, w, T("Online co-op"),
                 T("Messages, bloodstains and ghosts come from The Hunter's Dream; bells and summons go through a shadNet server."));
    Form f(ui_, x, y, w);
    AppFlag(f, "online", T("Play online"), T("Off: the game stays offline, as before."));
    f.Section(T("Game server (messages, bloodstains, ghosts)"));
    AppEntry(f, "online_community", T("Game server"),
             T("The Hunter's Dream by default. Only the name is needed; the game picks the ports."));
    f.Section(T("Co-op server (bells and summons)"));
    AppEntry(f, "online_server", T("Server address"),
             T("srv.shadps4.net:31313 is the public shadPS4 server. For a private server, enter host:port from the person who "
               "runs it."));
    AppEntry(f, "online_webapi", T("WebAPI address"),
             T("Of the shadNet server, not the game server. Leave empty: the server address with port 31315 is used."));
    f.Section(T("Account"));
    AppEntry(f, "online_npid", T("Online ID (NPID)"),
             T("Your shadNet account name, not your email. Register one at shadnet.shadps4.net, or ask the owner of a private "
               "server."));
    AppEntry(f, "online_password", T("Password"), T("Saved on this PC with the other launcher settings."), true);
    f.Section(T("Connection"));
    AppFlag(f, "online_upnp", T("UPnP (open a port on the router automatically)"), T("Turn it off on Tailscale or another VPN."));
    const std::string label = test_busy_ ? T("Checking…") : T("Test connection");
    if (ui_.Button("online.test", label, Rect{x, f.y, ui_.ButtonWidth(label, true), 32}, {true, false, test_busy_})) {
        test_busy_ = true;
        Save();
        App snapshot = app_;
        Background([this, snapshot] {
            std::string text = actions::TestConnection(snapshot);
            std::lock_guard l(lock_);
            online_result_ = text;
            test_busy_ = false;
        });
    }
    f.y += 44;
    std::string result;
    {
        std::lock_guard l(lock_);
        result = online_result_;
    }
    if (!result.empty()) f.y += ui_.Paragraph(result, x, f.y, std::min(w, 760.f), Style(13, MUTED)) + 12;
    f.Note(T("Everyone in a session needs the same game version (1.09) and the same server. Cheats change the game for the "
             "other players too: turn them off when you play together."));
    return f.y;
}

float Launcher::ModsPage(float x, float y, float w) {
    Gfx& g = ui_.g;
    if (ui_.in.time - lists_at_ > 3) RefreshLists(), lists_at_ = ui_.in.time;
    y = PageHead(x, y, w, T("Mods & patches", "Моды и патчи"),
                 T("The game files are never changed: mods are layered over them at start.",
                   "Файлы игры не меняются: моды накладываются при запуске."));
    Form f(ui_, x, y, w);
    f.Section(T("Mods", "Моды"));
    AppFlag(f, "mods_enabled", T("Load mods", "Загружать моды"),
            T("Loose-file mods, each in its own folder (with dvdroot_ps4, or chr\\, parts\\ … directly).",
              "Моды из файлов, каждый в своей папке (с dvdroot_ps4 или chr\\, parts\\ … напрямую)."));
    AppFolder(f, "mods_dir", T("Mods folder", "Папка модов"), Fmt(T("Empty: {}", "Пусто: {}"), PathText(DataDir() / L"mods")),
              DataDir() / L"mods");
    const float list_w = std::min(w, 760.f);
    if (mod_order_.empty()) {
        f.y += ui_.Paragraph(Fmt(T("No mods in {} yet.", "В {} пока нет модов."), PathText(ModsDir(app_))), x, f.y, list_w,
                             Style(13, MUTED)) + 14;
    } else {
        f.y += ui_.Paragraph(T("Lower in the list loads later and wins conflicts.", "Ниже в списке — загружается позже и перекрывает."),
                             x, f.y, list_w, Style(13, MUTED)) + 10;
        const Options on_off = {{"0", T("Off", "Выкл.")}, {"1", T("On", "Вкл.")}};
        for (size_t i = 0; i < mod_order_.size(); ++i) {
            const std::string name = mod_order_[i];
            bool hover = false;
            Rect row = f.Row("mod." + name, std::to_string(i + 1) + ".  " + name,
                             T("Lower in the list loads later and wins conflicts.", "Ниже в списке — загружается позже и перекрывает."),
                             46, &hover);
            std::string v = mod_enabled_[name] ? "1" : "0";
            const float arrows = 76;
            if (ui_.Cycle("mod.on." + name, Rect{row.Right() - f.value_w, row.y + 4, f.value_w - arrows - 8, row.h - 8}, on_off, v,
                          hover))
                mod_enabled_[name] = v == "1", SaveLists();
            if (ui_.Button("mod.up." + name, "▲", Rect{row.Right() - arrows, row.y + 8, 34, row.h - 16}, {true, false, i == 0})) {
                std::swap(mod_order_[i], mod_order_[i - 1]);
                SaveLists();
            }
            if (ui_.Button("mod.down." + name, "▼", Rect{row.Right() - 36, row.y + 8, 34, row.h - 16},
                           {true, false, i + 1 == mod_order_.size()})) {
                std::swap(mod_order_[i], mod_order_[i + 1]);
                SaveLists();
            }
            f.y += row.h;
        }
        f.y += 8;
    }
    f.Section(T("Third-party patches", "Сторонние патчи"));
    AppFolder(f, "patches_dir", T("Patches folder", "Папка патчей"),
              Fmt(T("shadPS4/GoldHEN XML patch files for version 1.09. Empty: {}", "XML-патчи shadPS4/GoldHEN для версии 1.09. Пусто: {}"),
                  PathText(DataDir() / L"patches")),
              DataDir() / L"patches");
    if (patches_.empty()) {
        f.y += ui_.Paragraph(T("No patch files for version 1.09 yet.", "Пока нет патчей для версии 1.09."), x, f.y, list_w,
                             Style(13, MUTED)) + 14;
    } else {
        const Options on_off = {{"0", T("Off", "Выкл.")}, {"1", T("On", "Вкл.")}};
        for (auto& [key, author] : patches_) {
            bool hover = false;
            Rect row = f.Row("patch." + key, key, author.empty() ? key : key + "\n\n" + author, 46, &hover);
            std::string v = patch_enabled_[key] ? "1" : "0";
            if (ui_.Cycle("patch.on." + key, Rect{row.Right() - f.value_w, row.y + 4, f.value_w, row.h - 8}, on_off, v, hover))
                patch_enabled_[key] = v == "1", SaveLists();
            f.y += row.h;
        }
    }
    return f.y;
}

float Launcher::AdvancedPage(float x, float y, float w) {
    y = PageHead(x, y, w, T("Advanced", "Дополнительно"),
                 T("Launcher options, performance switches and diagnostics.", "Настройки лаунчера, производительность и диагностика."));
    Form f(ui_, x, y, w);
    f.Section(T("Launcher", "Лаунчер"));
    {
        std::vector<Choice> languages;
        static std::vector<std::string> names;  // keep the strings alive for Choice
        auto list = i18n::UiLanguages();
        names.clear();
        for (auto& l : list) names.push_back(l.name);
        Options opts;
        for (auto& l : list) opts.push_back({l.code, l.name});
        std::string v = app_.Str("ui_language");
        if (f.Choice("app.ui_language", T("Launcher language", "Язык лаунчера"), v, opts)) {
            app_.Set("ui_language", v);
            i18n::SetLanguage(v);
            Changed();
            if (on_language) on_language();
        }
    }
    AppFlag(f, "close_on_play", T("Close the launcher when the game starts", "Закрывать лаунчер при запуске игры"));
    AppFlag(f, "check_updates", T("Check for updates when the launcher opens", "Проверять обновления при открытии лаунчера"));
    {
        // wbp.ini moves along at once; BB_CONFIG (tests, a restart from the game) keeps its own path.
        std::string v = app_.Str("config_place");
        if (f.Choice("app.config_place", T("Settings file (wbp.ini)", "Файл настроек (wbp.ini)"), v, Opts(CONFIG_PLACES),
                     Fmt(T("AppData: {}\nPort folder: {}", "AppData: {}\nПапка порта: {}"), PathText(ConfigDir() / L"wbp.ini"),
                         PathText(DataDir() / L"wbp.ini")))) {
            App next = app_;
            next.Set("config_place", v);
            try {
                ini_.Save();
                MoveIni(app_, next);
                app_ = next;
                app_.Save();
                Notify(Fmt(T("Settings file: {}", "Файл настроек: {}"), PathText(Ini::Path())));
            } catch (const std::exception& error) {
                Notify(error.what(), true);
            }
        }
    }
    const int a = f.Buttons("adv1", {T("Desktop shortcut", "Ярлык на рабочем столе"), T("Add to Steam"),
                                     T("Port folder", "Папка порта"), "wbp.ini"});
    if (a == 0) {
        try {
            Notify(actions::DesktopShortcut());
        } catch (const std::exception& error) {
            Notify(error.what(), true);
        }
    } else if (a == 1) {
        Save();
        void* window = ui_.window;
        Background([this, window] {
            try {
                Notify(actions::AddToSteam(window));
            } catch (const std::exception& error) {
                Notify(error.what(), true);
            }
        });
    } else if (a == 2) {
        actions::Open(DataDir());
    } else if (a == 3) {
        Save();
        actions::Open(Ini::Path());
    }
    const int b = f.Buttons("adv2", {T("Check for updates", "Проверить обновления"), T("Clear shader cache", "Очистить кэш шейдеров")});
    if (b == 0) {
        Background([this] {
            try {
                actions::Release r = actions::LatestRelease();
                if (actions::Newer(r.version, VERSION)) {
                    std::lock_guard l(lock_);
                    update_ = r;
                    update_state_ = Fmt(T("Version {} is available.", "Доступна версия {}."), r.version);
                    notices_.push_back({update_state_, false});
                } else {
                    Notify(Fmt(T("You have the latest version ({}).", "У вас последняя версия ({})."), std::string(VERSION)));
                }
            } catch (const std::exception& error) {
                Notify(Fmt(T("Could not check for updates: {}", "Не удалось проверить обновления: {}"), std::string(error.what())), true);
            }
        });
    } else if (b == 1) {
        if (game_.Running()) {
            Notify(T("Close the game first.", "Сначала закройте игру."), true);
        } else {
            std::error_code ec;
            fs::remove_all(UserDir(app_) / L"cache", ec);
            Notify(T("Shader cache cleared. The next start stutters for a few minutes while it is rebuilt.",
                     "Кэш шейдеров очищен. Следующий запуск несколько минут будет подтормаживать, пока кэш собирается заново."));
        }
    }
    f.Note(T("If the game only shows a black screen, clearing the shader cache usually helps.",
             "Если игра показывает только чёрный экран, обычно помогает очистка кэша шейдеров."));
    f.Section(T("Performance", "Производительность"));
    AppChoice(f, "draw_pipe", T("Two-stage GPU pipeline", "Двухстадийный конвейер GPU"), DRAW_PIPE,
              T("20–30% faster; switch it off if the game is unstable.", "Быстрее на 20–30%; при нестабильности выключите."));
    AppChoice(f, "readbacks", T("GPU readbacks", "Чтение данных GPU"), READBACKS,
              T("How exactly data the GPU writes is copied back for the game.",
                "Насколько точно данные, записанные GPU, возвращаются игре."));
    f.Section(T("Diagnostics", "Для разработчика"));
    AppFlag(f, "frame_stats", T("Frame statistics in the log (every 5 s)", "Статистика кадров в журнале (раз в 5 с)"));
    AppFlag(f, "gpu_profile", T("GPU time per pass in the log", "Профиль GPU в журнале"));
    AppFlag(f, "launcher_fps", T("Launcher FPS counter (overlay)", "Счётчик FPS лаунчера (оверлей)"),
            T("Live in the top left corner of the launcher window. Below it: frame interval, time to build the UI and to draw it. The "
              "launcher runs at the monitor refresh rate in front, 20 FPS behind other windows and 1 while the game runs.",
              "Вживую в левом верхнем углу окна лаунчера. Под ним мелко: интервал кадра, время сборки интерфейса и отрисовки. "
              "На переднем плане лаунчер идёт с частотой монитора, за другими окнами 20 FPS, во время игры 1."));
    AppFlag(f, "vk_validation", T("Vulkan validation layers (needs the Vulkan SDK; much slower)",
                                  "Слои валидации Vulkan (нужен Vulkan SDK; сильно замедляет)"));
    AppEntry(f, "extra_env", T("Extra variables", "Доп. переменные"),
             T("NAME=value pairs separated by spaces (README lists them).", "Пары ИМЯ=значение через пробел (список в README)."));
    return f.y;
}

void Launcher::LogPage(const Rect& area) {
    Gfx& g = ui_.g;
    const float x = area.x + 56, w = area.w - 112;
    float y = area.y + 18;
    const std::string copy = T("Copy", "Копировать"), clear = T("Clear", "Очистить");
    const float cw = ui_.ButtonWidth(copy, true), clw = ui_.ButtonWidth(clear, true);
    if (ui_.Button("log.copy", copy, Rect{x + w - cw - clw - 10, y, cw, 32}, {true}) && ui_.clipboard_set) {
        std::string all;
        for (auto& l : log_) all += l + "\r\n";
        ui_.clipboard_set(all);
    }
    if (ui_.Button("log.clear", clear, Rect{x + w - clw, y, clw, 32}, {true})) log_.clear(), log_scroll_ = 0;
    TextStyle cap = Style(13, MUTED);
    cap.caps = true, cap.spacing = 1.5f, cap.wrap = false;
    g.Text(PathText(UserDir(app_) / L"last_run.log"), Rect{x, y, w - cw - clw - 30, 32}, cap, true);
    y += 44;
    Rect box{x, y, w, area.Bottom() - 26 - y};
    g.Fill(box, Rgb(0x030203, 0.86f));
    g.Stroke(box, LINE);
    TextStyle s = Style(12.5f, Rgb(0xcbbfae), Align::Left, Font::Mono);
    s.wrap = false;
    const float line_h = 18.2f;
    const float visible = (box.h - 28) / line_h;
    const float max_scroll = std::max(0.f, float(log_.size()) - visible);
    if (ui_.Hovered(box) && ui_.in.wheel != 0) {
        log_scroll_ = std::clamp(log_scroll_ - ui_.in.wheel * 3, 0.f, max_scroll);
        log_follow_ = log_scroll_ >= max_scroll - 0.5f;
        ui_.in.wheel = 0;
    }
    if (log_follow_) log_scroll_ = max_scroll;
    log_scroll_ = std::min(log_scroll_, max_scroll);
    ui_.PushClip(box.Inset(1));
    const size_t first = size_t(log_scroll_);
    for (size_t i = first; i < log_.size() && float(i - first) < visible + 1; ++i)
        g.Text(log_[i], Rect{box.x + 16, box.y + 14 + float(i - first) * line_h, 6000, line_h}, s);
    ui_.PopClip();
    if (max_scroll > 0) {
        const float track = box.h - 20, bar_h = std::max(30.f, track * visible / float(log_.size()));
        ui_.g.Fill(Rect{box.Right() - 9, box.y + 10 + (track - bar_h) * (log_scroll_ / max_scroll), 5, bar_h}, GOLD.Alpha(0.22f));
    }
}

void Launcher::Toasts() {
    Gfx& g = ui_.g;
    float y = g.Height() - BAR - 18;
    const float w = 440;
    for (auto it = toasts_.rbegin(); it != toasts_.rend(); ++it) {
        const double age = ui_.in.time - it->born, life = it->error ? 7.0 : 4.5;
        const float fade = float(std::clamp(life + 0.5 - age, 0.0, 0.5) / 0.5);
        const float rise = float(std::max(0.0, 0.3 - age) / 0.3) * 8;
        TextStyle s = Style(14, it->error ? Rgb(0xf2c1b8) : TEXT);
        const float th = g.Measure(it->text, s, w - 36).height;
        Rect box{g.Width() - 26 - w, y - th - 28 + rise, w, th + 28};
        g.Fill(Rect{box.x + 6, box.y + 10, box.w, box.h}, Color{0, 0, 0, 0.35f * fade});
        g.Fill(box, Rgb(0x0e090a, 0.96f * fade));
        g.Stroke(box, (it->error ? Rgb(0xd9614f, 0.7f) : LINE_STRONG).Alpha(fade));
        s.color = s.color.Alpha(fade);
        g.Text(it->text, Rect{box.x + 18, box.y + 14, w - 36, th + 2}, s);
        y = box.y - 10;
    }
    toasts_.erase(std::remove_if(toasts_.begin(), toasts_.end(),
                                 [&](const Toast& t) { return ui_.in.time - t.born > (t.error ? 7.6 : 5.1); }),
                  toasts_.end());
}
