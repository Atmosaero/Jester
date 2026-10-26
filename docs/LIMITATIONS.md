# Alpha limits

- Windows x64 with Atom DX12 and Vulkan has been validated on a GTX 1650, driver 572.83; see the [backend test report](RENDER_BACKENDS.md). [Portable packaging](PACKAGING.md) is available from an SDK build.
- Native GPU skinning/morphs and the complete CPU fallback are compared on both RHIs. Wireframe uses cached topology with CPU-deformed edge positions. Optional native pass timestamps require `JESTER_GPU_TIMING=1`; they do not measure presentation or the entire GPU frame. See [measurement scopes](PERFORMANCE.md).
- Scene lighting includes a studio IBL, main/fill lights and pipeline post effects. Atom supports one directional shadow caster; fill shadows are rejected explicitly. Visual floor height is separate from MuJoCo ground.
- MuJoCo tools are experimental animation aids. Estimated human rigs are not measured biomechanics. A replay, a plausible pose or a COM display alone does not certify physically valid motion. Solver failures and approximate results remain visible.
- `.jst` schema 9 stores scene visuals and reads schemas 1-8. Review imported humanoid roles, IK chains, scale and joint limits before editing or simulating.
- Optional tests need user-supplied character fixtures. Those assets and local credentials are excluded from the repository.
- The tested SDK can print JSON Serialization context warnings during shutdown; native Trace errors/asserts are treated as failures by the GUI/smoke runners.
- DPI verification uses Qt scale factors 1 and 1.5 on the development desktop, including physical 1280×720 windows. A separate Windows display-setting change and a clean Windows VM have not been exercised; portable verification uses relocation, clean PATH and loaded-module inspection.
