# Physics reliability

The native suites check solver diagnostics, proxy consistency, momentum, contact and boundary metrics, provenance, saved controls, replay and timestep refinement. Run them with the [test runner](TESTING.md). Included synthetic source/result projects are regression fixtures; an approximate result is not a certified motion.

Independent Godot trajectory capture is optional and not part of this alpha's self-contained build. The `independentImportedDynamics` test skips when its external trajectory files are absent. Historical experiments and external capture scripts belong to the [Trickster baseline](https://github.com/Atmosaero/Trickster/blob/main/docs/PHYSICS_RELIABILITY.md); their measured results concern that recorded environment.

The [baseline acceptance definitions](https://github.com/Atmosaero/Trickster/blob/main/docs/PHYSICS_ACCEPTANCE.md) explain the inherited physics gates. Jester retains separate native-control, baked-kinematic, baked-dynamic, represented-impulse, boundary and external-validation results. See [physics workflow](PHYSICS.md) and [alpha limits](LIMITATIONS.md).
