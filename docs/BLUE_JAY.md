# O3DE Blue Jay / Jester

Jester uses O3DE's native AzQtComponents style and embedded widget configuration. The SDK remains unchanged. A small Jester layer recolors interaction states and adapts fonts, density and UI scale; native controls keep the SDK's behavior.

## Sources

- [Blue Jay tools UI guide](https://www.docs.o3de.org/docs/tools-ui/).
- [Component development and the StyleManager chain](https://www.docs.o3de.org/docs/tools-ui/uidev-component-development-guidelines/).
- [Tabs](https://www.docs.o3de.org/docs/tools-ui/component-library/uidev-tab-component/) and [styled docks](https://www.docs.o3de.org/docs/tools-ui/component-library/uidev-styled-dock-component/).
- [Semantic status colors](https://www.docs.o3de.org/docs/tools-ui/ux-patterns/error/).
- Installed SDK headers and its embedded BaseStyleSheet / widget Config.ini resources; upstream references [BaseStyleSheet.qss](https://github.com/o3de/o3de/blob/development/Code/Framework/AzQtComponents/AzQtComponents/Components/Widgets/BaseStyleSheet.qss) and [PushButtonConfig.ini](https://github.com/o3de/o3de/blob/development/Code/Framework/AzQtComponents/AzQtComponents/Components/Widgets/PushButtonConfig.ini).

## Color roles

| Role | Color |
|---|---|
| Blue Jay pane surface | #444444 |
| Toolbars / tab surfaces | #333333 |
| Input / canvas surfaces | #222222 |
| Separators | #111111 |
| Text / muted text | #FFFFFF, #CCCCCC / #999999 |
| Blue Jay interaction accent, links, information | #1E70EB |
| Jester red: logo and scene-tab close square | #D93644 |
| Error / warning / success | #FA2727 / #FFAA22 / #58BC61 |

The blue is the Blue Jay UI color, rather than a sampled pixel from the supplied logo or O3DE's brand logo. The active timeline, selection and authored curve follow this accent. The logo retains its blue/red identity, and scene tabs close with a red square and white cross. Active MCP status uses the success green #58BC61. RGB axes retain their semantic roles.

Light/high-contrast preferences, custom fonts/scales/colors, layout state, shortcuts and live English/Russian switching remain available. One-time local migrations update the former factory amber/red accent and selection colors; other custom colors and later explicit choices remain editable. Project visuals and animation data are separate from UI styling. This interface ships in 0.2.0-alpha.1.

The viewport header uses sun, solid-cube and wire-cube icons for Lit, Unlit and Wireframe. Translated tooltips, accessible names and Alt+1/2/3 shortcuts remain available. Loop is an icon toggle with its existing checkbox/API identity. Translation axes use flat shafts and filled, pointed arrowheads; rotation rings and transform behavior retain their existing controls.

## Logo

[The SVG](../Editor/resources/branding/jester.svg) is the canonical transparent vector drawing of the supplied angular jester mark. Qt uses it for crisp window/UI rendering. [The ICO](../Editor/resources/branding/jester.ico) includes 16, 20, 24, 32, 48, 64, 128 and 256 pixel variants and is embedded into the Windows executable for Explorer/taskbar use.

The checked-in ICO is reproducible with `python tools/generate_branding_icon.py` (Pillow is a development-only dependency). Neither the build nor the installed editor needs Python/Pillow. Interactive checkbox/radio/toggle SVG variants are recolored from the SDK's embedded resources into an application-owned temporary directory; installed builds do not read the developer's SDK source tree. Other widget styling comes from the SDK stylesheet plus Jester's embedded overrides. The original StyleManager-owned style remains alive, including during live preference previews.

## Native window integration

The SDK supplies Blue Jay window/dialog title bars. Quit closes the decorated frame, so the editor validates unsaved scenes once. Automation resolves the frame's dialog guest for accept/reject operations and uses the decorated window origin when composing modal screenshots.

Window position/size is saved for the decorated frame under a separate local settings key; existing content geometry remains available for initial migration. The dock workspace retains its explicit minimum and compact-layout behavior when the SDK attaches it as a guest. SDK text-field search/error/clear actions are excluded from customizable editor commands, so live style polish does not change command IDs.

Explicit dialog sizes survive SDK attachment, are bounded to the available screen and are centered on the owner. Standard dialog buttons retain usable minimum dimensions. Preferences Cancel restores the frame's geometry along with the previous preferences/layout. Valid text fields do not acquire a false error indicator during style changes.

Menu items retain the SDK's 28 px height: adding vertical padding makes Qt classify the items as overflow, even in a wide window. The regression check verifies actual rendered labels and opens the File popup; checking action visibility alone would miss this failure.
