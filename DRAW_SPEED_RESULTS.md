# Colour-screen optimization pass: 0.5.4 (2026-11-13)

**Target remains open.** This pass preserves the image and improves drawing and transfer overlap,
but does not achieve the requested 12–15 fps / ~70 ms camera frame under the tested conditions.
The 150 ms alert cap stays unchanged; 400 ms idle cap and saved 10 MHz SPI setting stay unchanged.

## Profile before optimization

Instrumented 0.5.3 renderer, drawing only in loop(), microseconds per camera frame:

| Part | us |
|---|---:|
| Grid / rotation setup | 6547 |
| Trail | 5488 |
| Nearby camera markers | 6886 |
| Target | 5266 |
| Chevron / glow | 3323 |
| Card / text / status bar | 71372 |

Total ~104969 us. This split was reported before optimization. The warning card/text/bar group
was the dominant cost. Profiling remains available in `tft bench`; ordinary rendering leaves
the profiling clock disabled. Part totals exclude a little fill/dispatch overhead.

## Before / after drawing-only bench

Actual 0.5.3 binary was temporarily restored for the baseline, then the exact final 0.5.4 build
was installed. Same demo models, 3 frames per screen in loop(), with passive capture enabled.

| Screen | 0.5.3 draw ms | 0.5.4 warm draw ms | 0.5.4 slowest warm strip ms |
|---|---:|---:|---:|
| Boot | 10.685 | 8.581 | 0.871 |
| Listening | 42.351 | 39.278 | 3.349 |
| Camera | 105.332 | 63.516 | 5.985 |
| Camera close | 102.458 | 60.814 | 5.698 |
| RF | 33.828 | 27.711 | 2.624 |
| Follower | 44.579 | 36.488 | 4.169 |

Cold first bench: camera averaged 73.760 ms over three frames, including the one-time warning
card coverage build; its slowest strip was 30.183 ms. Camera-close then averaged 62.015 ms.
These are wall-clock bench observations, not a deterministic frame budget or a controlled RF load.

Warm camera split (us): grid 4893, trail 3716, markers 6107, target 5151, chevron 3242,
card/text/bar 34245 (status 8535, rounded card 6291, text 19418). Remaining work is primarily
font lookup/decoding/text layout, map rasterization, and scheduling/stalls during real rendering.

## Real frames, including drawing and SPI

Camera demo acknowledged before sampling. 0.5.3 maxima isolated by rebooting at each saved
clock; 0.5.4 timing maxima reset after each panel/clock reinitialization. Startup listening
frames can be included in maxima; typical values below are confirmed camera frames.

| Build / requested clock | Sampled last-frame times | Maximum observed |
|---|---|---:|
| 0.5.3 / 10 MHz | 301, 298 ms | 319 ms |
| 0.5.3 / 40 MHz | 230, 229 ms | 234 ms |
| 0.5.4 / 10 MHz | 141, 137 ms | 342 ms |
| 0.5.4 / 40 MHz | 112, 123 ms | 378 ms |

Typical frames are faster; tail latency is NOT improved reliably. Research logging also showed
hundreds-of-ms filesystem operations during this run, but the present counters cannot prove
which individual frame stall they caused. Higher-priority radio tasks can preempt drawing.
There is no evidence here that reducing the frame cap to 70 ms would produce the target rate.
No task priority/core changes were made. Do not describe the target as achieved.

## Implementation

- `gfx.cpp`: actual glyph bounds reject nonintersecting strips before bitmap decoding; stop
  decoding below the strip. Reject off-strip lines/discs/rings early. Rounded-rectangle interior
  uses spans; straight distances avoid unnecessary square roots.
- `tft_screens.*`: caller-owned per-frame geometry cache reuses heading-up positions, grid
  endpoints, trail/camera positions and lens triangles across strips. Reset when starting each
  new model/frame. No global mutable geometry shared between the loop and render task.
- A caller-owned warning-card cache stores runs of identical coverage for the fixed 160x116
  card. It is reusable when the close-state colour changes because coverage is unchanged.
  Pixels underneath are still blended every frame, so moving grid/trail detail remains exact.
- Long card-fill spans use exact component blend lookup tables and reuse the result for adjacent
  identical background colours. Both card colours tested against ALL 65536 RGB565 backgrounds.
- `tft_driver.cpp`: two internal DMA-capable bounce buffers plus two persistent transactions.
  Queue strip N while rendering N+1. Wait before reusing a buffer; drain at frame end and before
  commands, clock changes, panel shutdown and read-back. SPI errors remain counted. If draining
  fails, shutdown does not free buffers that DMA might still own.
- `tft_ui.cpp`: geometry caches are separate for render and bench. Render remains priority 1,
  core 0. Timing counters restart after a panel reinitialization for per-clock comparisons.

## Validation

- Existing TFT/font/strip tests pass, with original assertions retained.
- Added prepared-geometry strip/full-frame comparisons for all demo screens/close states and
  exhaustive blend comparisons. All 11 preview PNGs byte-identical to the backed-up 0.5.3 files.
- uart and native-usb build with zero warnings on the final successful build.
- Unit A flashed. Post-test stats: 0 capture drops, 0 write errors, 0 SPI errors.
- `tft verify`: all five speeds pass, 0/153600 mismatches and 0 SPI errors each; original 10 MHz
  restored. Physical colours/orientation still need the owner; software previews cannot check them.
- CRC-valid session backups before controlled flashes and a final export remain private in
  captures/backup-before-speed. No radio, detector, logger, GPS or partition source changes.

Final memory snapshot: internal heap free ~47 KB, minimum ~42 KB, largest block ~31 KB;
loop stack high-water mark ~10 KB of 16 KB. This is bench evidence, not an overnight soak.

## Next useful work

Cache glyph lookup/layout or decoded glyph masks without changing pixels; profile maximum
24-camera / 96-point trail scenes; correlate frame tails with filesystem/cache stalls. Further
changes need their own image, memory and capture checks. Only lower the cap after a measured
frame budget supports it. Keep 10 MHz saved until the owner has checked the physical picture.

See ai_bundle/0.5.4-CHECKS.md for exact commands and recovery files.
