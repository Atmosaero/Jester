# Motion tools examples

Original procedural geometry and motion, distributed under the repository MIT
license. No Epic, HumanM or Street Fighter content is included.

| Project | Inspect |
|---|---|
| annotated-jump | Preparation/takeoff/flight/landing context tracks; editable pose bundles |
| curves-paths | Step/Linear/Bezier/Hermite/Bounce/Elastic/Back, local Euler two-turn motion and event labels |
| penetration-and-contact | Moving closed hand volume inside a closed torso; explicit mesh pair 0/1, with a short allowed-contact interval |
| retarget-target | Different arm/leg proportions with adjusted bind geometry |
| retarget-result | Separate editable transfer of annotated-jump; inspect its saved retarget profile/report |

Open them from the **Motion tools** category in the scene chooser. For transfer,
open annotated-jump first and retarget-target second, then Timeline > Motion
tools > Humanoid retarget. Review mapping and calibration, calculate the preview,
then create an independent result. Analysis never silently repairs animation.

Regenerate with `TricksterMotionDemos examples/motion-tools`. The generator
reopens each project and checks context/curve persistence. Development-only GLB,
FBX and exact evaluator references go to `artifacts/motion-independent-input`.
See [motion analysis guide](../../docs/MOTION_ANALYSIS.md).
