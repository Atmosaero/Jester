# DX12 / Vulkan validation

2026-10-26. Windows 10 x64 build 19045, NVIDIA GeForce GTX 1650, driver 572.83, 8 GB RAM, O3DE SDK 26.10 / Qt 6.10.2, optimized Profile configuration.

## Complete editor matrix

Source **9496429**, product **0.1.0-alpha.1**. Fresh CMake configuration and builds of `JesterEditor`, `JesterRegression` and `JesterPerformanceTests` passed. The application reports its actual RHI; the runner also requires the requested RHI to be registered in the native log.

| Backend | Successful runner cases | QtTest passed | Optional skips | Native Trace errors/asserts |
|---|---:|---:|---:|---:|
| DX12 | 18/18 | 244 | 4 | 0 |
| VULKAN | 18/18 | 244 | 4 | 0 |

Each backend runs native smoke at 100%/150% plus all eight GUI suites at both scales: Ui, Mvp, Visual, Automation, Notes, PhysicsUi, Motion and UxScope. QtTest counts include initialization/cleanup. Skips are unavailable optional character fixtures. Synthetic cases run without private character assets.

The matrix covers real rendering, input/shortcuts, import/export, edits/history, multiple documents, MCP transport and SDK bridge, modal controls, physics jobs, notes, resize, minimize/restore and shutdown. Visual checks compare real framebuffer output for modes, lighting, IBL, shadows and post effects, along with still/sequence parity and unchanged authored motion. Translation/appearance cases preserve note/numeric drafts and user names, including a custom name/type equal to an English UI word.

## Russian UI matrix

The localization-aware Visual and Notes suites also passed all eight cases: both APIs, both scales, Russian preferences. Each API records 56 QtTest passes, zero failures and zero skips, with no native Trace errors/asserts. Together with English, Notes completed four full suite runs per API, exercising repeated native scene/window teardown. Evidence: `artifacts/verified-9496429-ru/results.json`.

## Save, edit and restart

All eight process-restart workflows passed: both APIs, both languages and both scale factors in physical 1280x720 windows. They open the included character, create a pose key, save a Unicode uppercase `.JST`, exercise multiple documents and Undo/Redo, capture three modes, configure light/IBL/post, change font/scale/language, save and restart. Reopened authored motion/visuals and preferences match; three sequence frames are written. Both process exits and native Trace checks pass for every case. Evidence: `artifacts/verified-9496429-workflow/results.json`.

## Native GPU correctness

The eight-influence fixture includes a morph target with position/normal/tangent deltas and a non-uniform/sheared joint transform. Readback compares the actual Atom skinning output streams with the CPU reference. At 100%, maximum position component error is 7.16e-7 on DX12 and 5.97e-7 on Vulkan; normal error is at most 2.45e-6, tangent/bitangent error below 1.88e-4 (native morph payload quantization). CPU/GPU framebuffer mean absolute channel error is 0 on both APIs.

Output-buffer readback is a separate diagnostic: this full-matrix run recorded about 1.13 s DX12 / 0.33 s Vulkan. It is excluded from playback/seek measurements. Framebuffer capture and optional native GPU pass queries have separate scopes in [PERFORMANCE](PERFORMANCE.md); no callback or tick count is presented as physical FPS.

## Reproducing and evidence

```powershell
python tools/run_rhi_tests.py --scales 1 1.5 --output-dir artifacts/verified-en
python tools/run_rhi_tests.py --skip-smoke --suites Visual Notes --scales 1 1.5 --language ru --output-dir artifacts/verified-ru
python tools/verify_editor_workflow.py --output-dir artifacts/restart-workflow
python tools/update_translations.py --check
```

Full English evidence: `artifacts/verified-9496429-en/results.json`; native images and numerical comparison JSON are inside each Visual case. Each result hashes the actual executed test binary, separately from the main application. Main application SHA-256: `66359e3a18a95841b5caec20a9ca73006e9021bcbf96745302cb8a5f4de39cf7`.

## Failures found and corrected

Initial native regressions exposed missing base normals in older meshes, repeated material initialization overriding samplers, missing directional shadows, and transparent geometry overwritten by a late background. Renderer fixes generate missing normals in a cache copy, initialize the handler once, enable only the supported main shadow caster, and use a native pre-transparency sky plus correctly premultiplied Unlit alpha. Tests retain Clamp/Repeat/UV1/MASK/BLEND CPU/GPU assertions.

Initial 150% runs exposed a second vertical Scene dock group, stale explicit dock minimum widths and a native child taller than its visible parent. Fixes preserve the compact layout and assert control visibility, timeline/client boundaries, actual status-bar clearance and native child geometry. Fractional native pixel-edge rounding allows at most one physical pixel, independently of the exact logical-geometry assertion.

A previous command built the unrelated O3DE `Editor` target, leaving the main Jester executable stale while explicitly named test targets were current. Final builds use **JesterEditor**; earlier stale-app workflow evidence is not attributed to the current editor. Failed runs remain under separate ignored artifact directories.

## Scope

This is functional verification on one GPU/driver. Scale tests use `QT_SCALE_FACTOR=1/1.5`, including physical 1280x720 windows, on the same Windows desktop; they do not change the OS display setting. The supplied SDK still emits missing-PDB link warnings and JSON Serialization context warnings during shutdown; native Trace errors/asserts fail the runner. A separate clean Windows VM has not been exercised. See [limits](LIMITATIONS.md) and [portable validation](PACKAGING.md).
