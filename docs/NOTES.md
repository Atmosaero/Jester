# Notes for animators and AI

Jester **0.2.0-alpha.1**. Existing schema-8 annotations and schemas 1-8 remain readable. Notes are authored metadata; free text never creates a physical constraint or a successful measurement.

## Write one thought and continue

1. Pause on the desired frame and select a bone/controller. Shift-select several bones for one note with several bindings; clear selection for a character-wide note. No animation key is required.
2. Press **Ctrl+Alt+N**, choose **Add note** from the viewport context menu, or click **+ Note** in the Notes dock. The draft captures the current document, selected bones and frame. An explicit nonempty Ctrl+Shift-drag time selection proposes an interval instead.
3. Type in the nonmodal card. **Enter** adds a line; **Ctrl+Enter** saves; **Escape** cancels. The first nonempty text line supplies the short name, and the complete plain text is retained. Animation shortcuts do not run while typing.
4. Continue animating. Creating, confirming an edit, deleting or completing a time drag is one Undo. Cancel creates none.

**Notes** beside the Timeline opens the dock; **View > Notes** toggles it. Search text and choose All notes, Selected bones or Current moment. Click a row to visit its start, select its valid related bones and open the card. A character-wide note clears bone selection rather than choosing an arbitrary joint. The current-moment filter refreshes when playback stops; ordinary list items are retained through playback.

## Markers and shared selection

Small speech markers appear beside bones at the current moment. Their addresses are bone IDs and seconds, not pixels. The first captured selected bone is the primary anchor; the card lists all bindings. Character notes use a separate C marker. Hover gives a short excerpt; clicking opens the same ID as the panel and Timeline.

Markers follow pose/camera changes, use Qt logical coordinates for DPI, avoid the active gizmo region and spread to avoid each other. They clamp to the viewport edges; offscreen/behind-camera/missing primary anchors are hidden. In a crowded view, notes without room remain available in the dock. **View > Note markers** toggles them. Playback hides markers and disables their hit testing.

Timeline context rows retain their bands and move/stretch interaction. A label click or a band click opens that note. Point markers move as a unit. Selecting in any of the three views shares the document's annotation ID and reveals the Timeline row. Double clicking is not required.

## Advanced fields and time

Expand **Advanced** for exact start/end seconds, provenance, type, named bone checkboxes, tags, structured expectations and explicit key attachment/detachment. Empty binding selection means the whole character. Missing bones remain named as missing until explicitly removed or rebound. An attachment is optional: a new note never attaches automatically merely because a key happens to exist. The attachment command uses the frame/layer captured when the card was opened; nonexistent keys give an inline error and leave the draft intact.

Seconds remain authoritative through FPS changes. Display and navigation use `floor(seconds * FPS + 0.5)`, with ties toward positive infinity. A point is active at its nearest frame. Intervals use half-open rounded frame bins `[round(start*FPS), round(end*FPS))`; an interval narrower than one bin occupies that bin. Exact seconds, exact point/interval status and display frames are all exposed to AI. Navigation to a note outside the clip clamps to the nearest clip frame and labels the note Outside clip; it never silently trims or deletes it.

Moving/stretching note bounds applies the same affine transform to expectation `targetKeys` as the existing retime operation. A point translated or expanded uses translation for its targets. Collapsing an interval with expectations is rejected rather than losing ordered target times. Explicit expectation edits supply their own times. Clip trim retains its established clipping policy; target curves remain in their original absolute seconds when clipping (this is not a time warp).

Ordinary text edits patch only changed fields. Unknown fields, expectations, provenance, tags, attachments and exact subframe times survive. Metadata Undo/Redo does not rewind the current pose or frame and does not invalidate ordinary motion/geometry caches. Context-dependent validation/retarget/event-alignment results still become stale through metadata revisions.

## Draft safety

A draft retains its original document, bone IDs and seconds when the playhead or selection changes. Switching tabs hides it and returning restores it. There is one draft/card per document. Save or cancel it before editing a second note in that document. Closing/replacing the source document discards its draft; drafts are not project data until saved. If the same note is changed/deleted through another view or MCP, save reports the conflict inline and preserves the draft instead of overwriting newer data. Cancel and reopen to review the newer version.

## MCP

`motion_annotations` and `motion_context` keep their existing operations. Compact records now include `description` (up to 256 UTF-16 code units), `textTruncated`, `textLength`, `nodes`, `nodeNames`, `bindingLabel`, exact `start`/`end`, integer `startFrame`/`endFrame`, `timeLabel`, `point`, `missingBindings`, `outsideClip`, and the existing ID/provenance/type/tags/expectation count. Truncation is explicit. Use `detail: "full"` or `motion_annotations` list with `id` for complete text. `motion_context.id` filters active and neighboring events to that annotation; ID lookup of an inactive note is best done with `motion_annotations`.

```json
{"operation":"list","id":"ANNOTATION_UUID"}
```

```json
{"seconds":0.7333333333333333,"nodes":[15],"detail":"compact"}
```

The first arguments go to `motion_annotations`, the second to `motion_context`. Query results are author context. Measurements and certified physical feasibility remain separate.

## Interface examples

Current Blue Jay captures with Oddie, recorded at 1440 × 900: [card at 100%](notes/card-1.png), [panel at 100%](notes/panel-1.png), [card at 150%](notes/card-1.5.png), [panel at 150%](notes/panel-1.5.png). These combine the actual Atom viewport with Qt overlays and include the Unreleased note-card contrast fix. The card scrolls when necessary; Save/Cancel remain visible.

The [Oddie animation plan](ODDIE_WAVE.md) describes the three phase notes and finger timing. See [Testing](TESTING.md) for repeatable UI checks; these documentation captures do not constitute a new regression-test matrix or certify physics.
