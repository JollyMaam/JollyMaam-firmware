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

function hidLink({ handle, name, reportSize }) {
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
    kind: "hid", name, id: `hid:${name}`, reportSize, handle,
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
  choose: async () => hidLink(await call("usbChoose")),
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
  choose: async () => bleLink(await call("bleChoose")),
  // The same pad again (a restart into its updater keeps the address).
  reconnect: async (link, seconds) => bleLink(await call("bleReconnect", link.handle, link.id, seconds)),
};

// ---- the flasher (Bridge.kt: libjollymaam over JNI) ----

const flasher = {
  // A firmware package's bytes, checked by the app: [{product, board, chip}].
  package: (bytes) => call("flashPackage", toBase64(bytes)),
  // [{device, name, id, kind}]: kind 0 our tool device, 1 the 8BitDo boot
  // loader, 2 a Jieli ROM loader.
  scan: () => call("flashScan"),
  // kind 0 {product, build, uid, updater}, 1 {product}, 2 {chip}.
  probe: (device) => call("flashProbe", device),
  // The package's firmware for product onto device; progress(done, total).
  // say(line): what the flasher does, a line at a time (libjollymaam's).
  async run(device, product, uid, progress, say) {
    listeners.set(0, (type, v) => {
      if (type === "progress") progress?.(v.done, v.total);
      else if (type === "log") say?.(v);
    });
    try {
      return await call("flashRun", device, product, uid ?? "");
    } finally {
      listeners.delete(0);
    }
  },
  stockLoader: (device, uid) => call("flashStockLoader", device, uid),
};

export const android = native ? { bluetooth, usb, flasher } : null;
