# Own renderer: feasibility notes (2026-10-12)

Question: render Bloodborne through our own Vulkan code at the engine level instead of translating
the PS4 GPU command stream (PM4) in the shadPS4 renderer. Addresses are PS4 addresses (IDA,
base `0x400000`) for game version 1.09.

## What the game has

- **GraphicsFramework v0.147.3** (string at `0x4D3AC61`, sources `N:\GS\Source\GraphicsFramework\…`):
  FromSoftware's GX layer, the same framework the PC releases (Dark Souls 3, Sekiro, Elden Ring) put
  over D3D. Threads: `GXRenderThread`, `GXWorker:1..5`. Class names and RTTI are stripped.
- **Gnm is linked statically** into the eboot; its `DrawCommandBuffer` methods sit around
  `0x1472000-0x1490000` and write PM4 directly, e.g. `sub_14741B0` = drawIndexAuto
  (`0xC0012D00` header). The command buffer object: `+8` end, `+16` write cursor, `+24` out-of-space
  callback, `+40` predication/shader-type bits.
- **GX draw context** calls those Gnm methods itself. Example `sub_2AB55A0` (a full-screen pass):
  `this+16` is the Gnm command buffer, `this+46424` render-target state (`sub_2AD1E10` binds slot
  0..7, `sub_2AD1E30` unbinds), `this+80` shader binding (`sub_2ACE1E0`), `drawIndexAuto(3)`,
  dirty masks at `this+46200/46232`. So GX keeps high-level state (targets, shaders, passes) right
  above Gnm.
- **Shaders** come from game files (`shader:/GXShader.shaderbnd`, `shader:/GXPostEffect.shaderbnd`):
  a fixed set of PS4 GCN binaries. They can be translated once, ahead of time, but they have to be
  translated whichever level we intercept at: there is no source.

## Where the time actually goes (measured on an RTX 4060 Ti, BB_BENCH)

- CPU threads are about 20% busy; the only GPU wait is the frames-ahead throttle
  (`vk_presenter.cpp:490`). The GPU needs ~15 ms per frame for ~885 draws and ~140 passes, about
  5-10x what the game's work should cost on this card.
- The GPU time is lost in how the work is submitted: wide barriers (the memory barrier waits
  `AllCommands`), many small render passes, image round trips (depth tiled to a buffer and uploaded
  back as R32F every frame, `BufferCache::SynchronizeMemoryFromImage` → `TileManager::TileImage`),
  60 HLE copy dispatches with barriers, FSR at 1.8 ms.

## Conclusion

Intercepting at GX or Gnm level removes PM4 parsing, which is **CPU** cost, and the CPU is not the
bottleneck. The GPU waste comes from the Vulkan backend's conservative handling of resources and
synchronisation, and that has to be redone at any interception level. The cheapest route to the
same result is a **Bloodborne-aware backend** inside the existing renderer:

1. Per-pass knowledge of the frame (shadow cascades, G-buffer, lighting, fog, post): recognise
   passes by render targets and shader hashes, which `BB_GPU_PROFILE` already prints.
2. Barriers scoped to what the next pass reads (image barriers per target, no `AllCommands`
   memory barrier between passes that do not touch buffers).
3. Keep depth readable directly as R32F (view or GPU copy) instead of the tile/untile round trip.
4. Ahead-of-time translation of every shader in the `.shaderbnd` files (no compile stutter).

Engine-level interception (hooking the GX context methods above) stays an option for later:
it matters only once the CPU becomes the limit, for example for frame rates well above 120.

Measure every step with `C:\bb-re\bench\bench.ps1` (BB_BENCH, ~1.5% noise).

## Bench results (2026-10-12, RTX 4060 Ti, 1080p output, user's save at a lamp)

| Run | FPS |
|---|---|
| Native AA + FSR 3.1, model LOD -2, object motion (user's settings) | 64-66 |
| no buffer memory barriers (one-off experiment, removed) | 71.3 |
| object motion off | 70.8 |
| model LOD 0 (as in the game) | 71.1 |
| TAA instead of FSR 3.1 | 75.4 |
| shadow_quality=low | 66.4 (no gain) |
| Quality preset, live resolution on | 84.6 |
| Quality preset, live resolution off | 77.3 |
| Performance preset, live resolution off | 94.2 |
| Quality + LOD 0 + object motion off | 87.4 |

Per frame: ~890 draws, 189 render passes, 237 barrier batches (170 with the memory barrier, 259
image barriers). Even at a quarter of the pixels the GPU needs ~10.6 ms: a fixed cost that does not
follow resolution, shadows or LOD. Main suspect: the barrier batches (each image layout transition
drains the GPU), so next is fewer layout changes (GENERAL layout where the driver allows it) and
narrower stage masks. `BB_UPSCALER=off` crashes (DL_PANIC); a start can fail at random with
`_AllocateDirectMemory() failed for creating runtime heap` (seen twice in ~15 starts).

Tried and dropped (2026-10-12):
- Read-after-read image accesses in the same layout no longer get a barrier (`Image::GetBarriers`,
  `MergeRead`; `BB_OLD_IMAGE_BARRIERS=1` restores the old rule): ~6600 merges a frame, but only
  67.5 vs 66.9 FPS. Kept, it is correct and cheap.
- Early submission every 200 draws: no gain at native, slower at Performance (86 vs 94). The
  "between submissions: GPU idle" profile entry is mostly the presenter's own work on its
  separate schedulers, not idle time. Reverted.
- Next needs a real GPU profiler (Nsight Graphics GPU Trace) to see per-draw shader cost:
  the fixed ~9 ms at a quarter of the pixels points at geometry/vertex processing or
  translated-shader efficiency, which pass-level timestamps cannot separate.

## Nsight Graphics GPU Trace (2026-10-12)

Capture: `C:\bb-re\bench\ngfx_wrap.cmd` as `BB_GAME_EXE` (ngfx GPU Trace Profiler, start after
55 s, 2 frames, `--auto-export` to `C:\bb-re\bench\trace\BASE\GPUTRACE_FRAME.xls`). The driver must
allow performance counters for the user (NVIDIA Control Panel, Developer).

| Metric (per frame) | Object motion on | Object motion off |
|---|---|---|
| graphics engine active | 93-95% | |
| SM throughput | 18-21% | 27-28% |
| front end stalled on wait-for-idle (`fe__cycles_stalled_cmd_wfi`) | **64-75%** | 57% |
| `go_idle` commands | **886-1405** | 588 |
| 3D/compute subchannel switches | 225 | 215 |
| draws | 927-974 | 993-999 |

The GPU is not short of throughput: it drains completely about once per draw. The idles add up
from barrier batches (~232/frame), render pass changes (~187/frame) and 3D/compute switches
(~215/frame, every switch idles on NVIDIA); object motion adds several hundred more without
changing the barrier or pass counts (its G-buffer variants store positions from the vertex
stage). Fewer idles is the only large lever left: async compute for the game's compute work,
barriers batched per pass instead of per hazard, and render passes kept across compatible draws.

Barrier sources (BB_BENCH prints them; 229 batches a frame): draws 124.5 (`Rasterizer::Draw`),
buffer copies 31.5 (`Runtime::CopyBuffer`), HLE copy shader 28.7, dispatches 19.4, the rest ~25.
Draw barriers by reason, per frame: colour targets 83, depth target 29, vertex buffers 18, shader
buffers 10, GDS 3; exact (unrounded) read tracking changes nothing. So nearly all of them are real
render-target-to-texture dependencies between the game's ~190 passes: on the PS4 they are cheap,
on an NVIDIA GPU each one drains the whole chip. Removing them means overlapping independent work
(async compute queue, reordering independent passes), not trimming false hazards.

Async compute: not applicable. Bloodborne maps no asynchronous compute queue; all ~102
dispatches a frame come through the graphics ring in order with the draws (BB_BENCH counts them).
With the barriers being real pass dependencies too, the frame is serial by construction; what is
left is reordering or merging the game's own passes, i.e. a game-specific render graph.
