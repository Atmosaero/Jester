# Rendering measurements

Windows 10, GTX 1650 (572.83), 8 GB RAM, optimized O3DE Profile build, 100% Qt DPI. One process at a time without concurrent compilation. Fixture version 1 uses identical authored data in the unchanged baseline and current renderer. Individual runs, not hardware-independent promises.

## CPU deformation and pacing

Profiling queries were OFF. CPU values average cumulative deltas after warmup. Baseline preparation/upload combines two scopes split in current telemetry. Seek includes Qt scheduling and the next RPI preparation callback; callback intervals are not physical display presentation.

| API | Fixture (vertices) | Path | CPU skin (ms) | Palette (ms) | Prepare + buffer (ms) | Seek p95 (ms) | Callback p95 (ms) | Private commit (MiB) | Capture median (ms) |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|
| DX12 | simple (2,106) | baseline | 0.125 | 0.000 | 0.192 | 47.92 | 29.34 | 1359 | 152.19 |
| DX12 | simple (2,106) | cpu | 0.116 | 0.004 | 0.190 | 48.35 | 31.86 | 1357 | 150.77 |
| DX12 | simple (2,106) | gpu | 0.001 | 0.015 | 0.015 | 61.44 | 30.11 | 1366 | 157.83 |
| DX12 | heavy-morph (113,490) | baseline | 7.362 | 0.000 | 8.950 | 62.00 | 33.62 | 1505 | 153.14 |
| DX12 | heavy-morph (113,490) | cpu | 7.517 | 0.005 | 8.806 | 63.60 | 29.11 | 1552 | 142.47 |
| DX12 | heavy-morph (113,490) | gpu | 0.000 | 0.016 | 0.016 | 60.62 | 29.19 | 1579 | 154.69 |
| DX12 | multi-mesh (8,424) | baseline | 0.455 | 0.000 | 0.642 | 46.96 | 31.80 | 1463 | 154.64 |
| DX12 | multi-mesh (8,424) | cpu | 0.441 | 0.004 | 0.617 | 48.89 | 30.44 | 1510 | 151.91 |
| DX12 | multi-mesh (8,424) | gpu | 0.001 | 0.030 | 0.021 | 59.91 | 31.59 | 1527 | 152.27 |
| VULKAN | simple (2,106) | baseline | 0.143 | 0.000 | 0.180 | 47.81 | 29.47 | 1192 | 157.46 |
| VULKAN | simple (2,106) | cpu | 0.129 | 0.005 | 0.176 | 46.38 | 28.89 | 1198 | 156.57 |
| VULKAN | simple (2,106) | gpu | 0.001 | 0.018 | 0.038 | 46.65 | 31.15 | 1201 | 154.31 |
| VULKAN | heavy-morph (113,490) | baseline | 7.775 | 0.000 | 8.863 | 63.13 | 29.90 | 1276 | 147.27 |
| VULKAN | heavy-morph (113,490) | cpu | 7.733 | 0.005 | 8.593 | 62.45 | 32.68 | 1276 | 157.25 |
| VULKAN | heavy-morph (113,490) | gpu | 0.000 | 0.014 | 0.033 | 47.92 | 33.41 | 1261 | 156.62 |
| VULKAN | multi-mesh (8,424) | baseline | 0.449 | 0.000 | 0.570 | 47.79 | 29.63 | 1232 | 152.79 |
| VULKAN | multi-mesh (8,424) | cpu | 0.437 | 0.005 | 0.552 | 61.65 | 33.05 | 1235 | 136.50 |
| VULKAN | multi-mesh (8,424) | gpu | 0.001 | 0.027 | 0.049 | 45.89 | 32.89 | 1216 | 152.94 |

Heavy has two authored morph targets; multi-mesh has four meshes. Current CPU/GPU rows use identical lighting, IBL, floor and post settings. Baseline uses its original simpler pipeline. At the same 1280 ? 800 outer window, new controls reduce the viewport from 927 ? 375 to 869 ? 335 pixels: CPU per-vertex comparison is meaningful, but whole-scene/capture/memory numbers include this and the pipeline difference. Memory is process private commit, not VRAM. Readback is excluded from playback/seek CPU measurements.

Heavy deformation moves roughly 16 ms of CPU skinning/preparation/upload to about 0.03?0.05 ms of CPU palette/upload work. This does **not** imply a corresponding FPS increase: callback pacing remains around 30 ms p95, and DX12 seek p95 changes little.

## Optional native GPU timestamps

`--gpu-timing` sums actual enabled leaf-pass queries for parent effects. Values are the latest buffered query, not a frame-wide GPU timer, and include query overhead. Heavy fixture, 869 ? 335 pixels:

| API | Default ACES/display (ms) | Bypass sRGB/display (ms) | Grading (ms) | Bloom, 16 scopes (ms) |
|---|---:|---:|---:|---:|
| DX12 | 0.137 | 0.029 | 0.124 | 0.282 |
| VULKAN | 0.116 | 0.028 | 0.147 | 0.283 |

Disabled Bloom/grading have no enabled scopes and report 0; unavailable queries report null. Neutral saturation/contrast bypasses grading. Display conversion remains necessary in global bypass. Heavy native skinning/morph default queries: about 0.42/0.18 ms DX12 and 0.56/0.13 ms Vulkan, excluding forward shading. Independently buffered scopes do not establish displayed-frame latency.

## Reproduce

Build `JesterPerformanceTests` in Profile, then:

```powershell
python tools/run_render_benchmarks.py --output-dir artifacts/perf-gpu --backends dx12 vulkan --scales 1
python tools/run_render_benchmarks.py --output-dir artifacts/perf-cpu --cpu --backends dx12 vulkan --scales 1
python tools/run_render_benchmarks.py --output-dir artifacts/perf-bloom --gpu-timing --post bloom --cases heavy-morph --backends dx12 vulkan --scales 1
```

Other effect variants: `--post default`, `bypass`, `grading`. Raw JSON records settings, executable SHA, memory and capture/scope details. Evidence: `artifacts/task-baseline/performance`, `artifacts/performance-current/{cpu,gpu,queries-*}`. All baseline/current 3-fixture comparisons and eight corrected effect-query cases passed both APIs with zero Trace errors. Earlier attempts retain null parent timings in separate folders.
