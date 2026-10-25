# Portable MVP demonstrations

All three projects contain the original synthetic Trickster mannequin, source animation, reviewed humanoid profile, layers, morph data and contacts. They need no external model or engine project. The data is original and distributed under the repository MIT license.

Open from Scene → Choose scene → Templates, or open the files directly:

| Project | What to inspect | Reproduce |
|---|---|---|
| [Walk](../examples/walk-correction.trickster) | Foot planting plus independent upper-body/breath layer | Frame 12: Contacts → select a foot interval → Check. Original shows the sliding source without added contacts; switch it off for the corrected result. Frame 30: disable only Upper body and breath to compare that layer. |
| [Jump](../examples/jump-correction.trickster) | Explicit takeoff/landing support and unanchored flight | Contacts hold frames 0–10 and 50–60; frame 30 is in flight with no contact weight. Inspect Flight shaping and Arms at landing separately. Physics Passive/Follow is a separate approximate preview, not the author of this jump. |
| [Hand](../examples/hand-correction.trickster) | Wrist IK changing over five keys | Scrub 0/15/30/45/60. Green diamonds belong to Keyed wrist IK. Select LeftHand, pose at an intermediate frame, copy/drag a correction key, undo, and toggle Original. Neutral key returns the chain to the source. |

For each: open → review Profile → select a layer and correct FK/IK → compare → Ctrl+S (use Save As to preserve the example) → close → open saved project → Bake copy at **48 FPS** → Ctrl+Shift+E to GLB or FBX. Baking creates another document, leaving source clips/layers editable. Source sample events are read-only; ordinary pose keys are available in the baked document.

The automatic Qt workflow performs these steps with real mouse/keyboard events, including contact Update/Check, a timeline copy gesture and export. It records bone matrices and deformed points for independent Godot checks at five times. Optional user HumanM verification is separate and is skipped when its fixture is missing.

Godot precision check: use GLTFDocument/FBXDocument with tracks retained, compression disabled and bake FPS matching 48. Default FBX editor import at 30 FPS introduced about 1.5 mm error on this 48 FPS export; explicit 48 FPS passed the 1 mm tolerance without changing the file. These import settings are part of the check and should be reproduced when comparing numbers.
