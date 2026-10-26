# Testing

Build the app, assets and regression targets:

```powershell
./build.ps1 -EngineRoot D:/O3DE -Assets -Tests
./tools/run_core_tests.ps1
./tools/run_gui_tests.ps1
./tools/run_viewport_smoke.ps1
./tools/run_gui_tests.ps1 -Suites Ui,Mvp,Notes,UxScope -Scale 1.5
```

Run from an interactive Windows desktop with a DX12-capable GPU. GUI suites use the actual Atom renderer. The viewport smoke checks rendering, resize, minimize/restore, framebuffer readback and shutdown at 100% and 150% scaling. Logs, images and JSON summaries are generated under ignored `artifacts/`. GUI/smoke runners fail on native Trace errors/asserts as well as process failures. For headless core execution with this SDK, set `$env:QT_QPA_PLATFORM="minimal"` before the core runner and remove it before native GUI tests; the SDK offscreen platform plugin is unavailable. Core reliability cases can take several minutes; the runner allows 20 minutes per test function.

There are 15 unique regression executables: Core, Physics, MvpCore, MuJoCo, Media, Reliability, Settings, Ui, Mvp, Visual, Automation, Notes, PhysicsUi, Motion and UxScope. Media is also included in the PowerShell GUI runner; do not count it twice.

Synthetic fixtures are included. Optional HumanM cases skip when their local assets are absent. `TRICKSTER_TEST_CHARACTER` selects a compatible character for existing import checks; `TRICKSTER_TEST_FBX` selects the optional native HumanM FBX fixture. Some checks expect the original HumanM bone names. No private model is needed to run the synthetic cases.

Previous migration validation on the development machine recorded 214 passed checks across the 13 suites, plus 63 at 150% scaling. That run included optional local assets and external physics reference data; it is historical evidence, not the expected pass count for a fresh checkout. Full external Godot reference experiments are inherited research and are outside the self-contained alpha checks.

For a local MCP/input smoke test, start the demo with `-Demo -Mcp`, save its connection configuration to an ignored path and use `tools/live_editor_smoke.py --help`. The final import uses the included `examples/welcome.glb`; `--import-file` selects another asset. Connection JSON contains a local access token and must remain private.

## DX12 and Vulkan

After building all regression targets, run the backend matrix with Python 3.9+ (standard library only):

```powershell
python tools/run_rhi_tests.py
python tools/run_rhi_tests.py --skip-smoke --scales 1.5 --suites Ui Mvp --output-dir artifacts/rhi-validation-150
./launch.ps1 -Rhi vulkan -Demo
```

The default matrix runs native viewport smoke at 100%/150% and all eight GUI suites at 100%, sequentially for DX12 and Vulkan. It checks the backend actually registered in the engine log, process exit status, native Trace errors/asserts, framebuffer capture and QtTest totals. A fallback to another RHI fails the check. Reports, build identity, executable SHA-256 and screenshots stay in ignored `artifacts/rhi-validation/`.

`--project-root` selects an existing compiled SDK project; `--bin-dir` selects a different binary directory. `--backends vulkan`, `--suites Ui Mvp` and `--smoke-only` narrow the check. Each GUI executable has a 600-second timeout; native smoke has a 90-second limit. The existing PowerShell GUI/viewport runners remain DX12 checks.

Shortcut tests explicitly activate the editor window and focus the viewport before sending synthetic Qt keys. A visible native rendering surface alone does not establish the active-window prerequisite for window-scoped actions. Real Windows input remains a separate check.

The matrix accepts `--language ru` for localization-aware suites. Older suites assert English text; use English for their full regression run. Preferences and capture destinations are isolated per case. Visual and Notes cover a physical 1280×720 window at both scale factors; scaling uses Qt's scale-factor override on the same Windows desktop, rather than changing the machine's display setting.

The restart workflow opens the included character, edits a pose key, saves a Unicode uppercase `.JST` path, captures all three modes, configures lighting/IBL/post, persists appearance/language, records a three-frame sequence, and restarts the process to verify saved data:

```powershell
python tools/verify_editor_workflow.py
python tools/verify_editor_workflow.py --exe "dist/<package>/Jester.exe" --project-root "dist/<package>" --isolate --output-dir artifacts/portable-workflow
python tools/update_translations.py --check
```

Workflow screenshots composite actual Atom framebuffer readback with Qt overlays. Plain `QWidget::grab()` images whose names end in `-widgets.png` are UI layout diagnostics and do not include the native renderer. Local MCP connection files contain tokens: keep the output directory ignored and publish only reviewed screenshots/reports.

Measured results and the initial failed runs are described in [the backend report](RENDER_BACKENDS.md).
