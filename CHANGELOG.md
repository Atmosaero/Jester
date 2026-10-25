# Changelog

## Unreleased

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
