# Physics examples

These synthetic mannequins, clips and projects are original Trickster fixtures under the repository MIT license. No user HumanM meshes or external character files are included here.

Each `*-source.jst` has a matching `*-result.jst`:

- `whole-body`: two planted feet and a reachable hand; the native tests separately check an unreachable target.
- `com`: a horizontal COM target while preserving supports; static posing only.
- `jump`: deliberately corrupted COM corrected to analytical free flight; incompatible endpoint velocity has a separate negative test.
- `tuck`: arms/legs tuck and spread during flight, changing rotation rate while preserving angular momentum.
- `squat`: one second, fixed support schedule and important poses, with controlled root wobble.
- `landing`: starts airborne, transitions into support and requests rest by one second.

`legacy-v4.jst` exercises schema migration without losing authored keys/layers/pins/contacts. Supported dynamics use an explicit seven-body/20-DOF model with the stationary upper body lumped into the pelvis. Their result status remains **approximate** until the baked inverse-dynamics gate is met; a native control replay alone is insufficient. Budgets, model/input hashes, contact schedule and metrics persist in each result. Native replay data are part of project JSON, without an MJB/cache dependency.

`JesterMuJoCoTests` regenerates these owned fixtures. See the [physics guide](../../docs/PHYSICS.md) and [reliability notes](../../docs/PHYSICS_RELIABILITY.md).
