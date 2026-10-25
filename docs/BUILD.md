# Build and run

Supported alpha configuration: Windows x64, DirectX 12 GPU, installed **O3DE SDK 26.10** (`o3de-sdk`), **Visual Studio 2026 / MSVC** with Desktop development with C++, Windows SDK, Git and PowerShell. Use the SDK's **Qt 6.10.2** and bundled CMake; do not mix in another Qt installation.

```powershell
git clone https://github.com/Atmosaero/Jester.git
cd Jester
./build.ps1 -EngineRoot D:/O3DE -Assets
./launch.ps1 -Demo
```

`-EngineRoot` selects the installed SDK path (default `D:/O3DE`). The SDK must be registered with O3DE under the engine name `o3de-sdk`; use the O3DE Project Manager to register the SDK if necessary. This repository is a standalone O3DE project; no parent Trickster checkout is required.

`-Assets` runs AssetProcessorBatch to compile shaders, materials and passes. Repeat it after changing assets. Normal launches read the processed local catalog and do not require a running Asset Processor. CMake downloads the pinned MuJoCo 3.3.7 Windows package and verifies its SHA-256. An internet connection is required for dependency downloads. The default build uses one compile job; increase `-Jobs` only when memory permits.

```powershell
./launch.ps1                         # Empty editor
./launch.ps1 -File ./examples/welcome.glb
./launch.ps1 -Demo -Mcp -Port 18530  # Local authenticated automation
./launch.ps1 -Smoke                  # Native viewport check, then exit
```

Output: `build/bin/profile/Jester.exe`. `-Configuration release` is supported by both build and launch scripts. Cache, user settings, test output and imported libraries remain local and ignored by Git. The source version lives in `project.json`; CMake generates the application version and embeds the current Git SHA as the build identity.

Open `.fbx`, `.glb`, `.gltf` or `.trickster` files from File → Open. Try Scene → Choose scene for included synthetic templates. Save As preserves the original examples.

SDK upgrades may require adapting native Atom APIs. To regenerate the vendored PBR extensions after an SDK upgrade, install Python 3.9+ and run `python tools/prepare_materials.py --engine-root D:/O3DE`, then review the diff and process assets again.
