# Alpha limits

- Windows x64 with Atom DX12 and Vulkan has been validated on a GTX 1650, driver 572.83; see the [backend test report](RENDER_BACKENDS.md). [Portable packaging](PACKAGING.md) is available from an SDK build.
- Skinning and morph evaluation run on the CPU and upload dynamic Atom buffers; GPU skinning is future work. GPU timings and performance improvements are not claimed.
- The viewport uses basic directional lighting and Atom's display mapper. Metallic assets can appear dark; colors differ from the earlier OpenGL renderer.
- MuJoCo tools are experimental animation aids. Estimated human rigs are not measured biomechanics. A replay, a plausible pose or a COM display alone does not certify physically valid motion. Solver failures and approximate results remain visible.
- `.jst` projects retain schema-8 compatibility and read schemas 1–8. Review imported humanoid roles, IK chains, scale and joint limits before editing or simulating.
- Optional tests need user-supplied character fixtures. Those assets and local credentials are excluded from the repository.
- The tested SDK can print JSON Serialization context warnings during shutdown; native Trace errors/asserts are treated as failures by the GUI/smoke runners.

Next priorities: profiling, GPU skinning, richer lighting, visual regression coverage and portable packaging.
