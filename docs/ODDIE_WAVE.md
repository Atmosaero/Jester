# Oddie greeting

A new 4.4-second greeting, recorded in Jester's native Atom viewport with the blue Blue Jay interface. The source clip runs at 30 fps; the looping README GIF uses every second frame at 15 fps.

The supplied asset uses the name `Odie`; the preview and project are titled **Oddie · Hello**. Its existing skin weights and materials are preserved. A separate Blender working file supplies the FK clip; Jester imports it, stores the scene and bone-bound notes in `.jst`, and records the actual editor window. The original robotics-garden scene is unchanged. Character source files remain local.

## Notes that guide the motion

Three phase notes are stored in the Jester project, bound to `Hand_R` and the four finger-base bones. Their text describes intention, rather than physical constraints.

| Time / frames at 30 fps | Movement |
|---|---|
| 0–1.1 s / 0–33 | Raise the upper arm, then the forearm, and turn the palm toward the viewer. Open the fingers with a small relaxed bend. The thumb opens separately, clear of the palm. |
| 1.1–3.2 s / 33–96 | Three soft wrist waves, approximately ±18°. Fingers follow the wrist by 2–3 frames, with smaller motion at the middle and tip joints. Ease through reversals. |
| 3.2–4.4 s / 96–132 | Dampen the wave, lower the arm, and restore the loose resting curl. Fingertips settle last. The final pose matches the start. |

## Finger mechanics

This rig has a thumb (`Finger_1`) and three long fingers (`Finger_2` through `Finger_4`), each with three segments. The supplied rig notes identify **local Z** as the curl hinge; local X would fan the fingers sideways. Right-hand curl uses negative angles; the left hand uses positive angles.

The long fingers open from a loose 13°/18°/11° resting curl to roughly 4–8°/7°/4°. During the wave, small 3.5°/2°/1.2° ripples decrease toward the fingertips. Each following finger starts about half a frame later; each following segment opens about three quarters of a frame later. The thumb retains its separate, gentler bend. The body and feet remain stationary.

The GIF is a recorded animation, not a real-time performance measurement. The supplied model is not distributed with the repository.
