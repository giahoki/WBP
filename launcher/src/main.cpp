// SPDX-License-Identifier: GPL-2.0-or-later
// Bloodborne.exe: the launcher window, and the roles without a window:
//   --run [--after PID] [args]   prepare the game and run wbp-game.exe (the in-game restart too)
//   --play                       the game with the launcher's saved settings (shortcuts, Steam)
//   --install-update TARGET PID  run by a downloaded update from its temporary folder
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dwmapi.h>
#include <objbase.h>
#include <shellapi.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <thread>

#include "actions.h"
#include "i18n.h"
#include "runner.h"
#include "settings.h"
#include "ui/gfx.h"
#include "ui/launcher.h"
#include "ui/ui.h"

namespace {

// Console output for the roles when started from a terminal (a GUI program gets none).
void AttachOutput() {
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out && out != INVALID_HANDLE_VALUE) return;
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) return;
    HANDLE console = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                 OPEN_EXISTING, 0, nullptr);
    if (console == INVALID_HANDLE_VALUE) return;
    SetStdHandle(STD_OUTPUT_HANDLE, console);
    SetStdHandle(STD_ERROR_HANDLE, console);
}

struct Window {
    GLFWwindow* glfw = nullptr;
    Gfx gfx;
    Ui ui{gfx};
    Input pending;  // gathered by the callbacks between frames
    float scale = 1;
    bool close_requested = false;  // the close button: the window closes after the closing animation
};

Window* Of(GLFWwindow* w) { return static_cast<Window*>(glfwGetWindowUserPointer(w)); }

// A critically damped spring towards `target` (no overshoot, eases in and out), stable at any dt.
void Damp(float& x, float& velocity, float target, float smooth_time, float dt) {
    const float omega = 2 / smooth_time, k = omega * dt;
    const float decay = 1 / (1 + k + 0.48f * k * k + 0.235f * k * k * k);
    const float change = x - target, temp = (velocity + omega * change) * dt;
    velocity = (velocity - omega * temp) * decay;
    x = target + (change + temp) * decay;
}

float EaseOut(float t) {
    t = std::clamp(t, 0.f, 1.f);
    return 1 - (1 - t) * (1 - t) * (1 - t);
}

// The sky follows the cursor with weight instead of sticking to it: the parallax lags behind on
// a spring, and when the cursor leaves the window it stays where it left, then drifts back to
// the centre. The lantern follows closely and fades out outside. Also the opening and closing.
struct Motion {
    float x = 0.5f, y = 0.5f, vx = 0, vy = 0;
    float lx = 0.5f, ly = 0.5f, lvx = 0, lvy = 0, lamp = 0;
    float last_x = 0.5f, last_y = 0.5f;
    double left_at = -100;

    Gfx::Sky Step(const Input& in, float width, float height, double now, float dt, double closing_at) {
        const bool inside = in.x >= 0 && in.y >= 0;
        if (inside) {
            last_x = std::clamp(in.x / std::max(1.f, width), 0.f, 1.f);
            last_y = std::clamp(1 - in.y / std::max(1.f, height), 0.f, 1.f);
            left_at = -100;
        } else if (left_at < 0) {
            left_at = now;
        }
        const bool home = !inside && now - left_at > 0.8;
        const float tx = home ? 0.5f : last_x, ty = home ? 0.5f : last_y;
        Damp(x, vx, tx, home ? 1.4f : 0.45f, dt);
        Damp(y, vy, ty, home ? 1.4f : 0.45f, dt);
        Damp(lx, lvx, last_x, 0.07f, dt);
        Damp(ly, lvy, last_y, 0.07f, dt);
        lamp += ((inside ? 1.f : 0.f) - lamp) * (1 - std::exp(-dt * (inside ? 5.f : 3.f)));

        Gfx::Sky sky{x, y, lx, ly, lamp};
        // Opening: the sky comes out of the dark with the moon settling from above, then the
        // interface fades in and grows to its size. Closing: the same backwards, faster.
        const float open_sky = std::clamp(float(now) / 1.4f, 0.f, 1.f);
        const float open_ui = EaseOut((float(now) - 0.55f) / 0.75f);
        const float out = closing_at < 0 ? 0 : std::clamp(float(now - closing_at) / CLOSE_TIME, 0.f, 1.f);
        sky.reveal = open_sky * (1 - out);
        sky.ui_opacity = open_ui * (1 - EaseOut(out * 1.6f));
        sky.ui_scale = (0.96f + 0.04f * open_ui) * (1 - 0.03f * EaseOut(out));
        sky.ui_shift = (1 - open_ui) * 14;
        return sky;
    }
    static constexpr float CLOSE_TIME = 0.45f;
};

// The window itself opens and closes: it fades in while rising into place and fades out while
// sinking a little. Windows' own open and close transitions are turned off so they do not mix.
struct WindowFx {
    static constexpr float OPEN_TIME = 0.45f, RISE = 22, SINK = 12;  // seconds, DIPs
    HWND hwnd = nullptr;
    float scale = 1;
    int x = 0, y = 0;  // where the window belongs
    bool opening = true, moves = true;

    void Prepare(HWND window, float dpi_scale) {
        hwnd = window, scale = dpi_scale;
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE, GetWindowLongPtrW(hwnd, GWL_EXSTYLE) | WS_EX_LAYERED);
        SetLayeredWindowAttributes(hwnd, 0, 0, LWA_ALPHA);
        BOOL off = TRUE;
        DwmSetWindowAttribute(hwnd, 3 /* DWMWA_TRANSITIONS_FORCEDISABLED */, &off, sizeof(off));
        Remember();
    }
    void Remember() {
        RECT r{};
        GetWindowRect(hwnd, &r);
        x = r.left, y = r.top;
        moves = !IsZoomed(hwnd);  // a maximized window only fades
    }
    void Set(float alpha, float dy) {
        SetLayeredWindowAttributes(hwnd, 0, BYTE(std::clamp(alpha, 0.f, 1.f) * 255 + 0.5f), LWA_ALPHA);
        if (moves)
            SetWindowPos(hwnd, nullptr, x, y + int(std::lround(dy * scale)), 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    }
    void Step(double now, double closing_at) {
        if (closing_at >= 0) {
            const float t = std::clamp(float(now - closing_at) / Motion::CLOSE_TIME, 0.f, 1.f);
            Set(1 - t * t, SINK * EaseOut(t));
        } else if (opening) {
            const float t = std::clamp(float(now) / OPEN_TIME, 0.f, 1.f);
            Set(EaseOut(t * 1.3f), RISE * (1 - EaseOut(t)));
            opening = t < 1;
        }
    }
};

// Refresh rate of the monitor the window is on (60 if Windows does not say).
int MonitorRefreshHz(HWND hwnd) {
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &info)) return 60;
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    if (!EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode) || mode.dmDisplayFrequency <= 1) return 60;
    return int(mode.dmDisplayFrequency);
}

void SetTitleBar(HWND hwnd) {
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
    COLORREF caption = RGB(7, 5, 10), text = RGB(207, 174, 110), border = RGB(60, 44, 30);
    DwmSetWindowAttribute(hwnd, 35 /* DWMWA_CAPTION_COLOR */, &caption, sizeof(caption));
    DwmSetWindowAttribute(hwnd, 36 /* DWMWA_TEXT_COLOR */, &text, sizeof(text));
    DwmSetWindowAttribute(hwnd, 34 /* DWMWA_BORDER_COLOR */, &border, sizeof(border));
}

int RunWindow() {
    if (!glfwInit()) {
        actions::Message(nullptr, "Cannot open a window (GLFW).", true);
        return 1;
    }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    Window win;
    win.glfw = glfwCreateWindow(1240, 780, "Bloodborne \xe2\x80\x94 wbp", nullptr, nullptr);
    if (!win.glfw) {
        actions::Message(nullptr, "Cannot open a window (GLFW).", true);
        return 1;
    }
    glfwSetWindowUserPointer(win.glfw, &win);
    HWND hwnd = glfwGetWin32Window(win.glfw);
    SetTitleBar(hwnd);
    WindowFx fx;
    fx.Prepare(hwnd, 1);
    if (HICON icon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1))) {
        SendMessageW(hwnd, WM_SETICON, ICON_BIG, LPARAM(icon));
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL, LPARAM(icon));
    }
    float sx = 1, sy = 1;
    glfwGetWindowContentScale(win.glfw, &sx, &sy);
    win.scale = sx;
    fx.scale = sx;
    glfwSetWindowSizeLimits(win.glfw, int(1040 * sx), int(680 * sx), GLFW_DONT_CARE, GLFW_DONT_CARE);
    int fw, fh;
    glfwGetFramebufferSize(win.glfw, &fw, &fh);
    if (!win.gfx.Init(hwnd, fw, fh, win.scale)) {
        actions::Message(nullptr, "Cannot start Direct3D 11. Update the graphics driver.", true);
        return 1;
    }
    win.ui.window = hwnd;
    win.ui.clipboard_get = [&win] {
        const char* text = glfwGetClipboardString(win.glfw);
        return std::string(text ? text : "");
    };
    win.ui.clipboard_set = [&win](const std::string& text) { glfwSetClipboardString(win.glfw, text.c_str()); };
    Form::browse_folder = [hwnd](const std::string& title, const fs::path& start) {
        return actions::BrowseFolder(hwnd, title, start);
    };

    glfwSetKeyCallback(win.glfw, [](GLFWwindow* w, int key, int scancode, int action, int mods) {
        if (action == GLFW_PRESS || action == GLFW_REPEAT) Of(w)->pending.keys.push_back({key, scancode, mods});
    });
    glfwSetCharCallback(win.glfw, [](GLFWwindow* w, unsigned c) { Of(w)->pending.chars += char32_t(c); });
    glfwSetMouseButtonCallback(win.glfw, [](GLFWwindow* w, int button, int action, int) {
        if (button < 0 || button >= 5) return;
        Input& in = Of(w)->pending;
        if (action == GLFW_PRESS) in.pressed[button] = true, in.down[button] = true;
        else in.released[button] = true, in.down[button] = false;
    });
    glfwSetScrollCallback(win.glfw, [](GLFWwindow* w, double, double y) { Of(w)->pending.wheel += float(y); });
    glfwSetCursorPosCallback(win.glfw, [](GLFWwindow* w, double x, double y) {
        Window* self = Of(w);
        self->pending.x = float(x) / self->scale, self->pending.y = float(y) / self->scale;
    });
    glfwSetCursorEnterCallback(win.glfw, [](GLFWwindow* w, int entered) {
        if (!entered) Of(w)->pending.x = Of(w)->pending.y = -1;
    });
    glfwSetWindowContentScaleCallback(win.glfw, [](GLFWwindow* w, float x, float) { Of(w)->scale = x; });
    glfwSetWindowCloseCallback(win.glfw, [](GLFWwindow* w) {
        Of(w)->close_requested = true;
        glfwSetWindowShouldClose(w, GLFW_FALSE);  // closed by the loop once the animation ends
    });

    Launcher launcher(win.ui);
    launcher.on_language = [] {};
    GLFWcursor* hand = glfwCreateStandardCursor(GLFW_HAND_CURSOR);
    GLFWcursor* ibeam = glfwCreateStandardCursor(GLFW_IBEAM_CURSOR);
    Cursor shown = Cursor::Arrow;

    // Development: BB_LAUNCHER_SHOT=file.png renders BB_LAUNCHER_PAGE with the mouse at
    // BB_LAUNCHER_MOUSE=x,y (DIPs), saves the frame at BB_LAUNCHER_SHOT_AT seconds (2.5) and closes.
    const std::string shot = Env("BB_LAUNCHER_SHOT");
    if (!Env("BB_LAUNCHER_PAGE").empty()) launcher.Open(Env("BB_LAUNCHER_PAGE"));
    float shot_x = -1, shot_y = -1;
    std::sscanf(Env("BB_LAUNCHER_MOUSE").c_str(), "%f,%f", &shot_x, &shot_y);
    int shot_state = 0;
    const double shot_at = Env("BB_LAUNCHER_SHOT_AT").empty() ? 2.5 : std::atof(Env("BB_LAUNCHER_SHOT_AT").c_str());
    fx.Remember();  // where Windows placed it
    glfwShowWindow(win.glfw);
    const auto start = std::chrono::steady_clock::now();
    double last = 0, closing_at = -1, refresh_checked = -10;
    int refresh_hz = 60;
    Motion motion;
    fx.Set(0, WindowFx::RISE);  // shown transparent and a little low, then rises into place
    while (!glfwWindowShouldClose(win.glfw)) {
        const bool iconified = glfwGetWindowAttrib(win.glfw, GLFW_ICONIFIED);
        const bool focused = glfwGetWindowAttrib(win.glfw, GLFW_FOCUSED);
        if (iconified) {
            if (win.close_requested || launcher.WantsClose()) break;  // nothing to animate
            glfwWaitEventsTimeout(0.25);
            continue;
        }
        // In front: the monitor's refresh rate (V-Sync paces the frames; the timer only keeps a
        // missing V-Sync from spinning). 20 behind other windows, and 1 behind the game: the
        // launcher draws on the default adapter (the integrated GPU of a hybrid laptop), whose
        // animated background kept it busy all through the game.
        const double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        if (now - refresh_checked > 1.0) refresh_hz = MonitorRefreshHz(glfwGetWin32Window(win.glfw)), refresh_checked = now;
        const double period = focused ? 0.9 / refresh_hz : launcher.GameRunning() ? 1.0 : 1.0 / 20;
        if (now - last < period) {
            glfwWaitEventsTimeout(period - (now - last));
            continue;
        }
        glfwPollEvents();
        int w, h;
        glfwGetFramebufferSize(win.glfw, &w, &h);
        if (w <= 0 || h <= 0) continue;
        win.gfx.Resize(w, h, win.scale);

        Input& in = win.ui.in;
        const bool down[5] = {win.pending.down[0], win.pending.down[1], win.pending.down[2], win.pending.down[3],
                              win.pending.down[4]};
        in = win.pending;
        in.time = now;
        in.dt = float(std::min(0.1, now - last));
        last = now;
        win.pending = Input{};
        win.pending.x = in.x, win.pending.y = in.y;
        std::copy(down, down + 5, win.pending.down);

        if (!shot.empty()) in.x = shot_x, in.y = shot_y, in.pressed[0] = in.down[0] = false;
        if ((win.close_requested || launcher.WantsClose()) && closing_at < 0) {
            closing_at = now;
            if (!fx.opening) fx.Remember();  // it may have been moved since it opened
        }
        const auto build_start = std::chrono::steady_clock::now();
        win.ui.BeginFrame();
        win.gfx.BeginFrame(float(now), motion.Step(in, win.gfx.Width(), win.gfx.Height(), now, in.dt, closing_at),
                           launcher.ArtLayer());
        launcher.Frame();
        win.ui.EndFrame();
        const auto draw_start = std::chrono::steady_clock::now();
        win.gfx.EndFrame();
        const auto ms = [](auto a, auto b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
        launcher.FrameTimes(ms(build_start, draw_start), ms(draw_start, std::chrono::steady_clock::now()));
        fx.Step(now, closing_at);
        if (closing_at >= 0 && now - closing_at >= Motion::CLOSE_TIME) glfwSetWindowShouldClose(win.glfw, GLFW_TRUE);
        if (win.ui.cursor != shown) {
            shown = win.ui.cursor;
            glfwSetCursor(win.glfw, shown == Cursor::Hand ? hand : shown == Cursor::Text ? ibeam : nullptr);
        }
        if (!shot.empty()) {
            if (shot_state == 1) glfwSetWindowShouldClose(win.glfw, GLFW_TRUE);
            if (shot_state == 0 && now > shot_at) win.gfx.shot = PathFrom(shot), shot_state = 1;
        }
    }
    launcher.Closing();
    glfwDestroyWindow(win.glfw);
    glfwTerminate();
    return 0;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::wstring> args(argv + 1, argv + argc);
    LocalFree(argv);
    MigrateOldNames();

    if (!args.empty() && args[0] == L"--run") {
        AttachOutput();
        return RunGame(std::vector<std::wstring>(args.begin() + 1, args.end()));
    }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    App app;
    app.Load();
    i18n::SetLanguage(app.Str("ui_language"));
    if (args.size() == 3 && args[0] == L"--install-update")
        return actions::FinishUpdate(fs::path(args[1]), std::wcstoul(args[2].c_str(), nullptr, 10));
    actions::CleanUpdateLeftovers();
    // Without a usable game folder there is nothing to play yet: the launcher opens instead.
    std::error_code ec;
    if (std::find(args.begin(), args.end(), L"--play") != args.end() &&
        fs::is_regular_file(PathFrom(app.Str("game_dir")) / L"eboot.bin", ec)) {
        AttachOutput();
        return PlayWithoutWindow(app);
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    return RunWindow();
}
