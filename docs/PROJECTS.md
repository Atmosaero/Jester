# Jester projects

The native project format is **`.jst`** (also `.JST`). Use Open, Save or Save As; the same format works from the command line, drag-and-drop, the scene catalog and MCP. FBX, GLB and glTF remain interchange formats.

```powershell
./launch.ps1 -File './examples/welcome.jst'
./build/bin/profile/Jester.exe './My projects/character.jst'
```

Projects store the complete supported scene, animation, curves, layers, contacts, notes and scene visuals. Schema 9 adds visual settings and reads previous schemas 1-8. They do not require the original imported character file. Save As adds `.jst` to a filename without an extension; unsupported or repeated extensions are rejected. The previous project extension is unsupported.

Autosave uses regular `.recovery.jst` projects with recovery metadata. Recovery opens a new unsaved document and preserves the original project. Save As selects its destination explicitly. Recent-file and catalog entries use the same shared file classifier.

Project schema versions describe stored data independently of the product version. Internal historical serialization identifiers are retained; they are not additional supported file extensions.
