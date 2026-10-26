# Final alpha verification

Product **0.1.0-alpha.1**, schema **9**, preference schema **1**. Product code **3d49509**; later test/documentation commits do not change the compiled editor. Windows 10 / GTX 1650 / driver 572.83 / O3DE SDK 26.10 / Qt 6.10.2, optimized Profile.

## Structure and behavior

[ARCHITECTURE](ARCHITECTURE.md) maps the Core, Atom and UI targets to their source modules. App is the bootstrap; project/animation code does not depend on Window. Mechanical moves were committed separately before behavior changes and freshly configured/built.

Native `.jst` projects preserve animation, curves, correction layers, contacts, notes and scene visuals. Lit, neutral Unlit and actual deformed mesh Wireframe share the document without modifying authored geometry/materials. Scene controls use real Atom lights, studio IBL, shadows, background/floor, camera and post processing. Visual history is independent of motion/analysis invalidation. GUI/MCP share validation, capabilities and applied-state queries.

Preferences include themes, Cyrillic-capable fonts, size/scale/density/icons, overlay colors, named layouts, action bindings with conflicts, preview/apply/cancel/reset, portable validated import/export, and live English/Russian Qt catalogs. User text, paths, names and machine enums remain authored/stable. See [SCENE_SETTINGS](SCENE_SETTINGS.md), [PERSONALIZATION](PERSONALIZATION.md) and [MCP](MCP.md).

## Algorithms and persistence

| Suite | Passed | Failed | Optional skips |
|---|---:|---:|---:|
| core | 17 | 0 | 2 |
| physics | 10 | 0 | 0 |
| mvpcore | 34 | 0 | 0 |
| mujoco | 16 | 0 | 1 |
| media | 7 | 0 | 2 |
| reliability | 16 | 0 | 2 |
| settings | 11 | 0 | 0 |

**111 passed, 0 failed, 7 optional skips**, including QtTest setup/cleanup. Six unchanged suites were verified on 9496429; MuJoCo was rerun completely on 3d49509 after correcting its obsolete schema-8 expectation to schema 9. The initial failed log is preserved. Generated migration data now goes to artifacts, and the repository example remains unchanged. Reliability completed in about 394 seconds.

Checks include incomplete-contact retarget validity/provenance, measurement-derived error peaks and missing values, quaternion curve selection coordinates, strict `.jst` extension handling, full serialization, visual/preferences validation, independent history and numerical analysis/physics invariants.

## Native editor

The complete 9496429 matrix passed 36 English cases plus eight Russian cases. The final product follow-up passed 20 consolidated native cases after the documented test corrections/repeats: four main-app smoke cases plus Visual/Notes for both APIs, both scales and both languages. Native Trace errors/asserts are zero. [RENDER_BACKENDS](RENDER_BACKENDS.md) records exact scope, binary hashes, numerical GPU comparison, initial failures and unchanged reruns.

Eight full 9496429 restart workflows passed in physical 1280x720 windows, covering edit/key/save, Unicode uppercase `.JST`, multiple documents/history, three modes, light/IBL/post, appearance/language, sequence capture and persistence after process restart. The final product also passed all eight portable restart workflows from a separate Cyrillic/space path with clean PATH, actual GPU skinning and loaded-module audits. Signed Defender/OBS host injections are recorded separately; no SDK/developer dependencies are required. Details and evidence are in [PACKAGING](PACKAGING.md).

## Repeat

```powershell
./build.ps1 -EngineRoot D:/O3DE -Assets -Tests
$env:QT_QPA_PLATFORM='minimal'
./tools/run_core_tests.ps1
Remove-Item Env:QT_QPA_PLATFORM
python tools/run_rhi_tests.py --scales 1 1.5
python tools/run_rhi_tests.py --skip-smoke --suites Visual Notes --scales 1 1.5 --language ru --output-dir artifacts/rhi-ru
python tools/verify_editor_workflow.py
python tools/update_translations.py --check
```

[PERFORMANCE](PERFORMANCE.md) contains the comparison method, timing scopes and commands. [GALLERY](GALLERY.md) contains reviewed native captures. [LIMITATIONS](LIMITATIONS.md) records the single-GPU scope, Qt DPI override, absence of a clean Windows VM and experimental physics limits. Local raw artifacts are ignored; MCP connection files must not be published.

## Delivered package

Ready folder: `dist/Jester-0.1.0-alpha.1-windows-x64-3d49509` (253.51 MiB). Archive: the same name with `.zip` (67.88 MiB), plus `.zip.sha256`. It contains 1,373 files and 23 native synthetic projects; file hashes, app build/version and all three translation catalogs were checked. No test executables, Qt Test, PDBs, Python, builders or Asset Processor are included.

The actual final ZIP passed CRC and full manifest checks, was extracted to a second Cyrillic/space path, and passed complete DX12/Vulkan English restart workflows with clean PATH, module auditing and native GPU skinning. Evidence: `artifacts/archive-3d49509/results.json` and `artifacts/final-package.json`. The previous published folder and ZIP retain their original hashes.

Final ZIP SHA-256: `c4739f5a30b0a4c745202ef342f85a3cb31e6a00acb325bc1667caf18b89a1d1`.
