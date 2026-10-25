# Testing

Build the app, assets and regression targets:

```powershell
./build.ps1 -EngineRoot D:/O3DE -Assets -Tests
./tools/run_core_tests.ps1
./tools/run_gui_tests.ps1
./tools/run_viewport_smoke.ps1
./tools/run_gui_tests.ps1 -Suites Ui,Mvp,Notes,UxScope -Scale 1.5
```

Run from an interactive Windows desktop with a DX12-capable GPU. GUI suites use the actual Atom renderer. The viewport smoke checks rendering, resize, minimize/restore, framebuffer readback and shutdown at 100% and 150% scaling. Logs, images and JSON summaries are generated under ignored `artifacts/`. GUI/smoke runners fail on native Trace errors/asserts as well as process failures. Core reliability cases can take several minutes; the runner allows 20 minutes per test function.

There are 13 unique regression executables: Core, Physics, MvpCore, MuJoCo, Media, Reliability, Ui, Mvp, Automation, Notes, PhysicsUi, Motion and UxScope. Media is also included in the GUI runner; do not count it twice.

Synthetic fixtures are included. Optional HumanM cases skip when their local assets are absent. `TRICKSTER_TEST_CHARACTER` selects a compatible character for existing import checks; `TRICKSTER_TEST_FBX` selects the optional native HumanM FBX fixture. Some checks expect the original HumanM bone names. No private model is needed to run the synthetic cases.

Previous migration validation on the development machine recorded 214 passed checks across the 13 suites, plus 63 at 150% scaling. That run included optional local assets and external physics reference data; it is historical evidence, not the expected pass count for a fresh checkout. Full external Godot reference experiments are inherited research and are outside the self-contained alpha checks.

For a local MCP/input smoke test, start the demo with `-Demo -Mcp`, save its connection configuration to an ignored path and use `tools/live_editor_smoke.py --help`. The final import uses the included `examples/welcome.glb`; `--import-file` selects another asset. Connection JSON contains a local access token and must remain private.
