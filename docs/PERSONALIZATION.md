# Editor preferences

Open **Preferences** (`Ctrl+,`). Search filters appearance, overlay colors, language, keyboard actions and layouts. Font size and interface scale are device-independent; Qt applies screen DPI separately. Themes include dark, light and high contrast with an adjustable accent.

**Preview** applies the draft. **Apply** saves it, **OK** saves and closes, and **Cancel** restores the last applied preferences. Scene data and history are separate from editor preferences. Layouts can be named, restored and deleted. Shortcuts use stable action IDs, include conflicts with unmodified defaults, and can be disabled or reset individually.

Preferences use versioned `JesterPreferences` JSON. Import validates before changing state; export excludes recent files, document paths, MCP endpoints and credentials. Custom visual presets are part of this portable model. Writable installations store preferences in `user/editor-preferences.json`; read-only installations use the per-user application configuration directory. `JESTER_PREFERENCES_FILE` selects an isolated location for tests.

Language options are System, English and Russian; an unsupported system language falls back to English. Qt `.ts` sources compile to `.qm` catalogs during the build. Portable packages include Jester's catalog and `qtbase_ru.qm` for standard Qt dialogs. Serialized values, tool names, action IDs and JSON numbers remain language-independent.

Language changes update existing menus, panels and owned message dialogs. Numeric drafts follow the selected decimal separator; unfinished text, authored names and note bodies remain editable. Font family/size, toolbar icons and custom painted timeline, curve and note surfaces follow the current appearance. Text entry consumes animation keys while retaining global file shortcuts.

Update and audit the Russian catalog with `python tools/update_translations.py` and `python tools/update_translations.py --check`. Review new entries before building; the audit checks unfinished entries, source coverage and placeholder integrity. It does not replace reviewing dynamic text and the running interface.

MCP `editor_preferences` supports `get`, `set` and `reset`. `set` applies a partial patch through the shared preference and action validation. `get` also reports the actual selected language, font and action bindings.
