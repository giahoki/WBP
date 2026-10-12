// SPDX-License-Identifier: GPL-2.0-or-later
// The launcher window's content: the side bar, the pages (Play, Graphics, Display & FPS,
// Controls, Game & effects, Cheats, Online co-op, Mods & patches, Advanced, Log), the bottom bar
// with PLAY, and notifications. All settings are saved as they change.
#pragma once
#include <atomic>
#include <deque>
#include <functional>
#include <optional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../actions.h"
#include "../runner.h"
#include "../settings.h"
#include "ui.h"

class Launcher {
public:
    explicit Launcher(Ui& ui);
    ~Launcher();
    void Frame();             // one frame of the whole window
    // Time the last frame took to build (Frame and the UI) and to draw and present: the
    // launcher FPS counter (Advanced > Diagnostics).
    void FrameTimes(double build_ms, double draw_ms) { build_ms_ += build_ms, draw_ms_ += draw_ms; }
    bool WantsClose() const { return close_at_ > 0 && ui_.in.time >= close_at_; }
    bool GameRunning() const { return game_.Running(); }
    bool Animating() const;   // something moves: keep drawing at full rate
    void Closing();           // saves the settings
    void Open(const std::string& page) { Show(page); }
    // The dump's cover art (sce_sys/pic1.png) under the sky, or an empty function without one.
    std::function<void()> ArtLayer();
    std::function<void()> on_language;  // the window title and fonts follow the language

private:
    struct Toast {
        std::string text;
        bool error;
        double born;
    };
    struct Ash {
        float x, y, r, vx, vy, a, phase;
        bool ember;
    };

    // pages
    void Header(const Rect& r);
    void Description(const Rect& r);
    // The play page's panel: the game, the ready check (when no setting is hovered).
    float GameCard(float x, float y, float w);
    // Tab transitions: progress 0..1 and the side the new page comes from.
    float TabProgress() const;
    int tab_dir_ = 1;
    static const std::vector<std::pair<std::string, Text>>& Tabs();
    void BottomBar(const Rect& r);
    void Page(const Rect& r);
    float PageHead(float x, float y, float w, const std::string& title, const std::string& lead);
    float PlayPage(const Rect& r);
    float GraphicsPage(float x, float y, float w);
    float DisplayPage(float x, float y, float w);
    float ControlsPage(float x, float y, float w);
    float GamePage(float x, float y, float w);
    float CheatsPage(float x, float y, float w);
    float OnlinePage(float x, float y, float w);
    float ModsPage(float x, float y, float w);
    float AdvancedPage(float x, float y, float w);
    void LogPage(const Rect& r);
    void Toasts();
    void DrawAsh();

    // values
    bool IniFlag(Form& f, const std::string& key, const std::string& label, const std::string& hint = {});
    bool IniChoice(Form& f, const std::string& key, const std::string& label, const std::vector<Choice>& choices,
                   const std::string& hint = {});
    bool AppFlag(Form& f, const char* key, const std::string& label, const std::string& hint = {});
    bool AppChoice(Form& f, const char* key, const std::string& label, const std::vector<Choice>& choices,
                   const std::string& hint = {});
    bool AppEntry(Form& f, const char* key, const std::string& label, const std::string& hint = {}, bool secret = false);
    void AppFolder(Form& f, const char* key, const std::string& label, const std::string& hint, const fs::path& fallback);
    bool IniSlider(Form& f, const std::string& key, const std::string& label, float lo, float hi, float step,
                   const std::string& hint = {});
    void Changed();  // saves soon (debounced while a slider is dragged)
    void Save();
    void IniChanged(const std::string& key);
    static Options Opts(const std::vector<Choice>& choices);
    void Notify(const std::string& text, bool error = false);
    void Show(const std::string& page);

    // state and actions
    struct Check {
        std::string kind, text;  // ok, warn, bad, info
    };
    std::vector<Check> Checks();
    std::string Summary();
    std::string VersionWarning();
    std::string UpscalerWarning();  // FSR 4 on a graphics card that is not AMD
    void Play();
    void RefreshLists();
    void SaveLists();
    void Background(std::function<void()> work);

    Ui& ui_;
    App app_;
    Ini ini_;
    std::string page_ = "play";
    std::map<std::string, float> scroll_, scroll_target_, page_height_;
    double page_since_ = 0;
    bool dirty_ = false;
    double dirty_since_ = 0;
    double close_at_ = 0;
    std::vector<Toast> toasts_;
    std::vector<Ash> ash_;
    float mouse_sx_ = 0.5f, mouse_sy_ = 0.5f;
    // launcher FPS counter: sums over the current half second, and what it shows
    double fps_since_ = 0, build_ms_ = 0, draw_ms_ = 0;
    int fps_frames_ = 0;
    std::string fps_text_, fps_detail_;
    void FpsCounter();

    GameProcess game_;
    std::deque<std::string> log_;
    std::string log_partial_;
    float log_scroll_ = 0;
    bool log_follow_ = true;

    std::mutex lock_;  // what background work hands back
    std::string gpu_text_;
    std::string GpuText() {
        std::lock_guard g(lock_);
        return gpu_text_;
    }
    std::vector<std::pair<std::string, bool>> notices_;
    std::optional<actions::Release> update_;
    std::string update_state_;
    std::atomic<bool> fsr4_busy_{false}, test_busy_{false}, update_busy_{false};
    std::atomic<size_t> fsr4_done_{0}, fsr4_total_{0};
    std::string online_result_;
    std::vector<std::thread> workers_;

    std::vector<std::string> mod_order_;
    std::map<std::string, bool> mod_enabled_;
    std::vector<std::pair<std::string, std::string>> patches_;  // key, author
    std::map<std::string, bool> patch_enabled_;
    double lists_at_ = -10;
    fs::path art_;
    bool art_background_ = false;
    std::string default_title_, default_text_;
};
