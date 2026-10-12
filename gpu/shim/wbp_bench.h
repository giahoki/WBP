// SPDX-License-Identifier: GPL-2.0-or-later
// wbp: BB_BENCH=1, a repeatable performance run. The pad presses Cross until the game is in
// a scene (title, Continue, loading), the player then stands still where the save left them;
// after a warm-up the frame times are measured, one result line is appended to
// BB_BENCH_OUT (default wbp_bench.txt) and the game exits.
//   BB_BENCH_WARMUP=s (15), BB_BENCH_SECONDS=s (30), BB_BENCH_LABEL=text (the line's name).

#pragma once

namespace BbBench {
/// Every guest flip (VideoOut).
void OnFlip();
/// Every image handed to the swapchain (present thread): spacing of the shown frames.
void OnPresent();
} // namespace BbBench

extern "C" {
/// 1 while the bench wants Cross pressed now (runtime_pad.c).
int bbgpu_bench_press(void);
}
