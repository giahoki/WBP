// wbp: optimizations that can be switched off while the game runs (BB_TOGGLE_FILE,
// see runtime_memory.c), to find which one changes rendering without restarting.
#pragma once
#include <atomic>
#include <csetjmp>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#ifdef _WIN32
// wbp (Windows): no signals. The loader's exception handler resumes the faulting thread
// in bb_longjmp (src/compat_win.c: no unwinding through the faulting frames).
typedef unsigned long long sigjmp_buf[32];
extern "C" __attribute__((returns_twice)) int bb_setjmp(sigjmp_buf buffer);
#define sigsetjmp(buffer, save) bb_setjmp(buffer)
#endif

extern "C" std::uint64_t runtime_disabled_optimizations;
/// Recovery point for speculative guest memory reads on this thread (runtime_memory.c).
extern "C" __thread sigjmp_buf* runtime_fault_recover;

namespace BbToggle {
enum : std::uint64_t {
    RegionCache = 1,
    FetchShaderCache = 2,
    PageTrackingEarlyExit = 4,
    PendingPollLimit = 8,
    ThreadedRecording = 16,
    ImageDescCache = 32,
    LockFreeUploadCheck = 64,
    FindImageCache = 128,
    DeferredUploads = 256,
    AccessMemo = 512,
    TextureBindingMemo = 1024,
    CoarseReadTracking = 2048,
    PreparedResources = 4096,
    DrawPreparation = 8192,
    DeferredStreamCopies = 16384,
    HotPages = 32768,
    FaultWindow = 65536,
    ParallelCopies = 131072,
    AsyncFences = 262144,
    PoolSmallCopies = 524288,
    RecordPrefetch = 1ull << 32,
    TextureViewMemo = 1ull << 33,
    TextureBindHelper = 1ull << 34,
    EarlyDrawInputs = 1ull << 35,
    ConstantRing = 1ull << 36,
    DrawPipeline = 1ull << 37,
    PipelinedTasks = 1ull << 38,
    PendingFenceWaits = 1ull << 39,
    PipelinedDispatch = 1ull << 40,
    RecorderFences = 1ull << 41,
    PipelinedMemoryWrites = 1ull << 42,
    MultiCopyShader = 1ull << 43,
    RenderStateMemo = 1ull << 44,
    TextureSetMemo = 1ull << 45,
    PipelinedIndirectDraws = 1ull << 46,
    SceneAttachmentsOnly = 1ull << 47,
    SampleSceneProxies = 1ull << 48,
    OrderedGuestWrites = 1ull << 49,
    SceneHalfRes = 1ull << 50, ///< live scaling also reduces the 960x540 post targets
    UpdateImageFastPath = 1u << 30,
    // TAA A/B in one run: optional techniques, off by default (no measured gain, 2026-10-02).
    TaaTonemapBlend = 1ull << 51,
    TaaClip = 1ull << 52,
    TaaVariance = 1ull << 53,
    TaaFilter = 1ull << 54,
    // On by default (bit set: off): history of a thin feature this jitter phase missed is kept
    // when nothing moves. Static-camera flicker of railings/window bars p99.9 -45% (2026-10-03).
    TaaKeepNearerHistory = 1ull << 55,
    SceneMipBias = 1ull << 57, ///< negative LOD bias of G-buffer samplers at reduced scene sizes
    BufferEviction = 1ull << 58, ///< arena blocks unused for seconds are released under memory pressure
    HleCpuCopy = 1ull << 59, ///< copy shader HLE between CPU-side guest ranges done by the CPU
    // Bits 20-29 are used as raw debug toggles by the camera/object motion and the upscaler.
};
inline bool Disabled(std::uint64_t bit) {
    return (__atomic_load_n(&runtime_disabled_optimizations, __ATOMIC_RELAXED) & bit) != 0;
}
} // namespace BbToggle

namespace BbStats {
/// Guest writes caught by page protection, and pages currently left unprotected as hot.
inline std::atomic<std::uint64_t> tracker_faults{0};
inline std::atomic<std::int64_t> hot_pages{0};
/// Stall diagnostics (BB_FRAME_STATS): per-frame deltas printed for frames over 40 ms.
inline std::atomic<std::uint64_t> images_registered{0};
inline std::atomic<std::uint64_t> image_upload_bytes{0};
inline std::atomic<std::uint64_t> buffer_upload_bytes{0};
inline std::atomic<int> gpu_thread_clock{-1}; ///< clockid_t of the GPU command thread
inline std::atomic<std::uint64_t> draws{0}, dispatches{0}, submissions{0};
/// Frames the GPU command thread has started (display pass), for per-frame diagnostics.
inline std::atomic<std::uint64_t> gpu_frames{0};
/// Wall time spent in operations suspected of stalls (ns, all threads).
inline std::atomic<std::uint64_t> t_resident{0}, t_protect{0}, t_image_create{0}, t_refresh{0},
    t_staging{0}, t_host_wait{0}, t_copy{0}, copy_bytes{0}, t_read_faults{0}, read_faults{0},
    t_write_faults{0}, t_copy_cpu{0}, copy_sys_us{0}, copy_minflt{0};
/// Diagnostics are collected only with BB_FRAME_STATS=1.
inline const bool enabled = [] {
    const char* env = std::getenv("BB_FRAME_STATS");
    return env && env[0] == '1';
}();
struct Timer {
    std::atomic<std::uint64_t>& total;
    std::chrono::steady_clock::time_point start =
        enabled ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    ~Timer() {
        if (!enabled) {
            return;
        }
        total.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - start)
                            .count(),
                        std::memory_order_relaxed);
    }
};
/// Wall time the GPU command thread waited for guest submissions (ns).
inline std::atomic<std::uint64_t> gpu_idle_ns{0};
/// Draws recorded into the reduced scene targets, and draws after the scene started.
inline std::atomic<std::uint64_t> reduced_draws{0}, scene_draws{0};
/// Wall time spent blocked in the scheduler (ns): waiting for the recording thread to drain,
/// for host copies before a submission or fence, and for GPU ticks.
inline std::atomic<std::uint64_t> sync_recording_ns{0}, host_copies_wait_ns{0}, tick_wait_ns{0},
    copy_threads_wait_ns{0}, host_copy_waits{0};
struct WaitTimer {
    std::atomic<std::uint64_t>& total;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    ~WaitTimer() {
        total.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - start).count(),
                        std::memory_order_relaxed);
    }
};
/// GPU thread rusage, refreshed after each graphics submission.
inline std::atomic<std::uint64_t> gpu_sys_us{0}, gpu_user_us{0}, gpu_invol_switches{0},
    gpu_vol_switches{0}, gpu_minor_faults{0};
/// Protection faults (signals) taken by the GPU thread itself.
inline std::atomic<std::uint64_t> gpu_signal_faults{0};
/// Protection changes: calls and pages, those removing write access (TLB shootdowns) apart.
inline std::atomic<std::uint64_t> protect_calls{0}, protect_pages{0}, protect_revoke_calls{0},
    protect_revoke_pages{0};
/// Recorded render passes and barrier batches (memory part, image barriers) for BB_BENCH.
inline std::atomic<std::uint64_t> render_passes{0}, barrier_batches{0}, memory_barriers{0},
    image_barriers{0};
/// Image accesses that skipped a barrier: a read after reads in the same layout.
inline std::atomic<std::uint64_t> image_barriers_merged{0};
/// Copy shader HLE batches done by the CPU / by the GPU, and the bytes the CPU copied.
inline std::atomic<std::uint64_t> hle_cpu_batches{0}, hle_gpu_batches{0}, hle_cpu_bytes{0};
/// Barrier batches with no write before them (layout changes between reads only).
inline std::atomic<std::uint64_t> layout_only_batches{0};
/// GPU tick waits per call site (Scheduler::Wait): file and line of the caller, wall time.
struct TickWaitSite {
    std::atomic<const char*> file{nullptr};
    std::atomic<std::uint32_t> line{0};
    std::atomic<std::uint64_t> ns{0}, count{0};
};
inline TickWaitSite tick_wait_sites[64];
inline void NoteTickWait(const char* file, std::uint32_t line, std::uint64_t ns) {
    for (auto& site : tick_wait_sites) {
        const char* known = site.file.load(std::memory_order_acquire);
        if (!known) {
            const char* expected = nullptr;
            if (site.file.compare_exchange_strong(expected, file)) {
                site.line.store(line, std::memory_order_release);
                known = file;
            } else {
                known = expected;
            }
        }
        if (known == file && site.line.load(std::memory_order_acquire) == line) {
            site.ns.fetch_add(ns, std::memory_order_relaxed);
            site.count.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
}
/// Barrier batches per call site of Runtime::FlushBarriers (same table layout as tick waits).
inline TickWaitSite barrier_sites[64];
inline void NoteBarrierSite(const char* file, std::uint32_t line) {
    for (auto& site : barrier_sites) {
        const char* known = site.file.load(std::memory_order_acquire);
        if (!known) {
            const char* expected = nullptr;
            if (site.file.compare_exchange_strong(expected, file)) {
                site.line.store(line, std::memory_order_release);
                known = file;
            } else {
                known = expected;
            }
        }
        if (known == file && site.line.load(std::memory_order_acquire) == line) {
            site.count.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
}
/// Dispatches from the guest's asynchronous compute queues (ACB), for BB_BENCH.
inline std::atomic<std::uint64_t> asc_dispatches{0};
/// Images presented to the swapchain (game frames plus generated ones), for BB_BENCH.
inline std::atomic<std::uint64_t> presents{0};
/// Why a draw needed a barrier (BB_BENCH): 0 vertex buffers, 1 index buffer, 2 GDS, 3 shader
/// buffers, 4 colour targets, 5 depth target, 6 textures (helper).
inline std::atomic<std::uint64_t> barrier_reasons[8]{};
inline bool BarrierReason(int reason, bool hit) {
    if (hit) {
        barrier_reasons[reason].fetch_add(1, std::memory_order_relaxed);
    }
    return hit;
}
} // namespace BbStats
