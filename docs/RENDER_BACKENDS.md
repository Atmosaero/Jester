# DX12 / Vulkan validation

2026-10-25 · Windows 10 x64, build 19045 · NVIDIA GeForce GTX 1650, driver 572.83 · O3DE SDK 26.10.

Application: **Jester 0.1.0-alpha.1**, commit `7ba0c7b`. Both APIs were tested using the same unchanged application binary. The driver advertises Vulkan 1.4.303; that is the driver's supported version, not a claim about the API version requested by the application.

| Check | DX12 | Vulkan |
|---|---|---|
| Native viewport, 100% and 150% | 2/2 passed | 2/2 passed |
| Seven GUI suites, 100% | 108 passed, 0 failed, 2 skipped | 108 passed, 0 failed, 2 skipped |
| UI + MVP suites, 150% | 31 passed, 0 failed, 1 skipped | 31 passed, 0 failed, 1 skipped |
| Real Windows mouse/keyboard workflow | Passed | Passed |
| Textured glTF import and native PNG capture | Passed | Passed |
| Local Quinn/Juri animation, 162 nodes | Passed | Passed |
| Graceful application shutdown | Exit 0 | Exit 0 |

Totals per backend: **139 passed, 0 failed, 3 skipped** across the GUI matrix, including QtTest initialization/cleanup. Optional Epic/HumanM fixtures cause the skips. Suites: Ui, Mvp, Automation, Notes, PhysicsUi, Motion and UxScope. The 150% runs repeat Ui and Mvp.

Smoke exercises camera motion, resize, minimize/restore, four mesh draw packets and actual framebuffer readback. Every successful run registered the requested RHI and reported zero native Trace errors/asserts. Selecting another backend would fail the runner. The new runner's smoke path was also exercised separately on both APIs.

Native input checks cover orbit, pan, wheel zoom, pose dragging, Unicode note entry, Ctrl+Enter, project save/reopen, E/W tool switching, Space start/stop and three recorded viewport frames. The local skinned project was sampled at frames 0, 153 and 255 with one fixed camera; image changes were verified and the source SHA-256 remained unchanged. User-supplied character data and captures remain local.

## Initial failures and test correction

The first Vulkan UI run failed at the synthetic E shortcut. An unchanged isolated rerun passed at 100%; its 150% run failed at Space and Ctrl+N. These runs reported zero native Trace errors/asserts and are preserved as failures.

The UI harness waited for exposure but did not establish active-window/viewport focus before window-scoped QAction shortcuts. Explicit activation and focus checks were added to the affected tests, preserving all existing assertions. The rebuilt UI suite passed on both APIs at 100% and 150%. Real Windows E/W and Space input also passed on the unchanged editor executable.

## Evidence and scope

Run [the matrix](TESTING.md#dx12-and-vulkan) with `python tools/run_rhi_tests.py`. Local evidence is under ignored `artifacts/rhi-validation/`: `final-validation.json`, `smoke-results.json`, `gui-100/`, `ui-reproduction/`, `ui-focus-fixed/`, `mvp-150/`, `smoke-runner/` and per-backend `native-input/` folders. The original failure logs remain separate from final results. Connection files contain local MCP credentials and stay ignored.

Application SHA-256: `92f0a254bc7032903749e6a82644fbfc9dd3de994cc29190d13498f13a588b8b`.

This is functional validation on one GPU/driver. GPU timings, performance rankings and GPU-assisted validation were not measured. The existing core/physics algorithms are independent of the rendering API and were not rerun for this matrix.

SDK warnings about repeated SceneMaterialSrg compilation and JSON Serialization shutdown context remain visible; they did not produce native errors/asserts or failing exit codes. Source inspection also shows that the alpha's About text hardcodes DX12; actual backend identity is established from RHI initialization logs.
