// SPDX-License-Identifier: GPL-2.0-or-later
#include "actions.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <tlhelp32.h>
#include <wrl/client.h>

#include <algorithm>
#include <fstream>
#include <regex>
#include <mutex>
#include <thread>

#include <libdeflate.h>
#include <miniz.h>

#include "i18n.h"
#include "net.h"

using Microsoft::WRL::ComPtr;

namespace actions {
namespace {

std::wstring ExePath() {
    std::wstring buffer(32768, L'\0');
    buffer.resize(GetModuleFileNameW(nullptr, buffer.data(), DWORD(buffer.size())));
    return buffer;
}

// Never copied over an installation by an update (the package does not hold them either).
const char* USER_FILES[] = {"user", "out", "mods", "wbp.ini", "mods.json", "patches.json", "last_run.log"};

fs::path UpdateDir() {
    wchar_t temp[MAX_PATH + 1];
    GetTempPathW(MAX_PATH + 1, temp);
    return fs::path(temp) / L"wbp-update";
}

// ---- Steam's shortcuts.vdf: binary KeyValues (0 map, 1 string, 2 int32, 7 uint64, 8 end) ----

struct Kv {
    uint8_t type;
    std::string key, text;
    uint64_t number = 0;
    std::vector<Kv> children;
};

std::string CStr(const Bytes& d, size_t& at) {
    size_t end = at;
    while (end < d.size() && d[end]) ++end;
    if (end >= d.size()) throw Failure("truncated shortcuts.vdf");
    std::string s(d.begin() + ptrdiff_t(at), d.begin() + ptrdiff_t(end));
    at = end + 1;
    return s;
}

std::vector<Kv> Parse(const Bytes& d, size_t& at) {
    std::vector<Kv> items;
    while (at < d.size()) {
        const uint8_t type = d[at++];
        if (type == 8) return items;
        Kv kv{type, CStr(d, at)};
        if (type == 0) kv.children = Parse(d, at);
        else if (type == 1) kv.text = CStr(d, at);
        else if (type == 2) kv.number = Le32(d, at), at += 4;
        else if (type == 7) kv.number = Le64(d, at), at += 8;
        else throw Failure("unknown field type " + std::to_string(type) + " in shortcuts.vdf");
        items.push_back(std::move(kv));
    }
    return items;
}

void Dump(const std::vector<Kv>& items, Bytes& out) {
    for (const Kv& kv : items) {
        out.push_back(kv.type);
        AppendBytes(out, kv.key.data(), kv.key.size() + 1);
        if (kv.type == 0) Dump(kv.children, out), out.push_back(8);
        else if (kv.type == 1) AppendBytes(out, kv.text.c_str(), kv.text.size() + 1);
        else if (kv.type == 2) { uint32_t v = uint32_t(kv.number); AppendBytes(out, &v, 4); }
        else if (kv.type == 7) Append64(out, kv.number);
    }
}

Kv Str(const char* k, std::string v) { return Kv{1, k, std::move(v)}; }
Kv Int(const char* k, uint32_t v) { Kv kv{2, k}; kv.number = v; return kv; }

std::vector<Kv> Entry(const std::string& name, const std::string& exe, const std::string& start_dir,
                      const std::string& icon, const std::string& options) {
    const std::string quoted = "\"" + exe + "\"", key = quoted + name;
    // The id Steam gives a non-Steam game: CRC32 of the quoted exe and the name, top bit set.
    const uint32_t appid = libdeflate_crc32(0, key.data(), key.size()) | 0x80000000u;
    return {Int("appid", appid), Str("AppName", name), Str("Exe", quoted), Str("StartDir", "\"" + start_dir + "\""),
            Str("icon", icon), Str("ShortcutPath", ""), Str("LaunchOptions", options), Int("IsHidden", 0),
            Int("AllowDesktopConfig", 1), Int("AllowOverlay", 1), Int("OpenVR", 0), Int("Devkit", 0),
            Str("DevkitGameID", ""), Int("DevkitOverrideAppID", 0), Int("LastPlayTime", 0), Str("FlatpakAppID", ""),
            Kv{0, "tags"}};
}

void AddShortcut(const fs::path& vdf, const std::string& name, const std::string& exe, const std::string& start_dir,
                 const std::string& icon) {
    std::error_code ec;
    Bytes data = fs::exists(vdf, ec) ? ReadFile(vdf) : Bytes();
    size_t at = 0;
    std::vector<Kv> root = data.empty() ? std::vector<Kv>() : Parse(data, at);
    Kv* shortcuts = nullptr;
    for (Kv& kv : root)
        if (kv.type == 0 && Lower(kv.key) == "shortcuts") shortcuts = &kv;
    if (!shortcuts) {
        root.push_back(Kv{0, "shortcuts"});
        shortcuts = &root.back();
    }
    std::vector<Kv> fresh = Entry(name, exe, start_dir, icon, "");
    const std::string target = Lower("\"" + exe + "\"");
    bool updated = false;
    for (Kv& entry : shortcuts->children) {
        if (entry.type != 0) continue;
        auto exe_field = std::find_if(entry.children.begin(), entry.children.end(),
                                      [](const Kv& f) { return Lower(f.key) == "exe"; });
        if (exe_field == entry.children.end() || Lower(exe_field->text) != target) continue;
        // Keep what Steam or the player added (play time, tags).
        for (Kv& f : fresh) {
            const std::string k = Lower(f.key);
            if (k != "lastplaytime" && k != "tags") continue;
            for (const Kv& old : entry.children)
                if (Lower(old.key) == k) f = old;
        }
        entry.children = fresh;
        updated = true;
        break;
    }
    if (!updated) shortcuts->children.push_back(Kv{0, std::to_string(shortcuts->children.size()), "", 0, fresh});
    if (!data.empty()) {
        fs::path backup = vdf;
        backup += L".bak";
        WriteFile(backup, data);
    }
    Bytes out;
    Dump(root, out);
    out.push_back(8);
    WriteFile(vdf, out);
}

std::optional<fs::path> SteamFolder() {
    wchar_t value[1024];
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ, nullptr, value, &size))
        return std::nullopt;
    fs::path path(value);
    std::error_code ec;
    return fs::is_directory(path, ec) ? std::optional(path.make_preferred()) : std::nullopt;
}

bool SteamRunning() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W e{sizeof(e)};
    bool found = false;
    for (BOOL ok = Process32FirstW(snap, &e); ok; ok = Process32NextW(snap, &e))
        if (_wcsicmp(e.szExeFile, L"steam.exe") == 0) found = true;
    CloseHandle(snap);
    return found;
}

int Capture(const std::wstring& command, std::string& out, std::string& err, DWORD timeout_ms) {
    SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
    HANDLE out_r, out_w, err_r, err_w;
    CreatePipe(&out_r, &out_w, &inherit, 0), CreatePipe(&err_r, &err_w, &inherit, 0);
    SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0), SetHandleInformation(err_r, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = out_w, si.hStdError = err_w;
    PROCESS_INFORMATION pi{};
    std::wstring line = command;
    BOOL ok = CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(out_w), CloseHandle(err_w);
    if (!ok) {
        CloseHandle(out_r), CloseHandle(err_r);
        return -1;
    }
    auto drain = [](HANDLE h, std::string* s) {
        char b[4096];
        DWORD n;
        while (ReadFile(h, b, sizeof(b), &n, nullptr) && n) s->append(b, n);
    };
    std::thread t(drain, err_r, &err);
    drain(out_r, &out);
    t.join();
    CloseHandle(out_r), CloseHandle(err_r);
    if (WaitForSingleObject(pi.hProcess, timeout_ms) == WAIT_TIMEOUT) TerminateProcess(pi.hProcess, 1);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread), CloseHandle(pi.hProcess);
    return int(code);
}

bool UserFile(const fs::path& name) {
    const std::string n = PathText(name);
    return std::any_of(std::begin(USER_FILES), std::end(USER_FILES), [&](const char* u) { return IEquals(n, u); });
}

void CopyTree(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    fs::create_directories(to, ec);
    for (auto& entry : fs::directory_iterator(from)) {
        if (UserFile(entry.path().filename())) continue;
        const fs::path target = to / entry.path().filename();
        if (entry.is_directory()) CopyTree(entry.path(), target);
        else if (!CopyFileW(entry.path().c_str(), target.c_str(), FALSE)) throw Failure("in use: " + PathText(target));
    }
}

}  // namespace

void Open(const fs::path& path) { ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL); }

void OpenUrl(const std::string& url) {
    if (StartsWith(url, "https://") || StartsWith(url, "http://"))
        ShellExecuteW(nullptr, L"open", Wide(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

std::optional<fs::path> BrowseFolder(void* owner, const std::string& title, const fs::path& start) {
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
        return std::nullopt;
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dialog->SetTitle(Wide(title).c_str());
    ComPtr<IShellItem> folder;
    std::error_code ec;
    if (!start.empty() && fs::is_directory(start, ec) &&
        SUCCEEDED(SHCreateItemFromParsingName(start.c_str(), nullptr, IID_PPV_ARGS(&folder))))
        dialog->SetFolder(folder.Get());
    if (FAILED(dialog->Show(static_cast<HWND>(owner)))) return std::nullopt;
    ComPtr<IShellItem> item;
    PWSTR path = nullptr;
    if (FAILED(dialog->GetResult(&item)) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) return std::nullopt;
    fs::path result(path);
    CoTaskMemFree(path);
    return result;
}

bool Confirm(void* owner, const std::string& text) {
    return MessageBoxW(static_cast<HWND>(owner), Wide(text).c_str(), L"Bloodborne", MB_YESNO | MB_ICONQUESTION) == IDYES;
}

void Message(void* owner, const std::string& text, bool error) {
    MessageBoxW(static_cast<HWND>(owner), Wide(text).c_str(), L"Bloodborne", MB_OK | (error ? MB_ICONERROR : MB_ICONINFORMATION));
}

std::string DesktopShortcut() {
    ComPtr<IShellLinkW> link;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))))
        throw Failure("cannot create the shortcut");
    const std::wstring exe = ExePath();
    link->SetPath(exe.c_str());
    link->SetWorkingDirectory(PortDir().c_str());
    link->SetIconLocation(exe.c_str(), 0);
    PWSTR desktop = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &desktop))) throw Failure("no desktop folder");
    const fs::path file = fs::path(desktop) / L"Bloodborne.lnk";
    CoTaskMemFree(desktop);
    ComPtr<IPersistFile> persist;
    link.As(&persist);
    if (!persist || FAILED(persist->Save(file.c_str(), TRUE))) throw Failure("cannot save " + PathText(file));
    return T("Shortcut created on the desktop.", "Ярлык создан на рабочем столе.");
}

std::string AddToSteam(void* owner) {
    auto steam = SteamFolder();
    std::vector<fs::path> configs;
    std::error_code ec;
    if (steam)
        for (auto& d : fs::directory_iterator(*steam / L"userdata", ec)) {
            const std::string name = PathText(d.path().filename());
            if (d.is_directory() && name != "0" && std::all_of(name.begin(), name.end(), ::isdigit))
                configs.push_back(d.path() / L"config");
        }
    if (configs.empty()) throw Failure(T("Steam was not found on this PC (or nobody has signed in to it yet)."));
    const fs::path exe = PortDir() / L"Play Bloodborne.exe";
    bool restart = false;
    if (SteamRunning()) {
        if (!Confirm(owner, T("Steam has to close while the game is added; it starts again afterwards. Close Steam now?")))
            return {};
        ShellExecuteW(nullptr, L"open", (*steam / L"steam.exe").c_str(), L"-shutdown", nullptr, SW_HIDE);
        int i = 0;
        for (; i < 60 && SteamRunning(); ++i) Sleep(500);
        if (i == 60) throw Failure(T("Steam did not close. Close it yourself and try again."));
        Sleep(1000);
        restart = true;
    }
    try {
        for (const fs::path& config : configs)
            AddShortcut(config / L"shortcuts.vdf", "Bloodborne", PathText(exe), PathText(PortDir()), PathText(exe));
    } catch (const std::exception& error) {
        throw Failure(Fmt(T("Could not add the game to Steam: {}"), std::string(error.what())));
    }
    if (restart) ShellExecuteW(nullptr, L"open", L"steam://open/games", nullptr, nullptr, SW_SHOWNORMAL);
    return T("Bloodborne is in your Steam library. It starts with the settings saved in this launcher.");
}

Release LatestRelease() {
    net::Reply reply = net::Request("GET", RELEASES_API, {}, 15000, 0, "application/vnd.github+json");
    if (!reply.Ok()) throw Failure(reply.error.empty() ? "HTTP " + std::to_string(reply.status) : reply.error);
    nlohmann::json release;
    try {
        release = nlohmann::json::parse(reply.body);
    } catch (...) {
        throw Failure("bad answer from GitHub");
    }
    Release r;
    const std::string tag = release.value("tag_name", "");
    std::regex digits("\\d+");
    for (auto it = std::sregex_iterator(tag.begin(), tag.end(), digits); it != std::sregex_iterator(); ++it)
        r.version += (r.version.empty() ? "" : ".") + it->str();
    if (r.version.empty()) throw Failure("no version in the latest release");
    for (auto& asset : release.value("assets", nlohmann::json::array()))
        if (EndsWith(Lower(asset.value("name", "")), ".zip")) {
            r.zip = asset.value("browser_download_url", "");
            break;
        }
    r.page = release.value("html_url", RELEASES_PAGE);
    return r;
}

bool Newer(const std::string& version, const std::string& than) {
    auto parts = [](const std::string& v) {
        std::vector<int> out;
        std::regex digits("\\d+");
        for (auto it = std::sregex_iterator(v.begin(), v.end(), digits); it != std::sregex_iterator(); ++it)
            out.push_back(std::stoi(it->str()));
        return out;
    };
    return parts(version) > parts(than);
}

void InstallUpdate(const Release& release, const std::function<void(int)>& percent) {
    const fs::path dir = UpdateDir();
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    const fs::path archive = dir / L"update.zip";
    net::Download(release.zip, archive, [&](uint64_t done, uint64_t total) {
        if (percent) percent(total ? int(done * 100 / total) : 0);
    });
    mz_zip_archive zip{};
    const std::string archive_utf8 = PathText(archive);
    // miniz opens files with fopen: a path in the ANSI code page is safest, so read it ourselves.
    Bytes bytes = ReadFile(archive);
    if (!mz_zip_reader_init_mem(&zip, bytes.data(), bytes.size(), 0)) throw Failure("the download is not a zip file");
    fs::path exe_folder;
    for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&zip); ++i) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st)) continue;
        const fs::path relative = PathFrom(st.m_filename).lexically_normal();
        if (relative.is_absolute() || PathText(relative).rfind("..", 0) == 0) continue;
        const fs::path target = dir / L"new" / relative;
        if (mz_zip_reader_is_file_a_directory(&zip, i)) {
            fs::create_directories(target, ec);
            continue;
        }
        size_t size = 0;
        void* data = mz_zip_reader_extract_to_heap(&zip, i, &size, 0);
        if (!data) {
            mz_zip_reader_end(&zip);
            throw Failure("cannot unpack " + std::string(st.m_filename));
        }
        WriteFile(target, data, size);
        mz_free(data);
        if (IEquals(PathText(target.filename()), "Bloodborne.exe") && exe_folder.empty()) exe_folder = target.parent_path();
    }
    mz_zip_reader_end(&zip);
    fs::remove(archive, ec);
    if (exe_folder.empty()) throw Failure("Bloodborne.exe is missing from the download");
    std::wstring line = QuoteArg((exe_folder / L"Bloodborne.exe").native()) + L" --install-update " +
                        QuoteArg(PortDir().native()) + L" " + std::to_wstring(GetCurrentProcessId());
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, 0, nullptr, exe_folder.c_str(), &si, &pi))
        throw Failure("cannot start the new version");
    CloseHandle(pi.hThread), CloseHandle(pi.hProcess);
}

int FinishUpdate(const fs::path& target, unsigned long wait_pid) {
    if (HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, wait_pid)) {
        WaitForSingleObject(h, 60000);
        CloseHandle(h);
    }
    bool done = false;
    for (int attempt = 0; attempt < 30 && !done; ++attempt) {
        try {
            CopyTree(PortDir(), target);
            done = true;
        } catch (const std::exception&) {
            Sleep(1000);  // a file still in use: the old launcher is closing
        }
    }
    if (!done) {
        Message(nullptr, T("Could not install the update. Download it from the releases page.",
                           "Не удалось установить обновление. Скачайте его со страницы релизов."), true);
        OpenUrl(RELEASES_PAGE);
        return 1;
    }
    std::wstring line = QuoteArg((target / L"Bloodborne.exe").native());
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, 0, nullptr, target.c_str(), &si, &pi))
        CloseHandle(pi.hThread), CloseHandle(pi.hProcess);
    return 0;
}

void CleanUpdateLeftovers() {
    const fs::path dir = UpdateDir();
    std::error_code ec;
    const fs::path inside = PortDir().lexically_relative(dir);
    const bool running_from_update = !inside.empty() && inside.native().rfind(L"..", 0) != 0;
    if (!running_from_update) fs::remove_all(dir, ec);  // what a finished update left behind
}

namespace {
std::mutex gpu_lock;
GpuInfo gpu_found;
}  // namespace

GpuInfo DetectedGpu() {
    std::lock_guard g(gpu_lock);
    return gpu_found;
}

std::string DetectGpu() {
    fs::path exe = PortDir() / L"bin" / L"bb-gpu-capabilities.exe";
    std::error_code ec;
    if (!fs::is_regular_file(exe, ec)) exe = PortDir() / L"out" / L"bb-gpu-capabilities.exe";
    if (!fs::is_regular_file(exe.parent_path() / L"SDL3.dll", ec)) {
        const fs::path clang64 = PathFrom(Env("MSYS2_ROOT", "C:\\msys64")) / L"clang64" / L"bin";
        SetEnvironmentVariableW(L"PATH", (clang64.native() + L";" + Wide(Env("PATH"))).c_str());
    }
    std::string out, err;
    const int code = Capture(QuoteArg(exe.native()) + L" --live-resolution", out, err, 30000);
    for (std::string line : Split(err, '\n')) {
        if (!StartsWith(line, "GPU: ")) continue;
        // "GPU: NAME, discrete, 7949 MiB: ..."
        const std::vector<std::string> parts = Split(Split(line.substr(5), ':')[0], ',');
        std::string name = parts[0];
        {
            std::lock_guard g(gpu_lock);
            gpu_found = {Trim(name), false, 0};
            for (size_t i = 1; i < parts.size(); ++i) {
                const std::string part = Trim(parts[i]);
                if (part == "integrated") gpu_found.integrated = true;
                if (part.find("MiB") != std::string::npos) try { gpu_found.vram_mib = std::stoi(part); } catch (...) {}
            }
        }
        return Fmt(T("✓ Graphics card: {}", "✓ Видеокарта: {}"), Trim(name));
    }
    if (code != 0 && code != -1)
        return T("✗ No Vulkan 1.3 graphics card found (update the driver)", "✗ Не найдена видеокарта с Vulkan 1.3 (обновите драйвер)");
    return T("• Graphics card: not checked", "• Видеокарта: не проверена");
}

std::string TestConnection(const App& app) {
    std::string server = Trim(app.Str("online_server"));
    if (server.empty()) server = App::Defaults()["online_server"].get<std::string>();
    const std::string webapi = WebapiSetting(app), community = CommunityAddress(app.Str("online_community"));
    std::vector<std::string> lines;
    net::Reply game = net::Get(community + ":20443/bb-eu/ss.info", 6000, 64);
    lines.push_back(game.status ? Fmt(T("Game server {0}: reachable"), community)
                                : Fmt(T("Game server {0}: not reachable ({1})"), community, game.error));
    const size_t colon = server.rfind(':');
    std::string host = colon == std::string::npos ? server : server.substr(0, colon);
    int port = 31313;
    if (colon != std::string::npos) try { port = std::stoi(server.substr(colon + 1)); } catch (...) {}
    const std::string tcp = net::TcpCheck(host, port, 4000);
    lines.push_back(tcp.empty() ? Fmt(T("Server {0}: reachable"), server) : Fmt(T("Server {0}: not reachable ({1})"), server, tcp));
    std::string base = webapi;
    while (!base.empty() && base.back() == '/') base.pop_back();
    net::Reply status = net::Get(base + "/status", 4000, 512);
    if (!status.status) {
        lines.push_back(Fmt(T("WebAPI {0}: not reachable ({1})"), webapi, status.error));
    } else {
        const std::string body = Replace(status.body, " ", "");
        lines.push_back(body.find("\"ok\":true") != std::string::npos
                            ? Fmt(T("WebAPI {0}: reachable"), webapi)
                            : Fmt(T("WebAPI {0}: answered, but it is not a shadNet server"), webapi));
    }
    const std::string typed = Trim(app.Str("online_webapi"));
    if (!typed.empty() && typed != webapi) lines.push_back(T("The WebAPI box holds the game server address; it is ignored."));
    if (!app.On("online")) lines.push_back(T("\"Play online\" is off: the game starts offline."));
    std::string text;
    for (auto& l : lines) text += (text.empty() ? "" : "\n") + l;
    return text;
}

void DownloadFsr4(const std::function<void(size_t, size_t)>& progress) {
    const fs::path folder = PortDir() / L"fsr4_shaders";
    const auto missing = Fsr4Missing();
    size_t done = 0;
    for (const std::string& name : missing) {
        if (progress) progress(done, missing.size());
        try {
            net::Download(std::string(FSR4_BASE) + "/" + name, folder / PathFrom(name));
        } catch (const std::exception& error) {
            throw Failure(Fmt(T("Download failed: {}", "Не удалось скачать: {}"), std::string(error.what())));
        }
        ++done;
    }
    if (progress) progress(done, missing.size());
}

}  // namespace actions
