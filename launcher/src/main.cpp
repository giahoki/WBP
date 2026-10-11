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
};

Window* Of(GLFWwindow* w) { return static_cast<Window*>(glfwGetWindowUserPointer(w)); }

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
    if (HICON icon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1))) {
        SendMessageW(hwnd, WM_SETICON, ICON_BIG, LPARAM(icon));
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL, LPARAM(icon));
    }
    float sx = 1, sy = 1;
    glfwGetWindowContentScale(win.glfw, &sx, &sy);
    win.scale = sx;
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

    Launcher launcher(win.ui);
    launcher.on_language = [] {};
    GLFWcursor* hand = glfwCreateStandardCursor(GLFW_HAND_CURSOR);
    GLFWcursor* ibeam = glfwCreateStandardCursor(GLFW_IBEAM_CURSOR);
    Cursor shown = Cursor::Arrow;

    // Development: BB_LAUNCHER_SHOT=file.png renders BB_LAUNCHER_PAGE with the mouse at
    // BB_LAUNCHER_MOUSE=x,y (DIPs), saves the frame and closes.
    const std::string shot = Env("BB_LAUNCHER_SHOT");
    if (!Env("BB_LAUNCHER_PAGE").empty()) launcher.Open(Env("BB_LAUNCHER_PAGE"));
    float shot_x = -1, shot_y = -1;
    std::sscanf(Env("BB_LAUNCHER_MOUSE").c_str(), "%f,%f", &shot_x, &shot_y);
    int shot_state = 0;
    glfwShowWindow(win.glfw);
    const auto start = std::chrono::steady_clock::now();
    double last = 0;
    while (!glfwWindowShouldClose(win.glfw)) {
        const bool iconified = glfwGetWindowAttrib(win.glfw, GLFW_ICONIFIED);
        const bool focused = glfwGetWindowAttrib(win.glfw, GLFW_FOCUSED);
        if (iconified) {
            glfwWaitEventsTimeout(0.25);
            continue;
        }
        // About 60 frames per second in front, 20 behind other windows.
        const double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        const double period = focused ? 1.0 / 60 : 1.0 / 20;
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
        win.ui.BeginFrame();
        win.gfx.BeginFrame(float(now), in.x / std::max(1.f, win.gfx.Width()), 1 - in.y / std::max(1.f, win.gfx.Height()),
                           launcher.ArtLayer());
        launcher.Frame();
        win.ui.EndFrame();
        win.gfx.EndFrame();
        if (win.ui.cursor != shown) {
            shown = win.ui.cursor;
            glfwSetCursor(win.glfw, shown == Cursor::Hand ? hand : shown == Cursor::Text ? ibeam : nullptr);
        }
        if (launcher.WantsClose()) glfwSetWindowShouldClose(win.glfw, GLFW_TRUE);
        if (!shot.empty()) {
            if (shot_state == 1) glfwSetWindowShouldClose(win.glfw, GLFW_TRUE);
            if (shot_state == 0 && now > 2.5) win.gfx.shot = PathFrom(shot), shot_state = 1;
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
