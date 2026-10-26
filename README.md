# Jester

A character animation editor powered by **O3DE Atom (DX12 / Vulkan)**, Qt Widgets and MuJoCo. **0.1.0-alpha.1**; current development changes are listed in [CHANGELOG](CHANGELOG.md).

![Jester running on Atom](docs/images/editor.png)

FBX/glTF import, FK/IK, keys and curves, correction layers, retargeting, notes and experimental physics. Native **.jst** projects also save scene lighting, studio IBL and post effects. Lit, Unlit and mesh Wireframe, native GPU skinning, editor themes and live English/Russian UI are included.

Windows x64. Building requires O3DE SDK 26.10, Visual Studio 2026 and the SDK's Qt 6.10.2; the portable package includes its runtime dependencies.

```powershell
./build.ps1 -EngineRoot D:/O3DE -Assets
./launch.ps1 -Demo
```

[Build](docs/BUILD.md) | [Portable package](docs/PACKAGING.md) | [Screenshots](docs/GALLERY.md) | [Tests](docs/TESTING.md) | [Performance](docs/PERFORMANCE.md) | [Limits](docs/LIMITATIONS.md)

MIT. [Third-party notices](THIRD_PARTY.md). Private character libraries are not included. Evolved from [Trickster](https://github.com/Atmosaero/Trickster).
