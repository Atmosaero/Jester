# Scene display

The viewport header and Scene menu share Lit, Unlit and Wireframe actions (`Alt+1/2/3` by default). Preferences can rebind them. These controls affect viewing; original meshes, materials and exported animation remain authored data.

- **Lit** uses material lighting, the studio IBL, directional shadows and enabled post effects. Neutral defaults use 1600 lux main / 400 lux fill with manual **-8 EV**, suitable for the physical light scale. There is no automatic exposure adaptation.
- **Unlit** shows base color/texture with UV transforms and alpha behavior. Authored emissive, lighting, environment brightness, exposure, grading and Bloom are bypassed. Display conversion remains sRGB.
- **Wireframe** draws cached triangle edges at their deformed positions. Line color and hidden-edge visibility are independent from the skeleton overlay. The portable fallback evaluates positions on CPU and draws native Atom AuxGeom lines on both RHIs. Faces provide occlusion with the background color; hidden edges bypass depth testing.

The Scene dock contains main/fill direction, color and lux, main-light shadow quality, studio environment brightness/rotation, solid/gradient background, grid/axes, a visual floor, perspective/orthographic camera settings and post processing. The installed Atom supports one directional shadow caster; fill shadows are explicitly unsupported.

The visual floor does not edit MuJoCo. **Follow MuJoCo ground height** is an explicit, one-way viewing link. Otherwise its height is independent.

Visible background uses a small native solid/gradient sky cubemap, separate from the studio images used for actual IBL. It draws before transparent geometry. Sky exposure compensates manual scene EV; the selected tonemapper and grading still affect its display. Missing base normals in older native meshes are derived once in the rendering cache for both deformation paths, preserving the authored file data.

Neutral, Studio and Silhouette presets are built in. Saved custom presets contain visual settings only. Scene edits support Undo/Redo; a slider drag is one undo command. They persist in `.jst` schema 9 without invalidating motion analysis or physics provenance.

Post processing provides manual EV, ACES/Reinhard/display-sRGB mapping, saturation, contrast and optional Bloom. The global switch bypasses these effects and retains display conversion. Native captures and frame sequences use the same pipeline. Qt text, notes and manipulators are composited afterward and retain their separate overlay switch.

MCP `screenshot` and `record_frames` accept `overlays: false` to capture the native scene without viewport annotations/manipulators. Their default is `true`; both use the same composition path. Framing in orthographic mode adjusts the authored view height, and the mouse wheel changes that height without editing motion.

MCP `scene_visuals` accepts `get`, `set`, `capabilities`, `preset` and `reset`. `set` applies a partial nested patch using the same validation and history as the dock:

```json
{"operation":"set","settings":{"mode":"unlit","background":{"mode":"solid","topColor":"#343840"}}}
```

Read `capabilities` before editing unsupported features. Read `get` afterward: `authored` contains the document, `applied.effective` queries Atom settings interfaces, and `renderer` reports the actual RHI and deformation path. GPU completion is asynchronous.

GPU skinning uses eight influences, native compute/output streams and morph targets. Per-influence inverse-transpose normals support inherited non-uniform scale/shear. A tagged 32-byte native morph payload preserves small TBN deltas with half precision; untagged SDK input keeps its original format. CPU evaluation remains available for unsupported input, allocation failure, explicit diagnostic requests and the wireframe fallback.

Diagnostics separate CPU skinning, palette calculation, vertex preparation, buffer update and model construction. RPI preparation callbacks are not display presentation. GPU times remain null when unmeasured. The numerical skinning diagnostic performs an explicit output-buffer readback; its cost is separate from playback measurements.
