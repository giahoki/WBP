// SPDX-License-Identifier: GPL-2.0-or-later
#include "mods.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <optional>
#include <random>
#include <set>

#include <nlohmann/json.hpp>

namespace prep {
namespace {

// Top-level folders of the game's dvdroot_ps4: a mod made of these is a dvdroot_ps4 itself.
const std::set<std::string> GAME_FOLDERS = {"action", "chr",   "event",  "facegen", "font",     "map",    "menu",
                                            "movie",  "msg",   "mtd",    "obj",     "other",    "param",  "paramdef",
                                            "parts",  "remo",  "script", "sfx",     "shader",   "sound"};

std::string Name(const fs::path& p) { return PathText(p.filename()); }

std::vector<fs::path> Entries(const fs::path& folder) {
    std::vector<fs::path> out;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(folder, ec)) out.push_back(e.path());
    std::sort(out.begin(), out.end());
    return out;
}

bool IsDir(const fs::path& p) {
    std::error_code ec;
    return fs::is_directory(p, ec);
}

DWORD Attributes(const fs::path& p) { return GetFileAttributesW(p.c_str()); }
bool IsReparse(const fs::path& p) {
    DWORD a = Attributes(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT);
}

// `name` inside `folder`, matching an existing entry case-insensitively.
fs::path Child(const fs::path& folder, const std::string& name) {
    for (const fs::path& entry : Entries(folder))
        if (IEquals(Name(entry), name)) return entry;
    return folder / PathFrom(name);
}

// (root, prefix): where a mod's files are and the game path they go to.
std::optional<std::pair<fs::path, std::string>> ContentRoot(const fs::path& folder) {
    if (!IsDir(folder)) return std::nullopt;
    for (const char* wrapper : {"", "app0", "CUSA03173"}) {
        fs::path base = *wrapper ? Child(folder, wrapper) : folder;
        if (IsDir(Child(base, "dvdroot_ps4"))) return std::pair{base, std::string()};
    }
    std::vector<fs::path> entries, folders;
    for (const fs::path& e : Entries(folder))
        if (!StartsWith(Name(e), ".")) entries.push_back(e);
    for (const fs::path& e : entries)
        if (IsDir(e)) folders.push_back(e);
    if (!folders.empty() && std::all_of(folders.begin(), folders.end(),
                                        [](auto& f) { return GAME_FOLDERS.count(Lower(Name(f))) > 0; }))
        return std::pair{folder, std::string("dvdroot_ps4")};
    if (folders.size() == 1) {
        bool other_files = std::any_of(entries.begin(), entries.end(), [](const fs::path& e) {
            std::error_code ec;
            if (!fs::is_regular_file(e, ec)) return false;
            std::string ext = Lower(PathText(e.extension()));
            return ext != ".txt" && ext != ".md" && ext != ".jpg" && ext != ".png" && ext != ".ini";
        });
        if (!other_files) return ContentRoot(folders[0]);
    }
    return std::nullopt;
}

struct ModFile {
    std::vector<std::string> parts;  // game path components
    fs::path source;
};

void Walk(const fs::path& directory, const fs::path& root, const std::string& prefix, std::vector<ModFile>& out) {
    std::vector<fs::path> files, folders;
    for (const fs::path& e : Entries(directory)) {
        if (IsReparse(e)) throw Failure("Mod symlinks are unsupported: " + PathText(e));
        (IsDir(e) ? folders : files).push_back(e);
    }
    for (const fs::path& source : files) {
        std::vector<std::string> parts;
        if (!prefix.empty()) parts.push_back(prefix);
        for (const auto& part : fs::relative(source, root)) parts.push_back(PathText(part));
        // Readme/metadata stay outside the mounted game. Executable patches use the patch compiler.
        if (Lower(parts[0]) != "dvdroot_ps4") {
            std::string first = Lower(parts[0]);
            if (first == "eboot.bin" || first == "sce_module" || first == "sce_sys")
                throw Failure("Executable/system replacement is unsupported: " + PathText(source));
            continue;
        }
        std::error_code ec;
        if (!fs::is_regular_file(source, ec)) throw Failure("Not a regular mod file: " + PathText(source));
        out.push_back({parts, source});
    }
    for (const fs::path& f : folders) Walk(f, root, prefix, out);
}

std::vector<ModFile> ModFiles(const fs::path& folder) {
    auto layout = ContentRoot(folder);
    if (!layout) throw Failure(PathText(folder) + ": expected dvdroot_ps4 (or chr/, parts/, ...) inside the mod folder");
    std::error_code ec;
    fs::path root = fs::canonical(layout->first, ec);
    if (ec) root = layout->first;
    std::vector<ModFile> out;
    Walk(root, root, layout->second, out);
    return out;
}

// Links: symbolic links when Windows allows them (Developer Mode), else junctions for folders
// and hard links (copies across volumes) for files. Those are remembered with their targets.
std::map<std::wstring, fs::path> linked;

bool MakeJunction(const fs::path& link, const fs::path& target) {
    if (!CreateDirectoryW(link.c_str(), nullptr)) return false;
    HANDLE h = CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        RemoveDirectoryW(link.c_str());
        return false;
    }
    std::wstring absolute = fs::absolute(target).native();
    std::wstring substitute = L"\\??\\" + absolute;
    const USHORT sub_bytes = USHORT(substitute.size() * 2), print_bytes = USHORT(absolute.size() * 2);
    std::vector<uint8_t> buffer(8 + 8 + sub_bytes + 2 + print_bytes + 2, 0);
    auto put16 = [&](size_t at, USHORT v) { std::memcpy(buffer.data() + at, &v, 2); };
    const DWORD tag = IO_REPARSE_TAG_MOUNT_POINT;
    std::memcpy(buffer.data(), &tag, 4);
    put16(4, USHORT(buffer.size() - 8));                // ReparseDataLength
    put16(8, 0);                                        // SubstituteNameOffset
    put16(10, sub_bytes);                               // SubstituteNameLength
    put16(12, USHORT(sub_bytes + 2));                   // PrintNameOffset
    put16(14, print_bytes);                             // PrintNameLength
    std::memcpy(buffer.data() + 16, substitute.data(), sub_bytes);
    std::memcpy(buffer.data() + 16 + sub_bytes + 2, absolute.data(), print_bytes);
    DWORD returned = 0;
    BOOL ok = DeviceIoControl(h, FSCTL_SET_REPARSE_POINT, buffer.data(), DWORD(buffer.size()), nullptr, 0, &returned, nullptr);
    CloseHandle(h);
    if (!ok) RemoveDirectoryW(link.c_str());
    return ok;
}

void MakeLink(const fs::path& link, const fs::path& target) {
    const bool dir = IsDir(target);
    DWORD flags = SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE | (dir ? SYMBOLIC_LINK_FLAG_DIRECTORY : 0);
    if (CreateSymbolicLinkW(link.c_str(), target.c_str(), flags)) return;
    bool ok;
    if (dir) {
        ok = MakeJunction(link, target);
    } else {
        ok = CreateHardLinkW(link.c_str(), target.c_str(), nullptr) ||
             CopyFileW(target.c_str(), link.c_str(), TRUE);
    }
    if (!ok) throw Failure("cannot link " + PathText(link) + " to " + PathText(target));
    linked[link.native()] = target;
}

bool IsLink(const fs::path& p) { return IsReparse(p) || linked.count(p.native()); }

fs::path LinkTarget(const fs::path& p) {
    auto it = linked.find(p.native());
    if (it != linked.end()) return it->second;
    return fs::canonical(p);
}

void RemoveLink(const fs::path& p) {
    DWORD a = Attributes(p);
    BOOL ok = (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) ? RemoveDirectoryW(p.c_str())
                                                                              : DeleteFileW(p.c_str());
    if (!ok) throw Failure("cannot remove " + PathText(p));
    linked.erase(p.native());
}

// Materialize one directory level; never write through a directory link.
void Expand(const fs::path& directory) {
    if (IsLink(directory)) {
        fs::path target = LinkTarget(directory);
        if (!IsDir(target)) throw Failure("File/directory conflict at " + Name(directory));
        RemoveLink(directory);
        fs::create_directory(directory);
        for (const fs::path& entry : Entries(target)) MakeLink(directory / entry.filename(), entry);
    } else {
        std::error_code ec;
        if (fs::exists(directory, ec) && !IsDir(directory)) throw Failure("File/directory conflict at " + Name(directory));
        fs::create_directory(directory, ec);
    }
}

bool SymlinksWork(const fs::path& directory) {
    fs::path probe = directory / L"link-test.tmp";
    RemoveDirectoryW(probe.c_str());
    if (!CreateSymbolicLinkW(probe.c_str(), directory.c_str(),
                             SYMBOLIC_LINK_FLAG_DIRECTORY | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE))
        return false;
    RemoveDirectoryW(probe.c_str());
    return true;
}

fs::path TempDir(const fs::path& parent, const wchar_t* prefix) {
    std::random_device rd;
    for (int i = 0; i < 100; ++i) {
        wchar_t name[64];
        swprintf(name, 64, L"%ls%08x", prefix, unsigned(rd()));
        fs::path p = parent / name;
        if (CreateDirectoryW(p.c_str(), nullptr)) return p;
    }
    throw Failure("cannot create a folder in " + PathText(parent));
}

std::vector<std::string> Selected(const fs::path& root, const fs::path& config) {
    std::vector<std::string> available = DiscoverMods(root);
    auto text = config.empty() ? std::nullopt : ReadText(config);
    if (!text) return available;
    nlohmann::json settings;
    try {
        settings = nlohmann::json::parse(*text);
    } catch (...) {
        throw Failure("mods.json is not valid JSON");
    }
    std::set<std::string> disabled;
    std::vector<std::string> order;
    for (auto& v : settings.value("disabled", nlohmann::json::array())) {
        if (!v.is_string()) throw Failure("Disabled mods must be a list of folder names");
        disabled.insert(v.get<std::string>());
    }
    for (auto& v : settings.value("order", nlohmann::json::array())) {
        if (!v.is_string()) throw Failure("Mod order must be a list of folder names");
        order.push_back(v.get<std::string>());
    }
    // New folders are enabled automatically and appended in alphabetical order.
    std::vector<std::string> result;
    std::set<std::string> seen;
    auto add = [&](const std::string& n) {
        if (std::find(available.begin(), available.end(), n) != available.end() && !disabled.count(n) && seen.insert(n).second)
            result.push_back(n);
    };
    for (auto& n : order) add(n);
    for (auto& n : available) add(n);
    return result;
}

bool Under(const fs::path& path, const fs::path& folder) {
    auto rel = path.lexically_relative(folder);
    return !rel.empty() && rel.native()[0] != L'.';
}

}  // namespace

std::vector<std::string> DiscoverMods(const fs::path& root) {
    std::vector<std::string> names;
    if (!IsDir(root)) return names;
    for (const fs::path& p : Entries(root))
        if (IsDir(p) && ContentRoot(p)) names.push_back(Name(p));
    std::sort(names.begin(), names.end(), [](const std::string& a, const std::string& b) {
        std::string la = Lower(a), lb = Lower(b);
        return la != lb ? la < lb : a < b;
    });
    return names;
}

fs::path BuildOverlay(const ModOptions& o) {
    std::vector<std::pair<std::string, fs::path>> layers;
    for (const fs::path& extra : o.extra)
        if (IsDir(extra)) layers.push_back({Name(extra), extra});
    if (o.enabled) {
        // shadPS4's loose overlay convention, beside the original game.
        fs::path legacy = o.game;
        legacy += L"-mods";
        if (IsDir(legacy)) layers.push_back({Name(legacy), legacy});
        if (IsDir(Child(o.mods_dir, "dvdroot_ps4"))) layers.push_back({Name(o.mods_dir), o.mods_dir});
        else
            for (const std::string& name : Selected(o.mods_dir, o.config)) layers.push_back({name, o.mods_dir / PathFrom(name)});
    }
    const fs::path game = fs::canonical(o.game);
    std::vector<ModFile> replacements;
    std::map<std::string, std::string> owners;
    for (auto& [name, root] : layers) {
        size_t count = 0;
        for (ModFile& f : ModFiles(root)) {
            std::string key;
            for (auto& p : f.parts) key += Lower(p) + "/";
            auto it = owners.find(key);
            if (it != owners.end()) SayError("Mods: " + key + ": " + it->second + " -> " + name);
            owners[key] = name;
            replacements.push_back(std::move(f));
            ++count;
        }
        SayError("Mods: " + name + ": " + std::to_string(count) + " files");
    }
    if (replacements.empty()) return game;
    fs::path out = fs::absolute(o.out);
    fs::create_directories(out);
    if (!SymlinksWork(out) && !IEquals(PathText(out.root_name()), PathText(game.root_name())))
        out = game.parent_path();  // hard links only reach files on the same volume
    const fs::path overlay = TempDir(out, L"mod-game-");
    try {
        for (const fs::path& entry : Entries(game)) MakeLink(overlay / entry.filename(), entry);
        size_t replaced = 0, added = 0;
        for (const ModFile& f : replacements) {
            fs::path parent = overlay;
            for (size_t i = 0; i + 1 < f.parts.size(); ++i) {
                parent = Child(parent, f.parts[i]);
                Expand(parent);
            }
            fs::path destination = Child(parent, f.parts.back());
            std::error_code ec;
            if (IsLink(destination)) {
                replaced += Under(LinkTarget(destination), game);
                RemoveLink(destination);
            } else if (fs::exists(destination, ec)) {
                throw Failure("File/directory conflict: " + PathText(destination));
            } else {
                ++added;
            }
            MakeLink(destination, f.source);
        }
        SayError("Mods: " + std::to_string(replaced) + " game files replaced, " + std::to_string(added) + " added");
        return overlay;
    } catch (...) {
        RemoveOverlay(overlay);
        throw;
    }
}

void RemoveOverlay(const fs::path& overlay) {
    // Links (symbolic, junctions) are removed themselves, never entered; hard links and copies
    // are only names in the overlay.
    for (const fs::path& entry : Entries(overlay)) {
        DWORD a = Attributes(entry);
        if (a == INVALID_FILE_ATTRIBUTES) continue;
        if (a & FILE_ATTRIBUTE_REPARSE_POINT) {
            (a & FILE_ATTRIBUTE_DIRECTORY) ? RemoveDirectoryW(entry.c_str()) : DeleteFileW(entry.c_str());
        } else if (a & FILE_ATTRIBUTE_DIRECTORY) {
            RemoveOverlay(entry);
        } else {
            DeleteFileW(entry.c_str());  // never change attributes: a hard link shares the game file's
        }
        linked.erase(entry.native());
    }
    RemoveDirectoryW(overlay.c_str());
}

}  // namespace prep
