# Curves, motion analysis and retargeting

These native Qt tools share Timeline time, selection and the editor evaluator. Measurements are kinematic; they do not certify dynamics.

## Workflow

1. Choose **Motion tools / annotated-jump** from the scene chooser. Context
   intervals appear below the pose/layer tracks. Ctrl+Shift+drag selects a shared
   time interval. Scrub or play; selection and the playhead stay shared.
2. Open **Notes**, or select a bone and press **Ctrl+Alt+N** for a nonmodal
   text card at the captured frame. The dock, viewport markers and Timeline rows
   share annotation IDs. Advanced fields retain provenance, expectations and
   explicit bundle attachments. Move/stretch on Timeline, Undo and save/reopen;
   FPS changes keep seconds. See [NOTES.md](NOTES.md) for current interaction,
   subframe rules and draft safety.
3. Open **Timeline > Motion tools > Graph Editor**. Choose a bone, source override
   or layer, path and component. Create a channel; I inserts at the playhead.
   Drag keys/tangents, Shift-box-select, Ctrl-click, Ctrl+A/C/V/Delete, pan with
   middle/right drag and zoom with the wheel. Numeric table edits and bulk
   interpolation are undoable. Checked compatible channels overlay the active
   channel. Right-click selected legacy Timeline keys to author channel
   interpolation overrides without replacing the source bundles. FK/IK posing
   over authored source curves uses a correction layer; posing over a curve-driven
   layer records above it. Source curves and Original view remain intact. Animate
   time-varying IK goals explicitly with ikTarget/ikPole/ikWeight curves; ordinary
   correction-layer pin settings retain their existing interval-wide semantics.
4. Open a native glTF clip and use Graph Editor's **Native source** tab to inspect
   original LINEAR/STEP/CUBICSPLINE values and derivatives. Copy a channel to an
   authored override. Scalar cubic conversion preserves its polynomial; cubic
   quaternion conversion explicitly samples SLERP with a 0.02-degree default
   quarter-point tolerance. Native channels remain intact for Original view.
5. Open **Motion Paths / measurements**. Choose several bones, origin/end/local
   offset, world/parent/root coordinates, scene units/metres, range and step.
   Calculate, cancel, update or clear. Numbers use a virtual Qt table; the
   computed graph is read-only. JSON and CSV retain units and undefined values.
   Paths are cached; world metre values are converted for display. Root/parent
   coordinates are drawn in the corresponding frame fixed at the analysis start,
   explicitly labeled in the viewport/report. Choose labels, speed colours, a past/future window and
   automatic updates. Ctrl+click offers nearby paths/times alongside object hits;
   ambiguous hits use an explicit menu. Double-click a numeric row to seek.
6. Use **Validate motion** with the penetration-and-contact example and explicit
   mesh pair 0/1. Calculate approximate proxies and exact skinned surface checks.
   The Problems tab filters and navigates intervals; clicking seeks/highlights
   the pair. Enable transparent mesh or approximate volumes. Timeline bands and
   geometric witnesses reference the same problem IDs. The authored permitted
   interval stays distinct from confirmed/approximate/uncertain detections.
7. Open **retarget-target** as a second document. Humanoid retarget uses the
   active document as target. Review roles (custom rows through the table menu),
   per-pair orientation weight/IK, local A/T calibration, common up/forward basis,
   root policy, local sole points and bounded pelvis reach adjustment. Calculate
   asynchronously, scrub Source/result side by side, create an independent
   editable document, save, export and Undo its edits. The source is unchanged.
8. Compare the source and result in Motion analysis: time, duration phase or
   paired context events, explicit offset/root/height normalization. Review bone
   pairs and calibrated axes/reference rotations. Inspect position, SO(3),
   physical-time velocity, swing/twist, cycle closure and bounded lag estimates.
   Original times remain visible; no timing warp is applied to the clips.


## Evaluation and limitations

The order is source → authored source curves → ordered corrections (including
layer curves) → contacts/IK/limits. Original view bypasses authored corrections.
Source scalar TRS channels are absolute local values; layer TRS channels are
additive offsets. IK target/pole channels are world points and weight clamps to
[0,1]. Euler uses Qt's explicit YXZ convention. Quaternion sign equivalence is
handled with SO(3) distance; angular tangents are local rad/s.

Trajectory derivatives use independent sample stencils; moving-reference
coordinates are differentiated as coordinates, not rotated world velocities.
Step discontinuities are not bridged in paths. Background results carry source
UUID/revisions, progress, bounded memory/time, cancellation and partial status.
Reading never seeks the document or cancels an unfinished gesture. Retarget,
validation and event alignment have the context dependencies their results need.

Adaptive export checks quarter points plus a near-end sample against transforms,
morphs and skinned vertices. Defaults: 0.5 mm position/vertex, 0.1 degree rotation,
1e-4 scale/morph. These are sampled tolerances, not a mathematical continuous
bound. Euler turns seed additional samples. Step-containing glTF channels use
STEP samplers with refined continuous portions, avoiding a hidden linear ramp.
FBX writes constant interpolation flags. Frequency/memory/precision exhaustion
returns an error rather than silently reducing accuracy.

Retarget preserves target lengths/inverse binds and unmapped reference bones.
Additional spine/twist/finger bones can be mapped manually, but automatic twist
or spine redistribution is not implemented. Source-space arbitrary expectations
are retained as provenance and marked for recapture; mapped sole contacts become
new target-space expectations with measured error. In-place uses a documented
moving-ground reference. Sole-point penetration is not full foot-mesh depth.
The automatic target check is bounded; full surface/range validation is explicit.
Results remain approximate when feasibility is not established.

Surface self checks exclude welded triangle adjacency and report the first
crossing/nested closed-component pair per mesh. Open, inconsistently oriented or
self-intersecting containment inputs have stated limitations. Anatomical domain
checks use the editor's directed rest-local axes, before projection can hide a
violation. No diagnostic automatically changes the animation.


## Anatomical interpretation

Directed joint-domain checks require authored rest-local axes and limits. A humanoid role/IK profile does not supply validated anatomical ranges. Without configured limits the checker cannot label knee/elbow hyperextension or wrong-axis wrist rotation as an anatomical error. Compare-mode swing/twist values are calibrated differences against another animation, not a validity verdict.

Proxy overlaps are approximate candidates; compare the neutral pose and inspect surfaces before treating them as faults. Skin, clothing and permitted contacts can intersect without proving impossible joint motion. Surface self checks report the first hit per mesh, not an exhaustive penetration list. Sampled speed/acceleration peaks have no automatic naturalness classification. Diagnostics never change animation automatically.

Try the [synthetic projects](../examples/motion-tools/README.md). See [Motion editing](MOTION.md), [Notes](NOTES.md), [MCP](MCP.md) and [Testing](TESTING.md).
