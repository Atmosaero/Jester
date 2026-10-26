# Motion editing

Current release: **0.1.0-alpha.1**. Use `Jester --build-info` for the source Git SHA. Project schema 9 reads schemas 1-9; an old constant correction becomes one equivalent layer, and legacy transform keys retain all TRS components.

## Main workflow

1. Open FBX/glTF or Scene → Choose scene. Three synthetic MIT-licensed demos are available as templates: walk, jump and hand. Start with an empty scene by default.
2. Select the source clip. Another clip opens a separate tab. Review Profile: units/scale, humanoid roles, IK ancestors/poles and mirror pairs. Suggestions are editable; saved bindings use node IDs.
3. In Motion, select or create a layer. Edit the combo text to rename it; adjacent buttons duplicate/reorder it. Layer enables only the selected layer; Original shows the uncorrected source before added layers, contacts and limits. Record locks/unlocks pose authoring. Active document/layer/recording mode also appear in the status bar.
4. Envelope expands interval/fades/strength controls. New layer uses that interval and seeks full influence. W moves/solves IK, E rotates; inspector and viewport share transactions. I/Key and Neutral Key follow the selected timeline row: one channel, one bone, an explicitly listed controller chain/group, or the whole-layer bundle. Neutral key returns existing channels and the selected FK/IK chain to zero offsets. Set neutral keys at the desired boundaries, then change the wrist/body in between.
5. Green timeline diamonds are keys in the selected layer; source events remain read-only. Selected only filters bone/controller tracks; expand a row for Position/Rotation/Scale. A controller row groups the named IK root/mid/end. The layer summary is explicitly a whole-layer bundle; choose a bone/channel row to preserve other tracks. Drag moves a key, Ctrl+drag copies; Ctrl+C/V copies/pastes, Delete removes, comma/period navigate. Escape cancels an unfinished gesture. Ordinary full-pose keys remain available when there are no correction layers/native source channels.
6. Contacts opens explicit effector targets, position/orientation, dates and fades. Capture current takes the evaluated target; On ground uses Physics Ground Y. Add creates a non-overlapping interval, Update modifies the selected one, Remove deletes it. Check reports half-frame holding errors in mm/degrees and reach/limit conflicts. Fade transitions intentionally release support.
7. Compare, Space to play, Ctrl+S to save, close/reopen, then Ctrl+Shift+E to export. Bake copy samples the enabled result into a separate document at the chosen FPS. The original tab/source times remain intact. Bake/FPS stays in a fixed Motion footer. A trim boundary rounds to the nearest output frame (at most half an output frame); source data is never retimed.

## Layer semantics

Up to 32 layers compose in displayed order. Each has an interval, strength and smoothstep entrance/exit. Weights multiply the offsets. Multiple intervals may overlap. Transform tracks are sparse per node and independently keyed component; morph tracks are sparse per node: an unedited bone keeps following the source. Translation/scale/morph offsets interpolate linearly; quaternion offsets take the shortest path. Inside the envelope, a channel holds its first/last key value outside that channel's keyed times. Outside the layer interval it contributes nothing. Use neutral keys for an explicit return to the source.

Local offsets add parent-space translation, postmultiply local rotation and add local scale differences. World offsets add scene-space translation and premultiply scene-space rotation through the parent corrected at that layer. A world offset follows the moving source; use a contact for an absolute support target. Other layers are explicitly inverted when recording a selected layer so they are not baked into it. Pose edits affect changed nodes; IK writes its affected chain. Limits/pins/contacts are solved after composition. A constraint conflict can prevent the requested endpoint; inspect the residual rather than disabling limits implicitly.

Large rotation edits at partial influence can be ambiguous beyond 180° of stored correction. The editor rejects that case: use Strength 1 outside fades, or add another layer. Persistent pin overrides remain layer-level compatibility data; use timed contacts for support appearance/release. Use the native [Graph Editor](MOTION_ANALYSIS.md) for authored curve/tangent editing.

Original comparison bypasses added layers/contacts/limits, while layer disable only bypasses that layer. Original comparison and layer selection are view state and do not dirty the document. Export/bake always use enabled corrections. Explicit source-clip/pose-key editing does not overwrite original native curves. FBX source curves/layers are sampled at their reported rate, while glTF retains source seconds and LINEAR/STEP/CUBICSPLINE channels. Changing output FPS samples the shared evaluator.

## Profiles, limits and physical tools

Profiles assign root/bend/end ancestors with intermediate helpers, poles and mirrored IDs. Mirroring reflects rest-relative world motion and converts through each bone's own local axes. IK diagnoses degenerate chains and nonpositive/nonuniform ancestors above the bend. An unreachable target clamps to reach; contacts additionally report the remaining error. Hinge and swing/twist limits share local rest axes with the MuJoCo model; three-hinge swing approximations are diagnosed separately.

Physics offers Passive and Follow animation with editable mass distribution/capsules, units, gravity, friction, contacts, limits and PD stiffness/damping/torque caps. The target is evaluated each substep. Drives use bounded internal equal/opposite joint torques, with no root/world stabilization when no explicit anchor exists. Full-body synthetic checks cover target changes, contact release, free flight, cancellation and recalculation. Preview, compare and apply as a new document. Calculations are background snapshots with progress and cancellation; a changed/closed source cannot receive a stale result. Returning to its tab restores a ready preview.

This remains an experimental capsule/PD assistant: it does not guarantee balance or preserve the target exactly. In the recorded free-flight 21-body test, Follow had up to ~40.5° / 410 mm target deviation while joint/COM consistency held. The ballistic tool matches endpoint positions, not arbitrary boundary velocities. The newer MuJoCo whole-body, flight and bounded dynamics tools have distinct scopes and open certification gates; see [PHYSICS.md](PHYSICS.md) and [PHYSICS_RELIABILITY.md](PHYSICS_RELIABILITY.md) for their current measured status. No general automatic balance/contact planner is provided.

## Save and recovery

Each tab owns its document/history/selection/time/revision/dirty flag. Save is atomic. Normal close prompts to save/discard/cancel. The desktop app writes separate autosaves every 30 seconds for dirty committed documents. Autosave skips an unfinished gesture and never overwrites an original. On restart, a recovery notice leads to File → Recover autosave. Recovery creates a dirty new tab with no original output path; use Save As. Save/discard removes its recovery snapshot. The default folder is the platform's Trickster application-data `recovery` folder; `TRICKSTER_RECOVERY_DIR` can redirect it.

See [DEMOS.md](DEMOS.md), [TESTING.md](TESTING.md) for reproduced workflows, evidence and limitations.

## Symmetry and recording scope

Use **Shift+M / Symmetry X** for live paired edits; both sides undo together. **Ctrl+M / Flip entire pose across X** swaps the whole pose. Inspector buttons select the opposite bone, copy the selected FK bone/IK chain to it, or open its pair settings. Purple outlines identify the partner; an unassigned side bone is reported rather than self-reflected. See [Editing controls](#editing-controls) for selection and key scope details.

## Editing controls

Multiple viewport bones use Shift+click or rectangle selection; the Outliner uses Ctrl/Shift. Drag a selected controller to transform the group, with one Undo; P pins/releases selected effectors. Select timeline diamonds with Shift+click/Shift+rectangle, then move/copy/delete or paste relative times atomically. Bone/channel insertion automatically creates a correction for raw pose-key scenes, while their explicit whole-pose summary retains bundled editing. Live symmetry preserves unedited opposite TRS components. Numeric FK fields now follow the gizmo Local/World mode, with numeric editing limited to a single bone. See [Editing controls](#editing-controls) for channel and group behavior.


## Motion authoring and analysis

Timeline > Motion tools opens the native Qt Graph Editor, Motion Paths /
measurements and humanoid retarget workflow. Context tracks preserve intentions,
observations and hypotheses separately from measurements. Schema 8 keeps curves,
layer/controller channels, context UUIDs/key attachments and reusable retarget
profiles. Existing schemas retain their original motion semantics. Follow
[MOTION_ANALYSIS.md](MOTION_ANALYSIS.md) for the full route, keyboard
controls, independent export checks and current limitations.


## Quick notes

Pause, select a foot and press **Ctrl+Alt+N**. Write the note in the nonmodal card,
then **Ctrl+Enter** to save and continue animating. The editor captures document,
bones and time without adding an animation key. **Notes** beside Timeline opens
the searchable dock; viewport speech markers and Timeline rows open the same
note. See [NOTES.md](NOTES.md) for intervals, advanced data, drafts, MCP and
actual 1280 x 720 captures at 100%/150%.

Group rotations turn selected roots about their individual origins. Shared-pivot mode and arbitrary persistent user track groups are not implemented. Component retiming preserves unrelated channels/morphs and rejects collisions atomically. The whole-layer summary edits bundles; expanded bone/component rows provide narrower scopes. Wheel scrolls tracks, Alt+wheel pans time, Ctrl+wheel zooms and Home fits the range.
