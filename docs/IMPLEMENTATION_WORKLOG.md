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
2. **Mechanical moves implemented, verification/build pending:** 104 files grouped by responsibility; App now contains bootstrap only. All internal includes are module-qualified; CMake/resource references and tests/docs updated. No behavior change in this step.
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

## Checks and evidence

- Remote main and clean local status: verified at task start.
- Baseline `cmake --build <existing snapshot>/build --target JesterEditor JesterRegression --config profile --parallel 1 -- /p:CL_MPCount=1`: exit 0. Log artifacts/task-baseline/build.log. SDK missing-PDB warnings are pre-existing.
- Initial headless runner selected `offscreen`, which this SDK does not ship. Core/Media/Physics aborted before QtTest initialization; this is a runner environment failure, not a product regression. Re-running those exact executables with shipped `minimal` plugin passed (logs *-minimal.txt). MvpCore passed 31/0/0 and MuJoCo passed 16/0/1. Reliability and native RHI matrix still running. Preserve failed attempts in baseline.json and consolidate corrected results separately.

## Commits

Pending.

## Next action

Verify the mechanical move against the original source bodies, commit it separately, then fresh-configure/build canonical source using the existing dependency download cache. Let baseline tests continue against the immutable old snapshot. Capture pre-change renderer benchmarks before altering its sources. Then implement behavioral bug fixes and .jst model/serialization.
