# Changelog

## Unreleased

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
- FBX/glTF import and export; compatible `.trickster` project files.
- Experimental native MuJoCo physics and local MCP automation.
- Synthetic MIT-licensed examples and native regression suites.

Windows SDK source build; see [current limits](docs/LIMITATIONS.md).
