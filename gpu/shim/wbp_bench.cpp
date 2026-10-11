// SPDX-License-Identifier: GPL-2.0-or-later
#include "wbp_bench.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "wbp_toggles.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace BbBench {
namespace {

using Clock = std::chrono::steady_clock;

enum class Phase { Off, Menu, Warmup, Measure, Done };

const bool enabled = [] {
    const char* env = std::getenv("BB_BENCH");
    return env && env[0] == '1';
}();

double EnvSeconds(const char* name, double fallback) {
    const char* env = std::getenv(name);
    const double v = env ? std::atof(env) : 0.0;
    return v > 0 ? v : fallback;
}

std::atomic<Phase> phase{enabled ? Phase::Menu : Phase::Off};
Clock::time_point phase_start, last_flip;
std::uint64_t last_draws = 0, measure_draws = 0, measure_tick_wait = 0;
std::uint64_t measure_passes = 0, measure_batches = 0, measure_memory = 0, measure_images = 0;
std::uint32_t scene_flips = 0;
std::vector<double> intervals;

/// Draws per flip above this mean the game shows a scene (title and loading stay far below).
constexpr std::uint64_t SceneDraws = 300;
/// Consecutive flips over SceneDraws before the press stops.
constexpr std::uint32_t SceneFlips = 30;

void Finish() {
    std::vector<double> sorted = intervals;
    std::sort(sorted.begin(), sorted.end());
    double sum = 0;
    for (const double ms : intervals) {
        sum += ms;
    }
    const size_t n = sorted.size();
    const double seconds = sum / 1000.0;
    const double fps = seconds > 0 ? n / seconds : 0;
    const double median = n ? sorted[n / 2] : 0;
    const double p99 = n ? sorted[std::min(n - 1, n * 99 / 100)] : 0;
    const double worst = n ? sorted.back() : 0;
    const double draws = n ? double(BbStats::draws.load() - measure_draws) / n : 0;
    const double gpu_wait =
        seconds > 0 ? (BbStats::tick_wait_ns.load() - measure_tick_wait) / (seconds * 1e7) : 0;
    const char* label = std::getenv("BB_BENCH_LABEL");
    char line[512];
    std::snprintf(line, sizeof(line),
                  "%s: %.1f FPS, median %.2f ms, p99 %.2f ms, worst %.1f ms, %.0f draws/frame, "
                  "GPU thread waited for the GPU %.1f%% (%zu frames)\n",
                  label && label[0] ? label : "bench", fps, median, p99, worst, draws, gpu_wait, n);
    std::printf("Bench: %s", line);
    if (n) {
        std::printf("Bench:   per frame: %.0f render passes, %.0f barrier batches (%.0f with a "
                    "memory barrier, %.0f image barriers)\n",
                    double(BbStats::render_passes.load() - measure_passes) / n,
                    double(BbStats::barrier_batches.load() - measure_batches) / n,
                    double(BbStats::memory_barriers.load() - measure_memory) / n,
                    double(BbStats::image_barriers.load() - measure_images) / n);
    }
    // Where the GPU command thread (and others) waited for the GPU, most first.
    std::vector<const BbStats::TickWaitSite*> sites;
    for (const auto& site : BbStats::tick_wait_sites) {
        if (site.file.load() && site.count.load()) {
            sites.push_back(&site);
        }
    }
    std::sort(sites.begin(), sites.end(),
              [](const auto* a, const auto* b) { return a->ns.load() > b->ns.load(); });
    for (const auto* site : sites) {
        std::string file = site->file.load();
        file = file.substr(file.find_last_of("/\\") + 1);
        std::printf("Bench:   GPU wait %5.1f%% %7.1f/s  %s:%u\n",
                    seconds > 0 ? site->ns.load() / (seconds * 1e7) : 0.0,
                    seconds > 0 ? site->count.load() / seconds : 0.0, file.c_str(),
                    site->line.load());
    }
    const char* out = std::getenv("BB_BENCH_OUT");
    if (FILE* f = std::fopen(out && out[0] ? out : "wbp_bench.txt", "a")) {
        std::fputs(line, f);
        std::fclose(f);
    }
    std::fflush(stdout);
#ifdef _WIN32
    TerminateProcess(GetCurrentProcess(), 0);
#else
    std::_Exit(0);
#endif
}

} // namespace

void OnFlip() {
    const Phase p = phase.load(std::memory_order_relaxed);
    if (p == Phase::Off || p == Phase::Done) {
        return;
    }
    const auto now = Clock::now();
    const std::uint64_t draws = BbStats::draws.load(std::memory_order_relaxed);
    const std::uint64_t flip_draws = draws - last_draws;
    last_draws = draws;
    const double seconds = std::chrono::duration<double>(now - phase_start).count();
    switch (p) {
    case Phase::Menu:
        scene_flips = flip_draws > SceneDraws ? scene_flips + 1 : 0;
        if (scene_flips >= SceneFlips) {
            std::printf("Bench: in a scene; warming up\n");
            phase_start = now;
            phase = Phase::Warmup;
        }
        break;
    case Phase::Warmup:
        if (seconds >= EnvSeconds("BB_BENCH_WARMUP", 15)) {
            std::printf("Bench: measuring\n");
            phase_start = now;
            measure_draws = draws;
            measure_tick_wait = BbStats::tick_wait_ns.load();
            measure_passes = BbStats::render_passes.load();
            measure_batches = BbStats::barrier_batches.load();
            measure_memory = BbStats::memory_barriers.load();
            measure_images = BbStats::image_barriers.load();
            for (auto& site : BbStats::tick_wait_sites) {
                site.ns = 0;
                site.count = 0;
            }
            intervals.clear();
            phase = Phase::Measure;
        }
        break;
    case Phase::Measure:
        intervals.push_back(std::chrono::duration<double, std::milli>(now - last_flip).count());
        if (seconds >= EnvSeconds("BB_BENCH_SECONDS", 30)) {
            phase = Phase::Done;
            Finish();
        }
        break;
    default:
        break;
    }
    last_flip = now;
}

} // namespace BbBench

extern "C" int bbgpu_bench_press(void) {
    if (BbBench::phase.load(std::memory_order_relaxed) != BbBench::Phase::Menu) {
        return 0;
    }
    // A 120 ms press every 1.5 s: the title, Continue and dialogs each take one.
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        BbBench::Clock::now().time_since_epoch())
                        .count();
    return ms % 1500 < 120;
}
