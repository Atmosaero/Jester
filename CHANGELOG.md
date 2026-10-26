# Changelog

## Unreleased

- Refresh documentation captures for the blue Blue Jay interface and add an Oddie greeting GIF. Keep note-card text readable in the active theme.

## 0.2.0-alpha.1 — 2026-10-26

Windows portable alpha with native GPU deformation, scene personalization and the O3DE Blue Jay interface. The latest UI changes are awaiting manual validation; no new runtime test matrix was run for this release.

- Restyle the editor with native O3DE Blue Jay controls, blue interaction states and consistent painted surfaces. Add an authored SVG logo, Windows multi-resolution ICO and visible window branding.
- Use icon-only viewport modes and Loop, green active MCP status, red scene-tab close buttons and pointed translation gizmo arrows.

- Fix compact 150% dock/timeline/native-capture alignment, viewport text contrast and Russian Scene group search.
- Preserve transparent Unlit surfaces with a native pre-transparency background; derive missing legacy mesh normals in the rendering cache and enable the supported main shadow caster.

- Add native Atom GPU skinning with eight influences, morphs, shear-correct normal transforms, precise tagged TBN deltas and CPU fallback; verify native output against CPU on DX12/Vulkan.
- Add authored Lit/Unlit/mesh Wireframe, Scene controls, studio IBL, separate visual floor, presets and pipeline exposure/tonemapping/grading/Bloom.
- Add shared preference persistence, searchable preview/apply/cancel UI, stable action bindings/layouts, scene presets, and live English/Russian Qt localization with packaged application/standard-dialog catalogs.
- Apply editor themes, fonts, density and overlay colors to the existing timeline, curves, notes, panels and dialogs; preserve text and numeric drafts across language/appearance preview.
- Fix repeated native material initialization overriding sampler updates; compare Clamp and UV1 CPU/GPU output. Drain final IBL uploads before native scene shutdown.
- Expose scene capabilities/authored/applied state and editor preferences through shared MCP validation.

- Persist scene visuals in schema 9 with independent visual Undo/Redo and revision tracking, preserving motion analysis and physics provenance.
- Skip hidden and unchanged geometry, update materials independently, report actual RHI and separate CPU deformation/preparation/buffer/build metrics.
- Add repeatable three-scene rendering benchmarks; distinguish RPI preparation callbacks from display presentation and measure captures separately.
- Reject unsupported CLI paths before initializing the native renderer.

- Use `.jst` as the sole native project extension across GUI, CLI, MCP, examples, catalogs and recovery; share classification and reject repeated/unsupported extensions.
- Separate native project serialization and glTF import/export; preserve existing authored data and schema migrations.

- Organize editor sources by responsibility while preserving Core/Atom/UI boundaries.
- Keep partially retargeted contact annotations valid, retaining source provenance and explicit warnings.
- Select validation problem peaks from measured errors in their actual units; retain missing/uncertain peaks as null instead of zero.
- Extract curve widgets and use the same quaternion key coordinates for painting, clicking and rectangle selection.
- Add shared, strictly validated visual/preference data models and atomic import/export checks.

- Add portable Windows packaging with app-local runtimes, a reduced asset closure, file hashes and ZIP output; exclude SDK tools, Python and developer data.
- Embed a UTF-8 Windows manifest so the SDK module loader can open DLLs after relocation to a Unicode path.
- Initialize UTF-8 character conversion for the SDK's `mbstowcs_s` path handling; mark uncommitted builds with a dirty source identity.

- Add DX12/Vulkan selection to the launcher and a backend matrix runner with verified RHI selection, isolated reports and timeouts.
- Check active-window/viewport focus before synthetic GUI shortcut tests.
- Validate both backends on Windows/GTX 1650: GUI checks, 100%/150% viewport capture, real Windows input, textured import and a local skinned animation.

## 0.1.0-alpha.1 — 2026-10-25

First public Jester alpha, derived from Trickster 0.5.0.

- Native O3DE Atom/DX12 viewport with imported PBR materials and CPU skinning.
- Qt editing tools: FK/IK, timeline, curves, correction layers, notes and retargeting.
- FBX/glTF import and export; compatible `.jst` project files.
- Experimental native MuJoCo physics and local MCP automation.
- Synthetic MIT-licensed examples and native regression suites.

Windows SDK source build; see [current limits](docs/LIMITATIONS.md).
