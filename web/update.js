// A firmware update over Bluetooth LE through our updater (src/updater/,
// docs/firmware/updater.md), as padtool's update() does it over USB or LE:
// into the updater, BEGIN with the header, the image in order, COMMIT,
// REBOOT.  The app then starts in config mode on the same address.
import { Pad, CMD, UPDATE_ERRORS } from "./pad.js";

const HEADER_CHECKED = 0x2c;     // what BEGIN checks before it erases
const FRAME_HEAD = 5;
const ATT_MTU_MAX = 512;         // the updater's (its log: "le mtu 512")
const MIN_CHUNK = 15;            // the updater needs an MTU of 60 anyway (IDENTIFY)

const why = (e) => UPDATE_ERRORS[e] || `error ${e}`;

// The pad in its updater: pad itself, or the same pad found again after
// UPDATER (no chooser).  Returns the updater's Pad.
export async function intoUpdater(pad, links, log) {
  if (pad.id.updater) return pad;
  const { link, id } = pad;
  log("restarting into the updater…");
  await pad.restart(CMD.UPDATER);
  const up = new Pad(await links.reconnect(link, 30));
  await up.identify(1000, 8);
  if (!up.id.updater) throw new Error("the pad came back, but not in its updater");
  if (up.id.flashId !== id.flashId) throw new Error("another pad answered on its address");
  return up;
}

// The image to the updater.  progress(written, total).  Returns when
// committed; the caller reboots (finish).
export async function sendImage(up, image, log, progress) {
  let info = await up.updateInfo();
  log(`updater: app at 0x${info.at.toString(16)}, room ${info.room} bytes, ${info.valid ? "an app is there" : "no app"}`);
  if (image.length > info.room) throw new Error(`the image (${image.length} bytes) does not fit (${info.room})`);

  // Writes as long as the link takes: Web Bluetooth does not say its MTU,
  // and a longer write goes in pieces of which the updater keeps the first.
  // What it did not keep shows in its count: then shorter writes.
  let chunk = Math.min(info.dataMax || 512, ATT_MTU_MAX - 3 - FRAME_HEAD);
  up.frameData = Math.max(chunk, HEADER_CHECKED);   // one frame a command: the updater has no MORE

  await up.cmd(CMD.UPDATE_BEGIN, { arg: image.length, data: image.subarray(0, HEADER_CHECKED) });
  info = await up.updateInfo();
  if (info.error) {
    throw new Error(info.state === 0 && info.error !== 8
      ? `refused, nothing changed: ${why(info.error)}` : `begin: ${why(info.error)} (the pad stays in its updater)`);
  }
  log(`erasing and writing ${image.length} bytes`);
  const t0 = performance.now();
  let offset = 0, resent = 0;
  for (;;) {
    const every = Math.max(1, Math.floor(16384 / chunk));
    up.frameData = chunk;
    let failed = null;
    for (let i = 0; i < every && offset < image.length; i++) {
      const piece = image.subarray(offset, offset + chunk);
      try {
        await up.cmd(CMD.UPDATE_DATA, { arg: offset, data: piece, timeoutMs: 10000 });
      } catch (error) {
        if (up.gone) throw error;
        failed = error;
        break;
      }
      offset += piece.length;
    }
    info = await up.updateInfo();
    if (info.error) throw new Error(`at ${offset}: ${why(info.error)} (the pad stays in its updater)`);
    if (info.written !== offset) {
      if (++resent > 50) throw new Error("writes keep getting lost (the pad stays in its updater)");
      if (chunk > MIN_CHUNK) {
        chunk = Math.max(MIN_CHUNK, chunk >> 1);
        log(`${failed ? "a write failed" : "writes were cut short"} at ${offset}; ${chunk + FRAME_HEAD}-byte writes from ${info.written}`);
      }
      offset = info.written;
    }
    progress?.(info.written, image.length);
    if (info.written === image.length) break;
  }
  const s = (performance.now() - t0) / 1000;
  log(`${image.length} bytes in ${s.toFixed(1)} s (${(image.length / s / 1024).toFixed(1)} KiB/s, ` +
    `${chunk + FRAME_HEAD}-byte writes${resent ? `, ${resent} resends` : ""})`);
  await up.cmd(CMD.UPDATE_COMMIT, { answer: false, timeoutMs: 10000 });
  info = await up.updateInfo();
  if (info.state !== 2 || info.error) throw new Error(`the updater did not commit: ${why(info.error)}`);
  log("committed");
}

// REBOOT after the commit: the app starts in config mode, found again on
// the same address.  Returns its Pad.
export async function finish(up, links, log) {
  const link = up.link;
  log("restarting into the new firmware…");
  await up.restart(CMD.REBOOT);
  const pad = new Pad(await links.reconnect(link, 30));
  await pad.identify(1000, 8);
  log(`running build ${pad.id.build}`);
  return pad;
}
