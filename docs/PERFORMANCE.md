# Rendering measurements

Windows 10, GTX 1650 (572.83), 8 GB RAM, optimized O3DE Profile, Qt scale 1. One process at a time without compilation or other tests. Current build: **Jester 0.1.0-alpha.1 (3d49509)**. Fixture version 1 uses identical authored geometry/animation in the unchanged baseline and current renderer. These are individual host measurements.

## CPU deformation and preparation pacing

Profiling queries were OFF. CPU values average cumulative deltas after warmup. Baseline preparation/upload combines the scopes split in current telemetry. Seek includes Qt scheduling and the next RPI preparation callback; callbacks do not establish physical display presentation.

| API | Fixture (vertices) | Path | CPU skin (ms) | Palette (ms) | Prepare + buffer (ms) | Seek p95 (ms) | Callback p95 (ms) | Private commit (MiB) | Capture median (ms) |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
| DX12 | simple (2,106) | baseline | 0.125 | 0.000 | 0.192 | 47.92 | 29.34 | 1359 | 152.19 |
| DX12 | simple (2,106) | cpu | 0.168 | 0.011 | 0.278 | 39.28 | 12.83 | 1396 | 74.93 |
| DX12 | simple (2,106) | gpu | 0.001 | 0.028 | 0.031 | 28.68 | 27.58 | 1403 | 188.00 |
| DX12 | heavy-morph (113,490) | baseline | 7.362 | 0.000 | 8.950 | 62.00 | 33.62 | 1505 | 153.14 |
| DX12 | heavy-morph (113,490) | cpu | 9.908 | 0.006 | 10.992 | 62.00 | 37.67 | 1554 | 73.42 |
| DX12 | heavy-morph (113,490) | gpu | 0.001 | 0.023 | 0.022 | 30.78 | 15.51 | 1578 | 72.95 |
| DX12 | multi-mesh (8,424) | baseline | 0.455 | 0.000 | 0.642 | 46.96 | 31.80 | 1463 | 154.64 |
| DX12 | multi-mesh (8,424) | cpu | 0.606 | 0.005 | 0.873 | 46.34 | 11.53 | 1457 | 50.91 |
| DX12 | multi-mesh (8,424) | gpu | 0.001 | 0.037 | 0.031 | 91.20 | 9.67 | 1469 | 68.88 |
| VULKAN | simple (2,106) | baseline | 0.143 | 0.000 | 0.180 | 47.81 | 29.47 | 1192 | 157.46 |
| VULKAN | simple (2,106) | cpu | 0.255 | 0.008 | 0.298 | 41.17 | 20.68 | 1239 | 88.08 |
| VULKAN | simple (2,106) | gpu | 0.001 | 0.018 | 0.037 | 29.62 | 12.50 | 1254 | 67.32 |
| VULKAN | heavy-morph (113,490) | baseline | 7.775 | 0.000 | 8.863 | 63.13 | 29.90 | 1276 | 147.27 |
| VULKAN | heavy-morph (113,490) | cpu | 11.527 | 0.007 | 11.464 | 76.20 | 47.17 | 1317 | 97.75 |
| VULKAN | heavy-morph (113,490) | gpu | 0.001 | 0.021 | 0.041 | 36.20 | 12.98 | 1335 | 70.93 |
| VULKAN | multi-mesh (8,424) | baseline | 0.449 | 0.000 | 0.570 | 47.79 | 29.63 | 1232 | 152.79 |
| VULKAN | multi-mesh (8,424) | cpu | 0.820 | 0.007 | 0.956 | 56.45 | 20.26 | 1279 | 67.34 |
| VULKAN | multi-mesh (8,424) | gpu | 0.001 | 0.033 | 0.055 | 40.87 | 13.72 | 1299 | 58.46 |

Heavy has two authored morph targets; multi-mesh has four meshes. Current CPU/GPU cases share lighting, studio IBL, shadows, floor and post settings. Baseline retains its original simpler pipeline. Both harnesses request a 1280x720 window; their actual viewport sizes differ:

- DX12 baseline: 927x375 pixels.
- DX12 cpu: 939x371 pixels.
- DX12 gpu: 939x371 pixels.
- VULKAN baseline: 927x375 pixels.
- VULKAN cpu: 939x371 pixels.
- VULKAN gpu: 939x371 pixels.

Per-vertex CPU work is comparable. Whole-scene pacing, captures and memory also include the different viewport, pipeline and cache behavior. Cases share one process per API/path, so later memory rows include retained caches. Memory is process private commit, not VRAM. Capture/readback is excluded from playback/seek CPU measurements. Native output-buffer validation is a separate diagnostic, not a playback operation.

The CPU fallback is slower than baseline in this run: heavy skin/preparation is about 20.9 ms DX12 / 23.0 ms Vulkan versus 16.3 / 16.6 ms. DX12 multi-mesh GPU seek p95 is also worse (91.2 ms versus 47.0 ms baseline), despite lower deformation CPU cost. These are measured limitations, not a general speedup claim.

GPU deformation removes the heavy CPU vertex loop; the CPU fallback remains available. The table does not imply a corresponding FPS improvement: query-free callback intervals and seek scheduling remain limiting factors. No physical presentation timer or frame-wide GPU time is claimed.

## Optional native GPU timestamps

Separate runs with queries ON sum enabled native leaf scopes for parent effects. Values are the latest buffered timestamp sample and include query overhead. They are not averaged GPU frame times or displayed-frame latency.

| API | Default ACES/display (ms) | Bypass sRGB/display (ms) | Grading (ms) | Bloom (ms) |
|---|---:|---:|---:|---:|
| DX12 | 0.088 | 0.018 | 0.075 | 0.182 (16 scopes) |
| VULKAN | 0.088 | 0.019 | 0.098 | 0.186 (16 scopes) |

Disabled Bloom/grading have zero enabled scopes and report 0; unavailable query results are null. Neutral saturation/contrast bypasses grading. Display conversion remains in global bypass. Default heavy native skinning/morph scopes: DX12 0.315/0.112 ms; VULKAN 0.483/0.104 ms. Forward shading is separate; independently buffered scopes cannot establish a complete frame duration.

## Reproduce

Build `JesterPerformanceTests` in Profile, then run sequentially:

```powershell
& D:/O3DE/cmake/runtime/bin/cmake.exe --build build --config profile --target JesterPerformanceTests --parallel 1 -- /p:CL_MPCount=1
python tools/run_render_benchmarks.py --output-dir artifacts/perf-gpu
python tools/run_render_benchmarks.py --output-dir artifacts/perf-cpu --cpu
python tools/run_render_benchmarks.py --output-dir artifacts/perf-bloom --gpu-timing --post bloom --cases heavy-morph
```

Other effect variants: `--post default`, `bypass`, `grading`. Raw JSON includes actual main/benchmark executable SHA-256, authored settings, native viewport dimensions, CPU cumulative scopes, memory and capture distributions. Evidence: `artifacts/task-baseline/performance` and `artifacts/final-performance/{cpu,gpu,queries-*}`. All twelve current fixture cases and eight separate effect-query cases passed the runner's actual-RHI/native-error checks.
