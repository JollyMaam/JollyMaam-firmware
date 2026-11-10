# Web page

Pad settings in the browser over Web Bluetooth: a static page (plain ES
modules, no build), for GitHub Pages and, later, the Android app's WebView.

- `link-web.js`: the link, one GATT connection to a pad in config mode or
  its updater. The Android app will supply the same interface over its own
  Bluetooth (WebView has no Web Bluetooth).
- `pad.js`: the framing (`src/profiles/profile_config.c`) and the tool
  commands (`src/core/usb.c`) over a link.
- `app.js`, `index.html`, `style.css`: the page. Read only so far.

Web Bluetooth needs Chrome or Edge, and a secure context (https, or
localhost). To serve it from a checkout:

```sh
python3 -m http.server -d web 8000     # then http://localhost:8000
```

`make web-test` runs `pad.js` against a simulated pad (`test/`), in node.
