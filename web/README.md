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
  `android/app/src/main/java/dev/cyclone/padtool/Bridge.kt`), and its
  flasher; `android` is null in a browser.
- `flash.js`: the Flash panel, in the app only: firmware through the boot
  loaders (libjollymaam over JNI), which a browser cannot reach (the Jieli
  ROM loader is USB mass storage).

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
