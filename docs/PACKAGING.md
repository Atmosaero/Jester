# Portable Windows distribution

The portable alpha runs from one extracted folder. It includes Jester, app-local Qt/VC++/MuJoCo libraries, selected Atom runtime modules, processed assets, original examples and licenses. End users need Windows 10 version 1903+ / Windows 11 x64 and a compatible DX12/Vulkan graphics driver. O3DE, Qt SDK, Visual Studio, Python and Asset Processor installations are not required.

Double-click `Jester.exe`, `Jester DX12.cmd` or `Jester Vulkan.cmd`. Keep the complete folder together. Use a writable extraction location; local settings/logs are created under `user/`.

## Assemble from an SDK build

Build and process assets first. Python 3.9+ is used only for packaging:

```powershell
./build.ps1 -EngineRoot D:/O3DE -Assets
$jesterSha = git rev-parse --short HEAD
$jesterVersion = (Get-Content project.json -Raw | ConvertFrom-Json).version
python tools/package_editor.py --output "dist/Jester-$jesterVersion-windows-x64-$jesterSha" --crt-root "C:/path/to/Microsoft.VC145.CRT" --archive
```

`--project-root` can point at another existing compiled project. `--engine-root` selects the SDK. The CRT directory comes from the Visual Studio x64 redistributable runtime. Output must be empty. The packager computes normal and delay-load PE dependencies, bundles application-local VC runtime files, and derives the asset dependency closure with Asset Bundler. It creates an empty metadata schema instead of distributing the developer asset database.

The supplied SDK ships editor/tools DLLs in **Profile**, an optimized configuration. This distribution uses that matching configuration and excludes PDBs; it is not a CMake Release or monolithic build. Mixing SDK configurations is avoided. `BUILD-INFO.json` records the version, source build identity, configuration and executable SHA-256. `FILE-MANIFEST.json` hashes the payload, and the optional ZIP has a separate SHA-256 file.

The payload excludes Python, builders, Asset Processor, tests, compilers, SDK source, developer settings and private character libraries. A small engine/project runtime manifest and registry settings are part of the executable's required data. Shader assets contain DX12, Vulkan and Null reflection data; the small Null module registers serialized shader types even when DX12/Vulkan is selected.

The Windows executable embeds a process-local UTF-8 code page and long-path-aware flag. It also sets `LC_CTYPE` to UTF-8 before SDK startup: the module loader converts UTF-8 paths using `mbstowcs_s`. Numeric formatting and the system locale are unchanged. See [Microsoft's UTF-8 application manifest documentation](https://learn.microsoft.com/en-us/windows/apps/design/globalizing/use-utf8-code-page).

## Validation

Validation includes launches from an unrelated working directory with SDK/Qt/Python paths removed, a separate user-data environment, DLL path inspection, actual DX12/Vulkan framebuffer readback, GUI workflows and archive relocation. Runtime libraries must resolve inside the distribution or Windows/driver directories; Windows security software can inject its own modules.

The validation host still has developer tools installed. A separate clean Windows VM is an additional test, not a claimed result of environment/path isolation. Release artifacts and validation logs remain local and ignored by Git.

## Verified candidate

Local product build **3d49509**, version **0.1.0-alpha.1**: `dist/Jester-0.1.0-alpha.1-windows-x64-3d49509`. The pristine payload is about 253.51 MiB with 12 runtime module groups and 50 runtime files. It includes the skinning/morph/SkyBox shader closure, studio IBL/mips, Jester EN/RU and standard Qt RU catalogs, synthetic `.jst` examples, runtime libraries and notices. `BUILD-INFO.json` and `FILE-MANIFEST.json` identify the actual binary and payload.

A separate copy under a path containing spaces and Cyrillic passed all eight complete workflows: DX12/Vulkan, English/Russian, Qt scales 1/1.5, physical 1280x720 windows. Every case verifies native GPU skinning, saves/reopens Unicode uppercase `.JST`, compares authored motion/visuals and preferences after restart, and captures the three modes plus a sequence. Both processes exit 0 with zero native Trace errors/asserts. CWD is outside the package; PATH contains only Windows directories and SDK/Qt/Python environment overrides are removed.

Loaded-module audits found no SDK/developer runtime dependencies. This host injects Microsoft Defender `MpOAV.dll` and OBS `graphics-hook64.dll` from ProgramData. Their Authenticode signatures were verified (Microsoft Windows / Hugh Bailey), hashes/publishers recorded separately, and all payload PE normal/delay imports were checked: none depend on these hooks. They are not distributed or required by Jester. No security/capture software was disabled.

The strict default audit rejects all external modules. For this host the explicit `--allow-signed-host-hooks` option permits only those exact standard paths and expected valid publishers; all other external DLLs remain errors:

```powershell
python tools/verify_editor_workflow.py --exe "<relocated-package>/Jester.exe" --project-root "<relocated-package>" --isolate --allow-signed-host-hooks --output-dir artifacts/portable-workflow
```

Evidence: `artifacts/portable-3d49509-signed-hooks/results.json` and each process's `loaded-modules.json`. The initial strict rejection is retained separately. A separate clean Windows VM has not been tested. The existing published alpha folder/ZIP is retained unchanged.

The full Visual suite also passed on both APIs using the package's own assets/runtime, with SDK/Qt environment paths removed: 14 checks per API, zero native Trace errors/asserts. The unshipped test EXE and Qt Test DLL were staged only in a separate validation copy. That test EXE retains an ANSI bootstrap and required an ASCII copy path; the shipped UTF-8 Jester executable separately passed the Cyrillic-path workflows. Evidence: `artifacts/portable-native-ascii-3d49509/results.json`; the failed test-bootstrap attempt remains under `portable-native-3d49509`.

The final ZIP additionally passed CRC/full file-manifest verification, extraction to a second Cyrillic/space path and complete DX12/Vulkan restart workflows. See `artifacts/archive-3d49509/results.json` and [final verification](VALIDATION.md) for the delivered archive checksum. The archive includes the runtime/functional documentation; this later archive-roundtrip result is an external verification record.
