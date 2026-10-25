# Portable Windows distribution

The portable alpha runs from one extracted folder. It includes Jester, app-local Qt/VC++/MuJoCo libraries, selected Atom runtime modules, processed assets, original examples and licenses. End users need Windows 10 version 1903+ / Windows 11 x64 and a compatible DX12/Vulkan graphics driver. O3DE, Qt SDK, Visual Studio, Python and Asset Processor installations are not required.

Double-click `Jester.exe`, `Jester DX12.cmd` or `Jester Vulkan.cmd`. Keep the complete folder together. Use a writable extraction location; local settings/logs are created under `user/`.

## Assemble from an SDK build

Build and process assets first. Python 3.9+ is used only for packaging:

```powershell
./build.ps1 -EngineRoot D:/O3DE -Assets
python tools/package_editor.py --output dist/Jester-0.1.0-alpha.1-windows-x64 --crt-root "C:/path/to/Microsoft.VC145.CRT" --archive
```

`--project-root` can point at another existing compiled project. `--engine-root` selects the SDK. The CRT directory comes from the Visual Studio x64 redistributable runtime. Output must be empty. The packager computes normal and delay-load PE dependencies, bundles application-local VC runtime files, and derives the asset dependency closure with Asset Bundler. It creates an empty metadata schema instead of distributing the developer asset database.

The supplied SDK ships editor/tools DLLs in **Profile**, an optimized configuration. This distribution uses that matching configuration and excludes PDBs; it is not a CMake Release or monolithic build. Mixing SDK configurations is avoided. `BUILD-INFO.json` records the version, source build identity, configuration and executable SHA-256. `FILE-MANIFEST.json` hashes the payload, and the optional ZIP has a separate SHA-256 file.

The payload excludes Python, builders, Asset Processor, tests, compilers, SDK source, developer settings and private character libraries. A small engine/project runtime manifest and registry settings are part of the executable's required data. Shader assets contain DX12, Vulkan and Null reflection data; the small Null module registers serialized shader types even when DX12/Vulkan is selected.

The Windows executable embeds a process-local UTF-8 code page and long-path-aware flag. It also sets `LC_CTYPE` to UTF-8 before SDK startup: the module loader converts UTF-8 paths using `mbstowcs_s`. Numeric formatting and the system locale are unchanged. See [Microsoft's UTF-8 application manifest documentation](https://learn.microsoft.com/en-us/windows/apps/design/globalizing/use-utf8-code-page).

## Validation

Validation includes launches from an unrelated working directory with SDK/Qt/Python paths removed, a separate user-data environment, DLL path inspection, actual DX12/Vulkan framebuffer readback, GUI workflows and archive relocation. Runtime libraries must resolve inside the distribution or Windows/driver directories; Windows security software can inject its own modules.

The validation host still has developer tools installed. A separate clean Windows VM is an additional test, not a claimed result of environment/path isolation. Release artifacts and validation logs remain local and ignored by Git.
