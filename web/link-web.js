// The link interface over Web Bluetooth: one GATT link to a pad in config
// mode or its updater (src/profiles/profile_config.c, src/updater/).  The
// Android app supplies the same interface over its own Bluetooth; pad.js
// does the framing on top, so a link only writes frames and hands up
// notifications.
//
//   link = { name, id, maxWrite, write(frame) -> Promise,
//            onReply(fn), onDisconnect(fn), close() }
//   links = { available(), choose() -> Promise<link>,
//             reconnect(link, seconds) -> Promise<link> }

export const SERVICE = "c2550000-3e2d-66b1-8a4f-d5037c2b419e";
const REQUEST = "c2550001-3e2d-66b1-8a4f-d5037c2b419e";
const REPLY = "c2550002-3e2d-66b1-8a4f-d5037c2b419e";

// Pads whose radio advertises only a name (the U2C Bluetooth's core).
const NAME_ONLY = ["U2C BT Config"];

// A frame is at most BT_WRITE_MAX (src/core/pad.h) bytes.  Web Bluetooth
// does not tell the MTU; a frame longer than it goes as a long write.
const MAX_WRITE = 96;

// Each step named in its error, for the console: which one Chrome refuses.
const step = (name, p) => p.catch((error) => {
  error.step = name;
  throw error;
});

async function open(device) {
  const server = await step("connect", device.gatt.connect());
  const service = await step("service", server.getPrimaryService(SERVICE));
  const request = await step("request characteristic", service.getCharacteristic(REQUEST));
  const reply = await step("reply characteristic", service.getCharacteristic(REPLY));
  let replied = () => {}, gone = () => {};
  const notified = (e) => {
    const v = e.target.value;
    replied(new Uint8Array(v.buffer.slice(v.byteOffset, v.byteOffset + v.byteLength)));
  };
  const disconnected = () => {
    reply.removeEventListener("characteristicvaluechanged", notified);
    device.removeEventListener("gattserverdisconnected", disconnected);
    gone();
  };
  reply.addEventListener("characteristicvaluechanged", notified);
  device.addEventListener("gattserverdisconnected", disconnected);
  // Linux (BlueZ): a subscription the last link left on stays "notifying"
  // across the disconnect, and on the next link of the same device no
  // notification arrives through it; stopping it first starts afresh.
  await reply.stopNotifications().catch(() => {});
  await step("notifications", reply.startNotifications());
  return {
    name: device.name || "Bluetooth pad",
    id: device.id,
    maxWrite: MAX_WRITE,
    device,
    // quick: without response (UPDATE_DATA to a U2CB's updater, its count resyncs)
    write: (frame, quick = false) => quick ? request.writeValueWithoutResponse(frame) : request.writeValueWithResponse(frame),
    // Before a command that ends the link: leave no subscription behind.
    quiet: () => reply.stopNotifications().catch(() => {}),
    onReply(fn) { replied = fn; },
    onDisconnect(fn) { gone = fn; },
    // At once: a disconnect still pending when the next link comes up would
    // end that one.  open() clears a subscription left behind.
    close() { if (device.gatt.connected) device.gatt.disconnect(); },
  };
}

// A link to a device, tried again for a while: right after a disconnect
// (Disconnect, then Connect) Chrome on Linux answers "GATT Error Unknown"
// until BlueZ has finished with the old link.
async function openRetrying(device, seconds) {
  const end = Date.now() + seconds * 1000;
  for (;;) {
    try {
      return await open(device);
    } catch (error) {
      console.info(`link: ${error.step ?? "open"} failed: ${error.name}: ${error.message}` +
        (Date.now() > end ? "; giving up" : "; trying again"));
      if (Date.now() > end || error.name === "SecurityError") throw error;
      if (device.gatt.connected) device.gatt.disconnect();
      await new Promise((r) => setTimeout(r, 500));
    }
  }
}

export const links = {
  available: () => !!(globalThis.navigator && navigator.bluetooth),

  async choose() {
    const device = await navigator.bluetooth.requestDevice({
      filters: [{ services: [SERVICE] }, ...NAME_ONLY.map((name) => ({ name }))],
      optionalServices: [SERVICE],
    });
    return openRetrying(device, 5);
  },

  // The same pad again (after a restart into its updater, which keeps the
  // address): no chooser, as long as the page holds the device.
  async reconnect(link, seconds) {
    return openRetrying(link.device, seconds);
  },
};
