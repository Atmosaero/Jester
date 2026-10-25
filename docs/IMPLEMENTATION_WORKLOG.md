# Jester scene, structure and personalization work log

This is the single continuity file for the complete task supplied on 2026-10-25. Continue from **Next action** after context compaction. Do not treat completion of an intermediate stage as completion of the task.

## Scope and constraints

- Preserve C++20, Qt Widgets, O3DE Atom, MuJoCo and JesterCore/JesterAtom/JesterUI boundaries.
- Preserve DX12/Vulkan, imports, animation, retarget, notes, MCP, export and SDK-free portable startup.
- Product version stays `0.1.0-alpha.1`; record changes under Unreleased, identify builds by Git SHA.
- Use logical local commits; separate mechanical movement from behavior changes. Preserve the published alpha and user libraries. No force-push or replacement of release assets.
- One compiler job on this 8 GB host. Native rendering tests run sequentially. No success claim for unperformed checks.

## Baseline

- Canonical repository: `D:/Trickster/Jester`, initially clean at `ae49d4d4c76f0dbc589a2c236ec33102f6a2af8e`. Remote main checked on task start and matches.
- Root `D:/Trickster/AGENTS.md`: version policy only. No nested AGENTS found in canonical source.
- Existing SDK build: `D:/Trickster/Jester 3D/artifacts/release-source/build`, optimized Profile, SDK `D:/O3DE`, Qt 6.10.2, Visual Studio 2026. Its source/build artifacts provide the pre-change baseline; implementation belongs in canonical repository.
- Published portable alpha: `dist/Jester-0.1.0-alpha.1-windows-x64`; retain it intact. Binary SHA `400853632010707c9f4bef6f465a63288b6d89904d7daea3d83ee896ce81a8d6`, source identity `7ba0c7b-dirty` (portable fixes committed subsequently as ae49d4d).
- Previous recorded validation is historical: full RHI checks before Unicode fix and portable final startup checks. Fresh task baseline runs still pending.

## Responsibility and dependency map

- `core.h` owns Scene/mesh/animation data; core/document/workspace/evaluator/geometry/posing/edit_service implement state, evaluation and edits. They use Qt data types, not Window or Atom.
- animation_curves/key_editing/curve_bake/rotation_math/motion implement animation; motion_analysis/analysis_jobs/motion_compare/motion_validation/expectations implement measurements and jobs.
- motion_annotations/notes_navigation provide annotation data/context. document/recovery and core serializer cover project persistence.
- fbx/gltf/fbx_export handle interchange; scene_catalog/demo_scenes provide example discovery/creation.
- physics*, MuJoCo, whole_body/flight/dynamics_optimizer implement physics services and provenance.
- `App/AtomViewport.*` and `AtomEditorScene.cpp` currently mix renderer implementation into bootstrap directory. Renderer consumes Scene; JesterAtom depends on JesterCore and SDK graphical targets.
- Window-related implementation is spread across window/layout/helpers/panels/scenes/motion/retarget/physics/automation files. Viewport and timeline are widgets. Graph editor needs extraction of its independent class.
- editor_api/editor_ui_api/mcp_server/mcp_tools provide automation; settings, appearance and localization currently need dedicated services.

## Plan and status

1. **In progress: baseline tests/benchmarks.** Baseline build passed. Existing source text matches canonical ae49d4d after normalizing line endings. Baseline tests are running against the unchanged secondary snapshot.
2. **Mechanical modularization verified:** 104 files grouped by responsibility; App now contains bootstrap only. Qualified includes, CMake/resource/test/doc references updated. Source bodies checked equivalent. Fresh configure and complete JesterEditor/JesterRegression build passed; short Core/MVP/Media and GUI on both RHIs passed.
3. Fix retarget annotation validity, measurement peak selection and quaternion selection, with behavioral tests. Remove hidden/unchanged mesh work; honest split CPU metrics and active RHI; real GPU skinning with CPU fallback and comparison.
4. Make `.jst` the sole native format through one shared classifier, convert owned fixtures, reject unsupported extensions consistently; test Unicode/space paths and round trips.
5. Shared validated scene visual model, persisted independently from animation-analysis revisions; Lit/Unlit/real mesh Wireframe, action/header/shortcut controls.
6. Scene dock: real lighting, IBL/environment, background/grid/floor, camera, presets and undo transactions.
7. Actual pipeline exposure/tonemapping/color/bloom; preserve overlays, capture parity and measure costs.
8. Searchable Preferences service/dialog: themes/font/DPI/density/accent/overlays/layouts/shortcuts, live preview and rollback, validated versioned import/export without secrets/paths.
9. Qt translation pipeline, Russian/English/system and live retranslation of every owned UI surface; stable serialization/API values, Qt standard translations, packaged catalogs.
10. MCP scene capabilities/read/write/applied state, shared GUI validation and errors; update schemas/examples/docs.
11. Full end-to-end tests on both RHIs and DPI scales, languages/compact window, Unicode/multi-doc/undo/input/capture; performance comparison; fresh final build and portable package, real screenshots and repeatable commands.

## Findings and decisions

- Existing ae49d4d already fixes portable Unicode DLL lookup via manifest + LC_CTYPE, supports RHI launch selection, and packages PE dependency/asset closures. Preserve these fixes.
- About still reports DX12 statically (historical backend report documents it). Other requested defects require current-code reproduction.
- Baseline SDK build and published alpha remain unchanged while canonical sources are refactored. Canonical fresh build will reuse only downloaded dependency caches to avoid unnecessary downloads.
- Mechanical mapping: `core/{scene,animation,analysis,annotations}`, `io/{project,import,export,catalog}`, `physics`, `render/atom`, `ui/{window,viewport,timeline,curves,notes,motion,physics,widgets}`, `settings`, `automation`. Headers remain beside implementations. The data-only JSON validation helpers moved from editor_api_util.h to core/scene/json_helpers.h, so Core does not depend on the automation directory.
- Native project serialization is currently embedded in gltf.cpp. Keep it intact in the mechanical move; split it into io/project as part of the native-format implementation, with round-trip tests.
- Confirmed in current code: hidden meshes are skinned before visibility assignment; uploadMs includes tangent preparation; timer compares all poses every 16 ms; viewport callback is mislabeled presented frame; contact annotation remapping can lose a required endpoint; expectation peak falls back to zero; quaternion box selection uses raw values.
- SDK exposes real SkinnedMeshFeatureProcessor, ImageBasedLightFeatureProcessor, PostProcess settings and MainPipeline; implement against local headers/source rather than inventing interfaces.
- Prepared independent `core/scene/visual_settings.{h,cpp}` and `settings/preferences.{h,cpp}` models while the mechanical build runs. They are not yet linked or tested. Visual fields cover mode, main/fill lights (lux), IBL, background/floor/grid, projection and post effects. Validation is atomic and rejects unknown fields/types/ranges. Preferences export contains no paths/MCP fields; validates schema version, shortcuts and layout encoding.
- Installed Qt includes lupdate/lrelease/LinguistTools. SDK exposes GammaSRGB, ACES, Reinhard and additional display mappers; manual exposure, HDR grading, bloom and IBL interfaces are present.
- GPU investigation: native shader consumes BLENDINDICES (packed pairs, first index in high 16 bits) and BLENDWEIGHTS (one float per influence). Installed LinearSkinningCS.azsl uses `Load(jointIdStartOffsetForVertex + influenceIndex)` although Load offsets are bytes and influenceIndex increments by two: verify/fix the stride before claiming eight-influence parity. SDK blends bone matrices before inverse-transpose normal transformation, whereas the CPU reference blends separately transformed normals. Preserve CPU-reference behavior through a project shader adaptation or explicit documented fallback. No GPU implementation/validation claim yet.
- Official upstream SkinnedMeshInputBuffers.cpp saved under ignored artifacts/sdk-reference for reference (SDK ships headers/assets but omits this private implementation). Use installed shader/header interfaces as the actual ABI.

## Checks and evidence

- Remote main and clean local status: verified at task start.
- Baseline `cmake --build <existing snapshot>/build --target JesterEditor JesterRegression --config profile --parallel 1 -- /p:CL_MPCount=1`: exit 0. Log artifacts/task-baseline/build.log. SDK missing-PDB warnings are pre-existing.
- Initial headless runner selected `offscreen`, which this SDK does not ship. Core/Media/Physics aborted before QtTest initialization; this is a runner environment failure, not a product regression. Re-running those exact executables with shipped `minimal` plugin passed (logs *-minimal.txt). MvpCore passed 31/0/0 and MuJoCo passed 16/0/1. Reliability and native RHI matrix still running. Preserve failed attempts in baseline.json and consolidate corrected results separately.
- Baseline completed: Core 17/0/2, MvpCore 31/0/0, Media 7/0/2, Physics 10/0/0, MuJoCo 16/0/1, Reliability 16/0/2 (405.98 s). Both RHI matrices passed all 16 runner cases (native smoke + seven GUI suites per API, 100%). Corrected baseline record: artifacts/task-baseline/baseline-consolidated.json; original offscreen failures remain recorded separately.
- Fresh canonical CMake configure succeeded (84.6 s). Dependency packages and MuJoCo source cache reused; all application objects compile into a new canonical build directory. Full JesterEditor/JesterRegression build is still running. Processed Cache/pc and asset database copied from baseline for post-move runtime tests; no original source or release payload changed.
- Fresh modular build completed exit 0, session 25514 closed. Post-move Core 17/0/2, MVP 31/0/0, Media 7/0/2; UI + MVP on DX12/Vulkan all four cases passed (artifacts/modular-build/).
- Added three behavioral core regressions. Before fixes: all three failed for invalid partial contacts, expectation peak at 0 instead of 0.5 seconds, and invented peak for undefined measurement (artifacts/defect-regressions/before.txt). After fixes: complete MVP suite 34/0/0 (after.txt).
- Retarget now retains sourceAnnotation/source fingerprint/unmapped nodes and warnings; an incomplete allowed-contact becomes retarget-incomplete, leaving the strict validator intact. Measured peaks include normalized units and metric/value; missing measurements remain null and count unmeasured samples. UI navigation falls back to the interval start when no peak exists (final UI rebuild/test still pending).
- CurveCanvas and NativeCurvePlot extracted into paired headers/cpps while preparing the quaternion regression; not yet added to CMake. Their behavior is currently unchanged, including the box-selection bug. Do not build JesterUI until adding these two new sources.
- Curve widgets and settings models are now linked and built. A direct Qt widget regression reproduced quaternion box failure while clicking succeeded; a shared keyPoint function now serves paint/hit/box coordinates. Before: SettingsTests 5 passed / 1 failed; after: 6/0/0. Tests also verify atomic visual validation, invalid/secret preference fields, shortcut conflicts, malformed layouts, schema version, and Unicode-path export/import.
- JesterEditor rebuild with extracted widgets and UI peak navigation succeeded (artifacts/defect-regressions/curve-green-build.log). New preferences/visual models are data-only so far; no scene controls/application or Preferences UI is claimed yet.
- Native `.jst` implementation completed and built. Shared classifier and save normalization in io/project/project_format; native serializer and glTF exporter split from importer, private shared inline JSON values. All positive native paths/catalogs/MCP/help/fixtures/docs migrated; unsupported old extensions rejected. Recovery now stores regular `.recovery.jst` roots plus metadata; no legacy recovery loader. Tests generating demos now use a temporary folder instead of overwriting owned examples.
- Renamed all 23 owned example projects. Compared nodes/meshes/keys/curves/layers/contacts/annotations against their committed originals: unchanged. Original generated fixture mutations from baseline tests were restored before renaming. No user library files changed.
- `.jst` build session 72123 completed exit 0. Core 17/0/2, MVP 34/0/0, Media 7/0/2, Settings 8/0/0. Settings includes full authored-data Unicode-path round trip, strict extension rejection, recovery and preservation of original file.
- Live `.jst` workflows passed DX12 and Vulkan, exit 0 / Trace errors 0: create pose animation and Cyrillic note, save uppercase .JST under Cyrillic/spaced directory, close/reopen, compare keys/annotations/layers/contacts/curves/meshes, reject unsupported save without overwrite, reject unsupported open, export GLB, native screenshot, graceful quit. Evidence artifacts/jst-validation/live-results.json. CLI positive/negative checks session 61080 (collect result).
- Baseline performance complete, both APIs / three deterministic scenes: artifacts/task-baseline/performance/results.json and per-case PNG/JSON. Heavy case 113,490 vertices / two morphs: CPU skin/update 7.36 ms DX12, 7.77 ms Vulkan; legacy upload metric (includes preparation/tangents) 8.95 / 8.86 ms. Seek-to-preparation p95 62.0 / 63.1 ms; separate readback median 153 / 147 ms. These are CPU/callback/capture scopes, not GPU/display frame times. Simple 2,106 vertices; multi 4 meshes / 8,424 vertices. Baseline binary remains SHA40085363..., matching published alpha. Canonical benchmark target still needs registering in CMake; sources/tool ready.
- Prepared `settings/preferences_service.*` and `settings/editor_appearance.*` (not linked/tested). Service previews/apply/persists JSON, with per-user fallback for read-only installations. Appearance uses Fusion proxy style, dark/light/high-contrast palettes, font/UI/icon/density scale, and text-input shortcut guarding. Existing inline applyAppearance has not yet been replaced. Localization and preferences dialogs/controllers are not implemented yet.
- Added deterministic performance fixtures (simple mannequin, subdivided heavy mannequin with two authored position/normal/tangent morphs, four mesh parts) and a benchmark harness. It records CPU cost, preparation callback intervals (explicitly not presented/GPU frames), seek-to-preparation latency, process memory and separately timed readback. Diagnostic-only target added to old baseline CMake; baseline application C++ and binary remain unchanged. Baseline benchmark build/run session 33671 in progress. Do not run another compiler or native GUI while its measurements execute.

## Commits

- f2ef57c — mechanical modularization, responsibility map and continuity log; source bodies verified unchanged except qualified includes/resource path.
- 7f93136 — verified contact provenance and measured/missing peak fixes; MVP suite 34/0/0.
- ca35474 — tested visual/preferences data models, extracted curve widgets, quaternion hit/box fix; Settings 6/0/0 before native-format tests.

## Current checkpoint (2026-10-26)

- Native-format stage committed as da9fe1e. CLI checks corrected and completed on DX12/Vulkan: positive Unicode uppercase JST open/capture exits 0, unsupported extension exits 2 before SDK startup, all Trace errors 0 (artifacts/jst-validation/cli-results.json). App/main.cpp early argument validation remains a separate uncommitted fix.
- Added Scene.visuals and project schema9 for newly authored visual data, dedicated VisualCommand history/gesture merge, visualRevision/poseRevision, persisted session Undo/Redo. Physics provenance excludes visuals and the proxy UI watches motionRevision rather than the general revision. Product version unchanged.
- Added behavioral settings tests for motion/physics/analysis stability, visual history merging/session restore and atomic schema9 serialization. Settings tests passed 11/0/0; Core17/0/2 and MvpCore34/0/0.
- Renderer CPU path now skips all hidden geometry work before cache/revision consumption, rechecks the latest state on reshow, updates materials independently of mesh rebuild, splits CPU skin/preparation/buffer update/build telemetry, obtains actual RHI, and uses honest prepared-ordinal playback diagnostics. Viewport only compares geometry on document changes, only sets changed camera matrices, and avoids unconditional overlay repaint; resource compilation still polls. Added native widget regression for hidden/material/unchanged domains. Full single-job build passed. Native MVP suite passed 20/0/0 on EACH DX12/Vulkan, exit0, Trace errors0. Evidence artifacts/visual-foundation/rhi/results.json.
- Completed full build session 61213 (exit0): artifacts/visual-foundation/build.log, targets Editor/Settings/MVP GUI/Core/MVP Core/Performance, single compiler. Source layout is now safe to update for the next build.
- Newly prepared but UNLINKED/UNVERIFIED modules: gpu_skinning.hcpp (native skinning/morph proxies and conservative palette bounds), AtomVisualScene.cpp (lights, IBL, pipeline post and separate visual floor), scene_settings_panel/controller (real model controls, preset/undo/header modes), preferences service/appearance/localization and live text helpers. They are not advertised as completed features. GPU source shader adaptation generated by tools/prepare_skinning.py under Assets/Shaders/SkinnedMesh; requires AssetProcessor and CPU/GPU parity checks. No installed SDK files changed.

## Next action

Visual-domain/CPU-render foundation and benchmark harness passed all checks and are ready for a logical commit (exclude unlinked modules). Next link/finish native GPU skinning and visual pipeline, run AP for project shader override, validate eight-weight/morph/TBN CPU parity and honest timings on both APIs. Install Scene dock and header modes, finish wireframe/background/grid/camera, preferences/action/layout controllers, full live Qt RU/EN translation, shared MCP scene settings/capabilities, complete required workflow/matrix/performance/package evidence. Preserve published alpha and baseline. Task remains active until every stage is finished.
