# Source modules

`App` is the application entry point. Internal headers and implementations live together under `Editor/src`; includes are qualified from that root.

| Module | Responsibility | Target |
|---|---|---|
| core/scene | Scene, document, workspace, pose editing and geometry data | JesterCore |
| core/animation | Evaluation, keys, curves, layers and retarget | JesterCore |
| core/analysis | Measurements, validation, comparisons and snapshot jobs | JesterCore |
| core/annotations | Annotation data, context and navigation | JesterCore |
| io/project | Native project persistence and recovery | JesterCore |
| io/import, io/export | FBX/glTF interchange | JesterCore |
| io/catalog | Synthetic examples and scene discovery | JesterCore |
| physics | MuJoCo, simulation jobs, settings, provenance and validation | JesterCore |
| render/atom | Native viewport, materials and mesh buffers | JesterAtom |
| ui/window | Window composition, docks, inspector and document tabs | JesterUI |
| ui/viewport, ui/timeline, ui/curves | Editing surfaces and input | JesterUI |
| ui/notes | Notes cards, panel and overlays | JesterUI |
| ui/motion, ui/physics | Motion/retarget and physics tools | JesterUI |
| ui/widgets | Shared icons and widgets | JesterUI |
| settings | Editor appearance, preferences and localization | JesterUI |
| automation | Editor API, MCP server and schemas | JesterUI |

JesterCore uses Qt value types and QObject but does not depend on Window or Atom. JesterAtom consumes core data and owns SDK graphics types. JesterUI composes these services. Project/animation behavior must remain usable without creating a Window. Machine-facing values are independent of UI language.

The initial mechanical move preserves serialization in the glTF implementation; the native-format work extracts it into io/project separately. Generated build identity remains in the CMake binary directory. Vendored dependencies stay under Editor/third_party.
