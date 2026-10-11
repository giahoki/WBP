// SPDX-License-Identifier: GPL-2.0-or-later
// Starting the game: the successor of run.py (`Bloodborne.exe --run`), the launcher's game
// process with its log, and `--play` (the game with the saved settings, no window).
#pragma once
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "settings.h"

// --run [--after PID] [probe arguments]: prepares the game and runs wbp-game.exe. Exit code.
int RunGame(std::vector<std::wstring> args);

// --play: the game with the launcher's saved settings; output to the console and last_run.log.
int PlayWithoutWindow(const App& app);

// The game started from the launcher window: Bloodborne.exe --run in a Windows job (which also
// holds the launches of the in-game restart), its output collected for the Log page.
class GameProcess {
public:
    ~GameProcess();
    // Throws Failure when the process cannot start.
    void Start(const App& app);
    bool Running() const { return running_; }
    void Stop();
    std::string TakeLog();         // new output since the last call
    std::string Status() const;    // "running", "restarting" or ""
    int ExitCode() const { return exit_code_; }

private:
    void Read(void* pipe, fs::path log_path);
    void* job_ = nullptr;
    void* process_ = nullptr;
    std::thread reader_;
    std::atomic<bool> running_{false};
    std::atomic<int> exit_code_{0};
    mutable std::mutex lock_;
    std::string pending_, status_;
};
