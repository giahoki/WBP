// SPDX-License-Identifier: GPL-2.0-or-later
#include "runner.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <fstream>

#include "i18n.h"
#include "net.h"
#include "prep/image.h"
#include "prep/key_icons.h"
#include "prep/mods.h"
#include "prep/patches.h"

namespace {

// Co-op: the game server the game asks for messages and bloodstains, and the port of the
// community server's API (summons among them).
constexpr const char* GAME_SERVER = "https://ss4.scej-network.jp:20443";
constexpr int COMMUNITY_API_PORT = 18671;

void SetEnv(std::string_view name, std::string_view value) {
    SetEnvironmentVariableW(Wide(name).c_str(), Wide(value).c_str());
}
void UnsetEnv(std::string_view name) { SetEnvironmentVariableW(Wide(name).c_str(), nullptr); }
void DefaultEnv(const char* name, std::string_view value) {
    if (!HasEnv(name)) SetEnv(name, value);
}

std::wstring ExePath() {
    std::wstring buffer(32768, L'\0');
    buffer.resize(GetModuleFileNameW(nullptr, buffer.data(), DWORD(buffer.size())));
    return buffer;
}

DWORD NoConsole() { return GetConsoleWindow() ? 0 : CREATE_NO_WINDOW; }

// The process's output with an exit code; stdin is the null device.
int Run(const std::wstring& command_line, std::string* output = nullptr) {
    SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
    HANDLE null_in = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, nullptr);
    HANDLE read_end = nullptr, write_end = nullptr;
    if (output) CreatePipe(&read_end, &write_end, &inherit, 0), SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = null_in;
    startup.hStdOutput = output ? write_end : GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = output ? GetStdHandle(STD_ERROR_HANDLE) : GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION pi{};
    std::wstring line = command_line;
    BOOL ok = CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE, NoConsole(), nullptr, nullptr, &startup, &pi);
    CloseHandle(null_in);
    if (write_end) CloseHandle(write_end);
    if (!ok) {
        if (read_end) CloseHandle(read_end);
        return -1;
    }
    if (output) {
        char buffer[4096];
        DWORD n;
        while (ReadFile(read_end, buffer, sizeof(buffer), &n, nullptr) && n) output->append(buffer, n);
        CloseHandle(read_end);
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread), CloseHandle(pi.hProcess);
    return int(code);
}

void WaitFor(DWORD pid) {
    if (HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid)) {
        WaitForSingleObject(h, 30000);
        CloseHandle(h);
    }
}

// True when a shadNet WebAPI answers Bloodborne's summon calls: the route exists there (an
// empty request is refused with 400), where other servers answer 404.
bool SummonService(const std::string& webapi) {
    std::string base = webapi;
    while (!base.empty() && base.back() == '/') base.pop_back();
    net::Reply reply = net::Request("POST", base + "/summon_messenger/create", {}, 4000);
    return reply.status != 0 && reply.status != 404;
}

// user/host_overrides.json for this launch; user/host_overrides.custom.json is used as it is.
void OnlineOverrides(const fs::path& user_dir) {
    const fs::path custom = user_dir / L"host_overrides.custom.json";
    std::error_code ec;
    fs::path path = custom;
    if (!fs::exists(custom, ec)) {
        path = user_dir / L"host_overrides.json";
        std::string community = Env("BB_COMMUNITY_SERVER", COMMUNITY_SERVER);
        while (!community.empty() && community.back() == '/') community.pop_back();
        if (community.find("://") == std::string::npos) community = "https://" + community;
        const std::string host = community.substr(community.find("://") + 3);
        const std::string webapi = Env("BB_SHADNET_WEBAPI", "http://127.0.0.1:31315");
        // Game server -> community server (same port, 20443). Summon signs -> the shadNet WebAPI
        // when that server has the Bloodborne summon service, otherwise the community server.
        nlohmann::json overrides = {{GAME_SERVER, community}};
        if (SummonService(webapi)) {
            for (const char* scheme : {"https", "http"})
                overrides[std::string(scheme) + "://" + host + ":" + std::to_string(COMMUNITY_API_PORT) + "/summon_messenger"] = webapi;
            Say("Co-op: summon signs through " + webapi);
        } else {
            Say("Co-op: " + webapi + " has no summon service; summon signs through " + community);
        }
        WriteText(path, overrides.dump(2) + "\n");
    }
    SetEnv("SHADPS4_HTTP_HOST_OVERRIDES_JSON", PathText(path));
}

fs::path FindExecutable(const wchar_t* name) {
    for (const fs::path& p : {PortDir() / L"bin" / name, PortDir() / L"out" / name}) {
        std::error_code ec;
        if (fs::is_regular_file(p, ec)) return p;
    }
    return {};
}

std::string IniValue(const fs::path& ini, const std::string& key) {
    auto settings = prep::ReadSettings(ini);
    auto it = settings.find(key);
    return it == settings.end() ? std::string() : it->second;
}

}  // namespace

int RunGame(std::vector<std::wstring> args) {
    if (args.size() >= 2 && args[0] == L"--after") {
        WaitFor(DWORD(std::wcstoul(args[1].c_str(), nullptr, 10)));
        args.erase(args.begin(), args.begin() + 2);
    }
    try {
        // As before: relative paths (BB_GAME_DIR, BB_DATA_DIR, fsr4_shaders/) are from the port.
        SetCurrentDirectoryW(PortDir().c_str());
        const fs::path data = DataDir(), out = data / L"out";
        fs::create_directories(out);
        DefaultEnv("BB_CONFIG", PathText(Ini::Path()));
        const fs::path config = PathFrom(Env("BB_CONFIG"));
        std::error_code ec;
        if (!HasEnv("BB_FSR411_DIR") && !fs::is_directory(PortDir() / L"fsr4_411", ec) && fs::is_directory(data / L"fsr4_411", ec))
            SetEnv("BB_FSR411_DIR", PathText(data / L"fsr4_411"));

        const fs::path probe = HasEnv("BB_GAME_EXE") ? PathFrom(Env("BB_GAME_EXE")) : FindExecutable(L"wbp-game.exe");
        if (probe.empty() || !fs::is_regular_file(probe, ec))
            throw Failure("wbp-game.exe not found: build it with build.sh (MSYS2 CLANG64) or use a packaged build.");
        // Development builds run from the MSYS2 tree: its CLANG64 DLLs (SDL3, FFmpeg, ...).
        if (!fs::is_regular_file(probe.parent_path() / L"SDL3.dll", ec)) {
            const fs::path clang64 = PathFrom(Env("MSYS2_ROOT", "C:\\msys64")) / L"clang64" / L"bin";
            if (fs::is_directory(clang64, ec)) SetEnv("PATH", PathText(clang64) + ";" + Env("PATH"));
        }

        const fs::path game = HasEnv("BB_GAME_DIR") ? PathFrom(Env("BB_GAME_DIR")) : PortDir().parent_path() / L"CUSA03173";
        if (!fs::is_regular_file(game / L"eboot.bin", ec)) throw Failure("No eboot.bin in " + PathText(game) + " (set BB_GAME_DIR).");

        // Keyboard and mouse button prompts drawn from the bindings (wbp.ini key_icons=0: off).
        const auto settings = prep::ReadSettings(config);
        std::vector<fs::path> layers;
        UnsetEnv("BB_DODGE_ICON");  // a restart from the game inherits the last run's value
        if (!settings.count("key_icons") || settings.at("key_icons") == "1") {
            try {
                const bool native = !settings.count("input_mode") || settings.at("input_mode") != "emulation";
                const auto bindings = LoadBindings(settings);
                if (auto icons = prep::BuildKeyIcons(game, out, bindings, native)) {
                    layers.push_back(*icons);
                    if (prep::DodgeIconSplit(bindings, native)) SetEnv("BB_DODGE_ICON", prep::DODGE_ICON);
                }
            } catch (const std::exception& error) {
                SayError(std::string("Key icons: ") + error.what());
            }
        }
        prep::ModOptions mods;
        mods.game = game, mods.out = out, mods.extra = layers;
        mods.mods_dir = HasEnv("BB_MODS_DIR") ? PathFrom(Env("BB_MODS_DIR")) : data / L"mods";
        mods.config = HasEnv("BB_MODS_CONFIG") ? PathFrom(Env("BB_MODS_CONFIG")) : data / L"mods.json";
        mods.enabled = Env("BB_MODS_ENABLED", "1") == "1";
        const fs::path view = prep::BuildOverlay(mods);
        const bool overlay = view != fs::canonical(game);
        struct Cleanup {
            fs::path path;
            ~Cleanup() {
                if (!path.empty()) prep::RemoveOverlay(path);
            }
        } cleanup{overlay ? view : fs::path()};

        // The eboot and the system modules cannot be modded: the image comes from the game itself.
        prep::BuildImage(game, out);
        const std::string problem = prep::GameProblem(game);
        if (problem == "patched_eboot")
            throw Failure("eboot.bin is the 1.09 executable with a 60 FPS patch baked in (the gestures menu crashes the "
                          "game): use the clean 1.09 eboot.bin; the port has its own frame rate patches. "
                          "BB_SKIP_GAME_CHECK=1 skips this check.");
        if (problem == "damaged_files")
            throw Failure("The game files are damaged: the shaders in dvdroot_ps4/shader do not unpack, and the game "
                          "would hang while loading them. Extract the game and the 1.09 update again with another "
                          "tool. BB_SKIP_GAME_CHECK=1 skips this check.");
        prep::WriteContentProfile(game, out, Env("BB_CONTENT_SKU", "full"));

        // Patches exist for game version 01.09 only: other versions keep 30 FPS.
        const std::string version = prep::GameVersion(game);
        const bool patched = version.empty() || version == PATCH_VERSION || HasEnv("BB_FORCE_PATCHES");
        if (!patched) Say("Game version " + version + ": community patches need 01.09; 30 FPS, no effect patches");
        // Sizes chosen below for the previous launch are recomputed after an in-game restart.
        if (Env("BB_AUTO_RENDER_RES") == "1")
            for (const char* key : {"BB_RENDER_RES", "BB_OUTPUT_RES", "BB_AUTO_RENDER_RES"}) UnsetEnv(key);
        const std::string fps = patched ? Env("BB_FPS", "uncap") : "30";
        std::optional<std::pair<prep::Size, prep::Size>> scaled;
        if (Env("BB_RENDER_RES").empty()) scaled = prep::ScaledSizes(prep::ReadSettings(config));
        std::string live = "0";
        if (scaled) {
            live = Env("BB_LIVE_RES");
            if (live.empty()) live = IniValue(config, "live_resolution");
            if (live.empty()) live = "0";
            if (live == "auto") {
                std::string printed;
                const fs::path caps = probe.parent_path() / L"bb-gpu-capabilities.exe";
                int code = Run(QuoteArg(caps.native()) + L" --live-resolution", &printed);
                live = code == 0 ? Trim(printed) : "0";
            }
            live = (live == "1" || !patched) ? "1" : "0";
        }
        if (live == "1") {
            Say("Output " + scaled->second.Text() + ": live resolution changes (live_resolution=0: startup patch)");
        } else if (scaled) {
            SetEnv("BB_RENDER_RES", scaled->first.Text());
            SetEnv("BB_OUTPUT_RES", scaled->second.Text());
            SetEnv("BB_AUTO_RENDER_RES", "1");
            DefaultEnv("BB_DMEM_MB", "9152");
            Say("Output " + scaled->second.Text() + ": scene " + scaled->first.Text() + ", direct memory " +
                Env("BB_DMEM_MB") + " MiB (live_resolution=1: live changes)");
        }
        prep::PatchOptions patches;
        patches.xml = PortDir() / L"patches" / L"Bloodborne.xml";
        patches.out = out, patches.game = view, patches.settings = config, patches.fps = fps;
        patches.extra = Env("BB_PATCHES"), patches.render_res = Env("BB_RENDER_RES");
        patches.patches_dir = HasEnv("BB_PATCHES_DIR") ? PathFrom(Env("BB_PATCHES_DIR")) : data / L"patches";
        patches.patches_config = HasEnv("BB_PATCHES_CONFIG") ? PathFrom(Env("BB_PATCHES_CONFIG")) : data / L"patches.json";
        prep::CompilePatches(patches);
        if (!HasEnv("BB_VBLANK_HZ")) SetEnv("BB_VBLANK_HZ", fps == "uncap" ? "0" : fps == "90" ? "90" : "60");

        // GPU caches (shaders, pipelines) beside the saves; read before main(), so set here.
        const fs::path user_dir = fs::absolute(HasEnv("BB_USER_DIR") ? PathFrom(Env("BB_USER_DIR")) : data / L"user");
        fs::create_directories(user_dir);
        DefaultEnv("BB_GPU_USER_DIR", PathText(user_dir));
        if (Env("BB_ONLINE") == "1") OnlineOverrides(user_dir);
        // The in-game restart starts this role again once this process is gone.
        std::wstring restart = QuoteArg(ExePath()) + L" --run --after " + std::to_wstring(GetCurrentProcessId());
        for (auto& a : args) restart += L" " + QuoteArg(a);
        SetEnv("BB_RESTART_COMMAND", Utf8(restart));
        std::wstring command = QuoteArg(probe.native()) + L" " + QuoteArg((out / L"boot-linked.bin").native()) +
                               L" --content-profile " + QuoteArg((out / L"content.bin").native()) + L" --patches " +
                               QuoteArg((out / L"patches.bin").native()) + L" --app0 " + QuoteArg(view.native()) +
                               L" --user " + QuoteArg(user_dir.native()) + L" --timeout " + Wide(Env("BB_TIMEOUT", "0"));
        for (auto& a : args) command += L" " + QuoteArg(a);
        // No stdin: an inherited pipe (shells such as Git Bash) cost the game its console output.
        const int code = Run(command);
        if (code < 0) throw Failure("cannot start " + PathText(probe));
        // The game's own exit code in the log: a crash shows here even when the game printed
        // nothing (the in-game restart ends this launch with 0).
        const uint32_t status = uint32_t(code);
        const char* meaning = status == 0           ? "normal exit"
                              : status == 0xC0000005 ? "crash: access violation"
                              : status == 0xC00000FD ? "crash: stack overflow"
                              : status == 0xC0000409 ? "crash: stack buffer overrun / fast fail"
                              : status == 0xC000001D ? "crash: illegal instruction"
                              : status == 0xC0000094 ? "crash: integer division by zero"
                              : status == 0xE06D7363 ? "crash: unhandled C++ exception"
                              : status == 0xC0000017 ? "out of memory"
                              : status == 0xC000013A ? "closed (Ctrl+C / console closed)"
                              : status == 1          ? "stopped (error or the Stop button)"
                                                     : "unexpected";
        char line[160];
        std::snprintf(line, sizeof(line), "wbp-game exited with code %d (0x%08X): %s", code, status, meaning);
        Say(line);
        return code;
    } catch (const std::exception& error) {
        SayError(error.what());
        return 1;
    }
}

// ---- the game from the launcher ------------------------------------------------------------

GameProcess::~GameProcess() {
    if (reader_.joinable()) reader_.detach();
}

void GameProcess::Start(const App& app) {
    if (running_) return;
    if (reader_.joinable()) reader_.join();
    SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
    HANDLE read_end = nullptr, write_end = nullptr;
    if (!CreatePipe(&read_end, &write_end, &inherit, 0)) throw Failure("CreatePipe failed");
    SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);
    HANDLE null_in = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = null_in, startup.hStdOutput = write_end, startup.hStdError = write_end;
    PROCESS_INFORMATION pi{};
    std::wstring line = QuoteArg(ExePath()) + L" --run";
    const std::wstring env = GameEnvironment(app).Build();
    BOOL ok = CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED,
                             const_cast<wchar_t*>(env.c_str()), PortDir().c_str(), &startup, &pi);
    CloseHandle(write_end);
    CloseHandle(null_in);
    if (!ok) {
        CloseHandle(read_end);
        throw Failure(Fmt(T("Could not start: {}", "Не удалось запустить: {}"), std::to_string(GetLastError())));
    }
    // A job holds this process and everything it starts (wbp-game.exe, the in-game restarts).
    job_ = CreateJobObjectW(nullptr, nullptr);
    if (job_) AssignProcessToJobObject(job_, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    process_ = pi.hProcess;
    {
        std::lock_guard guard(lock_);
        status_ = "";
    }
    running_ = true;
    const fs::path log_dir = UserDir(app);
    reader_ = std::thread(&GameProcess::Read, this, read_end, log_dir);
}

void GameProcess::Read(void* pipe, fs::path log_dir) {
    std::error_code ec;
    fs::create_directories(log_dir, ec);
    std::ofstream log(log_dir / L"last_run.log", std::ios::binary | std::ios::trunc);
    std::string partial;
    char buffer[8192];
    DWORD n;
    while (ReadFile(pipe, buffer, sizeof(buffer), &n, nullptr) && n) {
        std::string text(buffer, n);
        text = Replace(text, "\r\n", "\n");
        if (log) log.write(text.data(), std::streamsize(text.size())), log.flush();
        partial += text;
        size_t nl = partial.rfind('\n');
        std::string lines = nl == std::string::npos ? std::string() : partial.substr(0, nl + 1);
        if (nl != std::string::npos) partial.erase(0, nl + 1);
        std::lock_guard guard(lock_);
        if (lines.find("Entering original x86-64 code") != std::string::npos) status_ = "running";
        else if (lines.find("restarting through") != std::string::npos) status_ = "restarting";
        pending_ += lines;
    }
    CloseHandle(pipe);
    WaitForSingleObject(process_, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(process_, &code);
    CloseHandle(process_);
    process_ = nullptr;
    if (job_) CloseHandle(job_), job_ = nullptr;
    {
        std::lock_guard guard(lock_);
        pending_ += partial;
        pending_ += Fmt(T("\n— the game exited (code {}) —\n", "\n— игра завершилась (код {}) —\n"), std::to_string(code));
        status_ = "";
    }
    exit_code_ = int(code);
    running_ = false;
}

void GameProcess::Stop() {
    if (job_) TerminateJobObject(job_, 1);
}

std::string GameProcess::TakeLog() {
    std::lock_guard guard(lock_);
    std::string out;
    out.swap(pending_);
    return out;
}

std::string GameProcess::Status() const {
    std::lock_guard guard(lock_);
    return status_;
}

// ---- --play ----------------------------------------------------------------------------------

int PlayWithoutWindow(const App& app) {
    const fs::path log_dir = UserDir(app);
    std::error_code ec;
    fs::create_directories(log_dir, ec);
    EnvBlock env = GameEnvironment(app);
    SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
    HANDLE read_end = nullptr, write_end = nullptr;
    CreatePipe(&read_end, &write_end, &inherit, 0);
    SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);
    HANDLE null_in = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = null_in, startup.hStdOutput = write_end, startup.hStdError = write_end;
    PROCESS_INFORMATION pi{};
    std::wstring line = QuoteArg(ExePath()) + L" --run";
    const std::wstring block = env.Build();
    BOOL ok = CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
                             const_cast<wchar_t*>(block.c_str()), PortDir().c_str(), &startup, &pi);
    CloseHandle(write_end);
    CloseHandle(null_in);
    if (!ok) return 1;
    std::ofstream log(log_dir / L"last_run.log", std::ios::binary | std::ios::trunc);
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    char buffer[8192];
    DWORD n, written;
    while (ReadFile(read_end, buffer, sizeof(buffer), &n, nullptr) && n) {
        log.write(buffer, n), log.flush();
        if (out && out != INVALID_HANDLE_VALUE) WriteFile(out, buffer, n, &written, nullptr);
    }
    CloseHandle(read_end);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread), CloseHandle(pi.hProcess);
    return int(code);
}
