# Web page

Pad settings in the browser over Web Bluetooth or WebHID: a static page
(plain ES modules, no build), for GitHub Pages and the Android app's
WebView (`android/`).

- `link-web.js`: the Bluetooth link, one GATT connection to a pad in
  config mode or its updater.
- `link-hid.js`: the USB link, the pad's tool device (16c0:05df, page
  0xffc2) over WebHID, in its app (any state) or its updater; several
  programs (padtool, other pages) share it.
- `frames-hid.js`: the tool device's framing (feature report in, 64-byte
  input reports out, per client and tag).

Links are raw: a Bluetooth link writes frames and hands up notifications
(`write`, `onReply`), a HID link sends feature reports and hands up input
reports (`kind: "hid"`, `reportSize`, `sendFeature`, `onInput`); both have
`onDisconnect` and `close`, and their module `choose()` and `reconnect()`.
The framing stays in JS, so the Android app supplies only raw links: GATT
over its own Bluetooth, and the tool device over its USB host API (WebView
has neither Web Bluetooth nor WebHID).

- `link-android.js`: those links in the app (`window.JollyMaamAndroid`,
  `android/app/src/main/java/dev/jollymaam/app/Bridge.kt`), and its
  flasher; `android` is null in a browser.
- `flash.js`: in the app only (libjollymaam over JNI, the app's Bluetooth
  scan): the device list (USB, and Bluetooth with its switch on) and a
  device's page: install, back up, upgrade and restore, through the vendor
  loaders a browser cannot reach (the Jieli ROM loader is USB mass storage).

The page starts at home: **Load Package** and its **i** (the package's
contents, and **Unload**), then in a browser a choice of link, in the app
the devices. A device (the pad chosen in a browser, a device tapped in the
app) has its page: what it is, and **Configure**, **Install**, **Upgrade**,
**Backup** and **Restore**, each on when the device and the package allow it.
In a browser only Configure and Upgrade (the updater over the link) work; the
link stays up from the device page down, so Back from the settings keeps it.
Each screen is a history entry, so Back and the browser's back go back a
screen. A `firmware.zip` next to the page (the app has one) is the firmware
package until another is loaded. The link tests (`linktest.js`) show only
with `?dev` in the page's address.

- `pad.js`: the Bluetooth framing (`src/profiles/profile_config.c`) and
  the tool commands (`src/core/usb.c`) over either link.
- `app.js`, `index.html`, `style.css`: the page. Edits every setting but
  address, bond and calibration, which stay read only.
- `editors.js`: the remap slot's keys (what each sends, its rules, presets,
  preset files) and the stick curves (presets, a graph to drag); both write
  the padtool text of their fields, which the settings table stages and saves.
- `remap.js`: `tools/padtool/padremap.py` and `padcurves.py` in JS, same
  text and bytes (tested against the vectors the Python writes,
  `test/generate_vectors.py`).

Web Bluetooth and WebHID need Chrome or Edge, and a secure context (https,
or localhost). On Linux, Chrome opens the pad's `/dev/hidrawN`, so the
user needs read and write access to it, as padtool does. To serve it from a checkout:

```sh
python3 -m http.server -d web 8000     # then http://localhost:8000
```

`make web-test` runs `pad.js` against a simulated pad (`test/`), in node.

Each macro card has its own wrapped text box for steps, with its name in
its own field. Buttons update the step text; typing edits the same steps.
New macros start empty, and deleting the last step keeps the empty card.
Empty cards are local drafts and do not count as changes to save. Invalid
step text must be corrected before saving.
**Import from quick settings** replaces that card's steps with the completed
QS recording, retained when entering configuration mode. **Save** writes the
edited macros. Add a remap rule to choose how a saved macro plays (once,
while held, or looping). A full 198-byte recording fills an empty
200-byte macro field and leaves no space for a name.

## String catalog

The editable English and setting ID mapping are in `strings/`; see
`strings/README.md`. `make strings-report` produces a searchable, editable
`build/strings.html`, including a source audit for remaining strings.
`start.js` loads the bundled catalog before importing the page.

## Workflow and message review

For reviewing wording and UI behavior, use **`review.html`** as the entry point.
With the HTTP command below, open `http://localhost:8000/review.html`.

Select **Pure web UI** or **Android app** in the header, then choose a workflow
on the left and click a state in the map. Navigation is filtered to that
platform. Shared workflows are labelled **Shared** and preview their actual
browser or Android rendering; platform-only workflows are labelled explicitly. The production
UI replays against fresh mock devices to that checkpoint. Labelled branches
let you jump directly to success, refusal, confirmation, progress, or recovery
states. There is no need to step through every preceding screen yourself.
The previous scenario/tour interface remains available through “Open full mock
UI” and `?mock=normal`.

The review page includes connection, settings, firmware actions, browser updates,
package loading/unloading, remap presets and macro editing. There are 50 shared
state shapes rather than a separate copy for each action and error code.

Android USB firmware operations use one **Firmware actions** graph. Its
**Action** selector changes between Install, Backup, Upgrade and Restore while
keeping shared states in one place. The graph hides inapplicable states:
Upgrade has no manual-loader state, Backup/Upgrade have no write-confirmation
state, and a missing matching install/update image is not a Backup/Restore
blocking state. Upgrade requires a device already running our firmware.

All libjollymaam updater error codes converge on the Android operation-failure
presentation. Browser update failures remain separate because the JavaScript
updater and UI have different messages and recovery behavior. Browser updater
codes and setting refusal codes are **Error detail** variants of one failure
node each, not separate graph nodes. Select a detail to inspect its exact text.
The right pane reads messages from the rendered UI, with catalog keys or source
locations where available. Native alert/confirmation text is displayed in an
inspectable mock box; accepting or cancelling is a branch in the map.

Click **Collect all messages** to replay the applicable state shapes for the
selected platform and action in a separate mock frame. Error details remain
selectable within their shared node; collection does not expand them back into
duplicate states. Search and the Errors / Successes / Confirmations filter then use the
actual collected text as well as state titles. Collection has a stop control;
preview failures are counted explicitly. The English catalog is separately
searchable, including strings not associated with a mapped state.

Each state has a stable ID and shareable URL, such as
`review.html?state=settings.refused&platform=browser&variant=3&board=c2`.
For shared Android operations, `operation=upgrade` records the selected action.
Older install/backup/upgrade/restore and per-code diagnostic links still resolve
to the corresponding shared state. Write a requested change in the
notes box and use **Copy state reference** to paste the ID, messages and request
into a conversation. **Export review notes** downloads the collected messages
and notes as JSON. Notes are kept in this page until copied or exported; they
are not saved automatically.

This is a map of implemented UI workflows, not an exhaustive list of every
possible native/backend diagnostic or OS-owned dialog. Unmapped messages can
be inspected through the string report/source audit. The previews share the
real rendering, validation, dialogs, package reader and command clients;
simulated device errors are identified by their fixture details. Each node
replays its own fixture rather than retaining arbitrary edits from another node.
No hardware transports are imported, and no real device writes occur.

## Mock UI without hardware

Serve the repository's `web/` directory over HTTP, then open
`http://localhost:8000/?mock=normal`. For example, from the repository root:

```
python3 -m http.server 8000 --bind 127.0.0.1 --directory web
```

The **guided tour** at the top describes the current state. Click **Next** to
perform the next action and stop at the next state. **Play** advances every
three seconds; **Pause** stops automatic advancement. **Previous** reloads and
replays with fresh mock devices to the previous checkpoint. **Restart** starts
from Home. Loading, saving and app-operation progress are deliberately held so
you can inspect them for as long as needed. The tour does not start playing
automatically.

For example, the default settings tour walks through Home, Loading, Settings,
Help, Invalid edit, Unsaved edit, Saving, Saved, and Disconnected. Failure and
busy outcomes change the ending. App-view tours walk through discovery,
the device page, the backup action, progress and its result. Package or device
restrictions instead stop at the state explaining why actions are unavailable.
Unknown-setting and populated-remap scenarios also highlight their specific UI.

The toolbar selects a scenario, board, browser or Android app view, operation
outcome, and firmware-package state. Selections are shareable URL parameters:

- `?mock=populated`: saved settings, remap rules and inherited macros.
- `?mock=normal&view=app`: device discovery and the device page in a browser.
- `?mock=normal&view=app&outcome=failure`: failed install, backup, upgrade or
  restore. In settings, saves are refused atomically.
- `?mock=updater`: firmware update without settings; `no_app` shows an updater
  without a valid application. `ephemeral` shows temporary settings status.
- `?mock=unknown`: an uncatalogued field and choice.
- `?mock=loading`, `empty`, `incompatible`, `read_error`, `unavailable`:
  loading, no settings, unsupported protocol, failed reads, missing browser APIs.
- App-view scenarios `stock`, `manual`, `jieli`, `bitdo`, `probe_error`,
  `permission`, `no_devices`: stock firmware, manual loader instructions,
  vendor loaders, identification error, denied Bluetooth, and empty discovery.
- `board=c2|n2|u2cw|u2cb` selects representative board layouts/capabilities.
- `package=present|missing|mismatch|corrupt` selects the bundled mock package.
- `outcome=success|failure|busy` selects save/update/job outcomes. Busy app jobs
  remain in progress until reload; protocol commands retain normal timeouts.

You can also explore manually with the real controls to create invalid edits, save, discard, reset, add/remove
rules and macros, import/export presets, and open help. The toolbar can expand
settings/help, disconnect the mock pad, and change connection/battery status
(shown on the next 5-second poll). It provides sample package and backup
files for the normal file pickers, including Restore. Reload clears mock writes.
Tours show errors as nonblocking notices so playback can continue. Restart the
tour after manual exploration if you want to resume a predictable sequence.
Scenario selection automatically chooses the app view for app-only scenarios;
you can override it with the View selector. The unavailable-browser-APIs
scenario always uses the browser view and disables that selector. The `step` URL parameter shares a
checkpoint, which is replayed on load.
Browser update success/failure uses the real updater flow; app firmware jobs
simulate progress, completion, or errors.

Mock mode selects in-memory providers before importing the application. It does
not import the hardware transport modules or Android bridge, shares no device
lock with a real tab, and uses a separate Bluetooth-preference key. It is also
available in the APK when its page is opened with the same query parameters.

`mock.js` simulates command answers through the actual USB/Bluetooth framing;
`Pad`, the editors, catalog lookups and screen rendering are the production
code. `mock-fields.json` is a checked-in fixture exported from the firmware
field tables by the host C tool. Run `make mock-fixture` after changing those
tables; `make web-test` checks for stale fixtures and exercises both transports.
The board fixtures represent UI capabilities, not exact electrical behavior or
all firmware validation rules. These scenarios cover the main screen states;
OS-owned choosers/permission dialogs and physical radio/USB behavior still
require their actual platforms.

An optional browser smoke test drives settings, help and a mock install through
Chromium's debugging interface. Start Chromium with
`--remote-debugging-port=9226`, serve the repository root on port 8796, then run:

```
node web/test/mock-browser.mjs http://127.0.0.1:9226 http://127.0.0.1:8796/web/
```

`web/test/mock-tour-browser.mjs` additionally checks held checkpoints, validation,
save/refusal, Previous replay, app progress, and automatic playback. It accepts
the same CDP endpoint and page URL arguments. Its default page URL is the local
HTML file; start Chromium with `--allow-file-access-from-files` to test without
an HTTP server.

`node web/test/review-browser.mjs [CDP endpoint] [review page URL]` checks every
mapped review state, background message collection and message search. As with
the tour check, the default URL is a local file and needs Chromium's
`--allow-file-access-from-files`; the test starts no web server.
