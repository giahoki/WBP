// SPDX-License-Identifier: GPL-2.0-or-later
// What the launcher's buttons do outside the game: folders, dialogs, the desktop shortcut,
// Steam, updates, the GPU check, the online test and the FSR 4 download.
#pragma once
#include <functional>
#include <optional>
#include <string>

#include "settings.h"

namespace actions {

void Open(const fs::path& path);           // Explorer, or the file's program
void OpenUrl(const std::string& url);      // http(s) only
std::optional<fs::path> BrowseFolder(void* owner, const std::string& title, const fs::path& start);
bool Confirm(void* owner, const std::string& text);
void Message(void* owner, const std::string& text, bool error);

// Each returns a message for the player; Failure carries the error text.
std::string DesktopShortcut();
std::string AddToSteam(void* owner);

// The newest release on GitHub: version ("1.0"), zip URL, page URL.
struct Release {
    std::string version, zip, page;
};
Release LatestRelease();  // throws Failure
bool Newer(const std::string& version, const std::string& than);
// Downloads and starts the installer role of the new version; the caller closes the launcher.
void InstallUpdate(const Release& release, const std::function<void(int)>& percent);
// --install-update TARGET PID, run by the downloaded version from its temporary folder.
int FinishUpdate(const fs::path& target, unsigned long wait_pid);
void CleanUpdateLeftovers();

// "✓ Graphics card: NAME", "✗ ..." or "• ..." (bb-gpu-capabilities.exe).
std::string DetectGpu();
std::string TestConnection(const App& app);
void DownloadFsr4(const std::function<void(size_t, size_t)>& progress);  // throws Failure

}  // namespace actions
