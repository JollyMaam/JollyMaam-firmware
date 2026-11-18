# UI strings

`en.json` is the editable English source: a flat object of stable keys and
strings. There are no review states or translator metadata. Edit values, keep
keys. `{count}` and other named parameters are substituted at runtime; keep
their names when rewriting or translating text.

`settings.json` maps `kind:id` from the firmware descriptors to a catalog
prefix such as `settings.slot.radio`. Under it, `.label`, `.help`, and
`.options.bluetooth` hold presentation text. Firmware names and enum tokens
remain unchanged, including the values of HTML options sent to the controller.

From the repository root:

```
make strings-report
make strings-check
make strings-sync
```

Open `build/strings.html` directly in a browser. Search and edit the English,
then download `en.json` and replace this directory's file. The report is a
standalone snapshot; regenerate it after edits. Its source audit also shows
string-bearing lines from the web page, Android code/resources and native
library, including technical tokens and logs. It is intentionally broader
than the translatable catalog.

`strings-sync` uses a host-compiled C exporter of the complete field tables,
without board-specific filtering. It adds missing setting entries and rebuilds
the ID mapping; it preserves existing English edits and obsolete entries. New help entries start
empty and are written in the catalog; help is never imported from firmware.
`strings-check` detects missing settings, changed mappings, duplicate keys,
unknown translation keys, non-string values, and mismatched parameters.
The rule count in help is a parameter calculated from the connected device's
field size, rather than the exporter's build-time limit.

To add a language, copy `en.json` to a locale file such as `de.json`, translate
values, and run the checks. Partial dictionaries are allowed. Open the page
with `?lang=de` to select it; otherwise it uses the browser/Android WebView
language. A regional locale tries its base language, then English. Missing
entries fall back to English; unknown fields and enum choices fall back to
firmware text. The APK bundles the JSON files alongside the page.

The current catalog covers settings and the main page/editor/firmware controls.
Dynamic messages, native errors, parser diagnostics and some generated labels
still have literal text in their sources; use the audit view to inspect these
when extending the catalog. This is an inventory and initial UI integration,
not complete multilingual coverage. Firmware descriptors retain an empty legacy
help slot for older clients; names and enum tokens are still sent. The web UI
reads help from this catalog (or from older firmware for unknown fields).
CLI translation is outside this catalog's scope.
