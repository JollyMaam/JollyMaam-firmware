// The links in the Android app (android/: the page in a WebView, which has
// neither Web Bluetooth nor WebHID): the same raw links as link-web.js and
// link-hid.js, over the app's own Bluetooth and USB (Bridge.kt, as
// window.JollyMaamAndroid).  And the app's flasher: libjollymaam over JNI,
// for the vendor loaders a browser cannot reach.
//
// A call to the app takes a call number and returns at once; the answer
// comes as jmAndroid.done(call, error, value), what a link hears as
// jmAndroid.event(handle, type, value).  Bytes go as base64.
//
//   android = null outside the app, else { bluetooth, usb, flasher }

const native = globalThis.JollyMaamAndroid ?? null;

const FLAGS = typeof location !== "undefined" ? new URLSearchParams(location.search) : new URLSearchParams();
const DEBUG = FLAGS.has("debug");
const hex = (d) => Array.from(d, (b) => b.toString(16).padStart(2, "0")).join("");

export function toBase64(d) {
  let s = "";
  for (let i = 0; i < d.length; i += 0x8000) s += String.fromCharCode(...d.subarray(i, i + 0x8000));
  return btoa(s);
}

export const fromBase64 = (s) => Uint8Array.from(atob(s), (c) => c.charCodeAt(0));

const calls = new Map(), listeners = new Map();
let nextCall = 1;

if (native) {
  globalThis.jmAndroid = {
    done(call, error, value) {
      const c = calls.get(call);
      if (!c) return;
      calls.delete(call);
      if (error) c.reject(Object.assign(new Error(error.message), { name: error.name }));
      else c.resolve(value);
    },
    event(handle, type, value) {
      listeners.get(handle)?.(type, value);
    },
  };
}

// method(call, ...args) in the app; its answer.
function call(method, ...args) {
  return new Promise((resolve, reject) => {
    const n = nextCall++;
    calls.set(n, { resolve, reject });
    try {
      native[method](n, ...args);
    } catch (error) {
      calls.delete(n);
      reject(error);
    }
  });
}

let linkCount = 0;

// A link's handle in the app: its events to the link, and close.
function events(handle, on) {
  listeners.set(handle, (type, value) => {
    if (type === "gone") {
      listeners.delete(handle);
      on.gone();
    } else on[type]?.(value);
  });
  return () => {
    listeners.delete(handle);
    native.closeLink(handle);
  };
}

// ---- USB: the tool device (as link-hid.js) ----

function hidLink({ handle, name, reportSize, device }) {
  let input = () => {}, gone = () => {};
  const n = ++linkCount;
  const close = events(handle, {
    input: (b) => {
      const d = fromBase64(b);
      if (DEBUG) console.log(`hid ${n} RX ${hex(d.subarray(0, 16))}…`);
      input(d);
    },
    gone: () => gone(),
  });
  return {
    kind: "hid", name, id: `hid:${name}`, reportSize, handle, device,
    sendFeature(report) {
      if (DEBUG) console.log(`hid ${n} TX ${hex(report.subarray(0, 32))}`);
      return call("usbSend", handle, toBase64(report));
    },
    onInput(fn) { input = fn; },
    onDisconnect(fn) { gone = fn; },
    close,
  };
}

const usb = {
  available: () => true,
  // device: the app's name for the USB device to take; else the user
  // chooses if there are several.
  choose: async (device = null) => hidLink(await call("usbChoose", device ?? "")),
  // The same pad after a restart: any tool device that comes up, the same
  // board first; the caller checks the flash ID.
  reconnect: async (link, seconds) => hidLink(await call("usbReconnect", link.name, seconds)),
};

// ---- Bluetooth (as link-web.js) ----

// A frame is at most BT_WRITE_MAX (src/core/pad.h) bytes; longer ones go
// as long writes.
const MAX_WRITE = 96;

function bleLink({ handle, name, id }) {
  let replied = () => {}, gone = () => {};
  const n = ++linkCount;
  const close = events(handle, {
    reply: (b) => {
      const d = fromBase64(b);
      if (DEBUG) console.log(`link ${n} RX ${hex(d)}`);
      replied(d);
    },
    gone: () => gone(),
  });
  return {
    name, id, handle, maxWrite: MAX_WRITE,
    write(frame, quick = false) {
      if (DEBUG) console.log(`link ${n} TX ${hex(frame)}`);
      return call("bleWrite", handle, toBase64(frame), !!quick);
    },
    quiet: () => call("bleQuiet", handle).catch(() => {}),
    onReply(fn) { replied = fn; },
    onDisconnect(fn) { gone = fn; },
    close,
  };
}

const bluetooth = {
  available: () => !!native?.bleAvailable(),
  // Scanning and connecting allowed (the app asks if need be): true/false.
  permission: () => call("blePermission"),
  // Pads in config mode (or their updater) while they advertise:
  // found({name, id}) each, again every half second or so while heard;
  // failed(why); returns stop.
  scan(found, failed) {
    let handle = null, stopped = false;
    call("bleScan").then(({ handle: h }) => {
      if (stopped) return native.closeLink(h);
      handle = h;
      listeners.set(h, (type, v) => (type === "found" ? found(v) : type === "failed" ? failed?.(v) : null));
    }, (error) => failed?.(error.message));
    return () => {
      stopped = true;
      if (handle === null) return;
      listeners.delete(handle);
      native.closeLink(handle);
    };
  },
  // The link to a pad by its address (from scan); without one the first
  // pad the scan finds.
  async choose(device = null) {
    if (!device) {
      device = await new Promise((resolve, reject) => {
        let done = false;
        const stop = bluetooth.scan((v) => { if (!done) { done = true; stop(); resolve(v.id); } },
          (why) => { done = true; stop(); reject(new Error(why)); });
      });
    }
    return bleLink(await call("bleOpen", device));
  },
  // The same pad again (a restart into its updater keeps the address).
  reconnect: async (link, seconds) => bleLink(await call("bleReconnect", link.handle, link.id, seconds)),
};

// ---- the flasher (Bridge.kt: libjollymaam over JNI) ----

// The flasher's progress and lines (events on handle 0) while work runs.
async function watched(progress, say, work) {
  listeners.set(0, (type, v) => {
    if (type === "progress") progress?.(v.what, v.done, v.total);
    else if (type === "log") say?.(v);
  });
  try {
    return await work();
  } finally {
    listeners.delete(0);
  }
}

const flasher = {
  // A firmware package's bytes, checked by the app: [{product, board, chip}].
  package: (bytes) => call("flashPackage", toBase64(bytes)),
  // [{device, name, id, kind}]: kind 0 our tool device, 1 the 8BitDo boot
  // loader, 2 a Jieli ROM loader, 3 a stock pad.
  scan: () => call("flashScan"),
  // {kind, what, product, board, name, chip, mode, build, uid, updater,
  // canEnter, byHand} ("" where not known).
  probe: (device) => call("flashProbe", device),
  // One pad session through its vendor loader (libjollymaam's jm_pad): the
  // whole flash backed up first into a file named after label (with
  // backup), then action: 0 none (it stays in its loader), 1 install, 2
  // restore image (raw: all of it), 3 reboot.  uid: our firmware's.
  // {backup: where the file is, or null}.  progress(what, done, total);
  // say(line): libjollymaam's lines.
  session: (device, { uid = "", backup = false, label = "pad", action, image = null, raw = false }, progress, say) =>
    watched(progress, say, () => call("flashSession", device, uid ?? "", !!backup, label, action,
      image ? toBase64(image) : "", !!raw)),
  // update.bin for product through our updater (our firmware, uid).
  update: (device, product, uid, progress, say) =>
    watched(progress, say, () => call("flashUpdate", device, product, uid)),
};

export const android = native ? { bluetooth, usb, flasher } : null;
