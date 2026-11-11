// The HID link over WebHID: the pad's tool device on USB (16c0:05df,
// usage page 0xffc2: platform_jieli_usb.c, platform_telink_usb.c), its app
// or its updater, in any state (idle, connected, config mode).  The
// Android app can supply the same raw link over its own USB; frames-hid.js
// does the framing on top, so a link only sends feature reports and hands
// up input reports.
//
//   link = { kind: "hid", name, id, reportSize, sendFeature(bytes) -> Promise,
//            onInput(fn), onDisconnect(fn), close() }
//   links = { available(), choose() -> Promise<link>,
//             reconnect(link, seconds) -> Promise<link> }

export const VENDOR = 0x16c0, PRODUCT = 0x05df, USAGE_PAGE = 0xffc2;
const FILTER = { vendorId: VENDOR, productId: PRODUCT, usagePage: USAGE_PAGE };

const FLAGS = typeof location !== "undefined" ? new URLSearchParams(location.search) : new URLSearchParams();
const DEBUG = FLAGS.has("debug");
const hex = (d) => Array.from(d, (b) => b.toString(16).padStart(2, "0")).join("");

// Linux (hidraw) sends a feature report as long as it is given and the pad
// goes by the request's length: a short one is quicker (a B80's EP0 takes
// 8 bytes a packet).  Elsewhere the report goes at its descriptor's size.
const SHORT_REPORTS = typeof navigator !== "undefined" &&
  /linux/i.test(navigator.userAgentData?.platform ?? navigator.platform ?? "");

// The tool collection's report sizes, from the descriptor.
function sizes(device) {
  const tool = device.collections.find((c) => c.usagePage === USAGE_PAGE);
  if (!tool) return null;
  const bytes = (reports) => (reports?.[0]?.items ?? []).reduce((n, i) => n + i.reportSize * i.reportCount, 0) / 8;
  return { feature: bytes(tool.featureReports), input: bytes(tool.inputReports) };
}

let linkCount = 0;

async function open(device) {
  const size = sizes(device);
  if (!size?.feature) throw new Error(`${device.productName}: not a tool device (no feature report on page 0xffc2)`);
  if (size.input < 64) throw new Error(`${device.productName}: the tool device has no input report ` +
    "(firmware from before 2026-10-27: update it through its boot loader)");
  if (!device.opened) await device.open();
  let input = () => {}, gone = () => {};
  const n = ++linkCount;
  const reported = (e) => {
    const d = new Uint8Array(e.data.buffer, e.data.byteOffset, e.data.byteLength);
    if (DEBUG) console.log(`hid ${n} RX ${hex(d.subarray(0, 16))}…`);
    input(d);
  };
  const disconnected = (e) => {
    if (e.device !== device) return;
    device.removeEventListener("inputreport", reported);
    navigator.hid.removeEventListener("disconnect", disconnected);
    gone();
  };
  device.addEventListener("inputreport", reported);
  navigator.hid.addEventListener("disconnect", disconnected);
  return {
    kind: "hid",
    name: device.productName || "USB pad",
    id: `hid:${device.productName}`,
    reportSize: size.feature,
    device,
    async sendFeature(report) {
      if (DEBUG) console.log(`hid ${n} TX ${hex(report.subarray(0, 32))}`);
      let out = report;
      if (!SHORT_REPORTS && report.length < size.feature) {
        out = new Uint8Array(size.feature);
        out.set(report);
      }
      await device.sendFeatureReport(0, out);
    },
    onInput(fn) { input = fn; },
    onDisconnect(fn) { gone = fn; },
    // Only this page's handle: the device stays for every other program.
    close() {
      device.removeEventListener("inputreport", reported);
      navigator.hid.removeEventListener("disconnect", disconnected);
      if (device.opened) device.close().catch(() => {});
    },
  };
}

export const links = {
  available: () => !!(globalThis.navigator && navigator.hid),

  async choose() {
    const [device] = await navigator.hid.requestDevice({ filters: [FILTER] });
    if (!device) throw Object.assign(new Error("No pad chosen."), { name: "NotFoundError" });
    return open(device);
  },

  // The same pad again, after a restart (into its updater, or back into
  // its app): a tool device that comes up within the time, of those the page
  // may use (the pad keeps its serial, the flash UID, so Chrome keeps the
  // permission).  The same product name first; the caller checks the flash
  // ID.  No chooser.
  async reconnect(link, seconds) {
    const end = Date.now() + seconds * 1000, failed = new Set();
    const board = (name) => (name ?? "").replace(/ Update$/, "");   // the updater's product name
    const same = (d) => board(d.productName) === board(link.name);
    for (;;) {
      const found = (await navigator.hid.getDevices())
        .filter((d) => d !== link.device && d.vendorId === VENDOR && d.productId === PRODUCT && sizes(d))
        .sort((a, b) => same(b) - same(a));
      for (const device of found) {
        try {
          return await open(device);
        } catch (error) {
          if (!failed.has(device)) console.info(`hid: ${device.productName}: ${error.message}`);
          failed.add(device);
        }
      }
      if (Date.now() > end) {
        throw new Error(`the pad did not come back on USB in ${seconds} s (or this page may not use it: Connect over USB)`);
      }
      await new Promise((r) => setTimeout(r, 250));
    }
  },
};
