// link-android.js over a fake app (Bridge.kt's calls, answered as the app
// answers them: jmAndroid.done and jmAndroid.event, bytes in base64), with
// pad.js on top: the USB link against the simulated tool device, the
// Bluetooth link's frames, a link gone, the flasher's progress.
import { test } from "node:test";
import assert from "node:assert/strict";
import { simulatedTool } from "./sim-tool.mjs";

const b64 = (d) => Buffer.from(d).toString("base64");
const bytes = (s) => new Uint8Array(Buffer.from(s, "base64"));
const later = (fn) => setTimeout(fn, 0);

const app = { tool: null, frames: [], closed: [], calls: [] };
const ok = (call, value = null) => later(() => globalThis.jmAndroid.done(call, null, value));
globalThis.JollyMaamAndroid = {
  bleAvailable: () => true,
  closeLink: (handle) => app.closed.push(handle),
  usbChoose(call) {
    app.tool = simulatedTool();
    app.tool.onInput((r) => globalThis.jmAndroid.event(7, "input", b64(r)));
    ok(call, { handle: 7, name: "Cyclone 2", reportSize: 512 });
  },
  usbSend(call, handle, report) {
    assert.equal(handle, 7);
    app.tool.sendFeature(bytes(report)).then(() => ok(call));
  },
  bleChoose: (call) => ok(call, { handle: 9, name: "Cyclone 2 Config", id: "AA:BB:CC:DD:EE:FF", mtu: 512 }),
  bleReconnect(call, handle, address, seconds) {
    app.calls.push(["bleReconnect", handle, address, seconds]);
    ok(call, { handle: 10, name: "Cyclone 2 Update", id: address, mtu: 512 });
  },
  bleWrite(call, handle, frame, quick) {
    app.frames.push({ handle, frame: bytes(frame), quick });
    ok(call);
  },
  bleQuiet: (call) => ok(call),
  usbReconnect: (call) => later(() => globalThis.jmAndroid.done(call, { name: "Error", message: "the pad did not come back" }, null)),
  flashBackup(call, device, uid, label) {
    app.calls.push(["flashBackup", device, uid, label]);
    later(() => {
      globalThis.jmAndroid.event(0, "log", "reading");
      globalThis.jmAndroid.event(0, "progress", { done: 4, total: 4 });
      ok(call, `Download/JollyMaam/${label}.bin`);
    });
  },
  flashRun(call, device, product, uid, install) {
    app.calls.push(["flashRun", device, product, uid, install]);
    later(() => {
      globalThis.jmAndroid.event(0, "progress", { done: 50, total: 100 });
      globalThis.jmAndroid.event(0, "progress", { done: 100, total: 100 });
      ok(call);
    });
  },
};
const { android, toBase64, fromBase64 } = await import("../link-android.js");
const { Pad } = await import("../pad.js");

test("android: base64 both ways, long data too", () => {
  const d = Uint8Array.from({ length: 100000 }, (_, i) => (i * 7) & 0xff);
  assert.equal(toBase64(d), b64(d));
  assert.deepEqual(fromBase64(b64(d)), d);
});

test("android usb: pad.js over the app's link", async () => {
  const link = await android.usb.choose();
  assert.equal(link.kind, "hid");
  assert.equal(link.reportSize, 512);
  const pad = new Pad(link);
  assert.equal((await pad.identify()).boardName, "Cyclone 2");
  assert.equal((await pad.status()).state, "idle");
  let gone = false;
  pad.onDisconnect = () => { gone = true; };
  globalThis.jmAndroid.event(7, "gone", null);
  assert.ok(gone && pad.gone);
  globalThis.jmAndroid.event(7, "input", b64(new Uint8Array(64)));   // after gone: dropped
  await assert.rejects(android.usb.reconnect(link, 1), /did not come back/);
});

test("android bluetooth: frames out, replies in, reconnect by address", async () => {
  const link = await android.bluetooth.choose();
  assert.equal(link.maxWrite, 96);
  const got = [];
  link.onReply((r) => got.push(r));
  await link.write(Uint8Array.of(1, 2, 3), true);
  assert.deepEqual(app.frames.at(-1), { handle: 9, frame: Uint8Array.of(1, 2, 3), quick: true });
  globalThis.jmAndroid.event(9, "reply", b64(Uint8Array.of(4, 5)));
  assert.deepEqual(got, [Uint8Array.of(4, 5)]);
  await link.quiet();
  const again = await android.bluetooth.reconnect(link, 30);
  assert.deepEqual(app.calls.at(-1), ["bleReconnect", 9, "AA:BB:CC:DD:EE:FF", 30]);
  assert.equal(again.handle, 10);
  again.close();
  assert.equal(app.closed.at(-1), 10);
});

test("android flasher: progress while it runs", async () => {
  const seen = [];
  await android.flasher.run("/dev/bus/usb/001/002", "CYCLONE2", null, (d, t) => seen.push(d / t));
  assert.deepEqual(seen, [0.5, 1]);
  assert.deepEqual(app.calls.at(-1), ["flashRun", "/dev/bus/usb/001/002", "CYCLONE2", "", false]);
  await android.flasher.run("/dev/bus/usb/001/002", "CYCLONE2", "00ff", null, null, true);
  assert.deepEqual(app.calls.at(-1), ["flashRun", "/dev/bus/usb/001/002", "CYCLONE2", "00ff", true]);
});

test("android flasher: a backup's progress and lines, and where it went", async () => {
  const seen = [], said = [];
  const where = await android.flasher.backup("/dev/bus/usb/001/003", "00ff", "CYCLONE2", (d, t) => seen.push(d / t), (l) => said.push(l));
  assert.equal(where, "Download/JollyMaam/CYCLONE2.bin");
  assert.deepEqual(seen, [1]);
  assert.deepEqual(said, ["reading"]);
  assert.deepEqual(app.calls.at(-1), ["flashBackup", "/dev/bus/usb/001/003", "00ff", "CYCLONE2"]);
});
