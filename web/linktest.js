// Link tests, for what Web Bluetooth does not tell: how long a write may be
// (no MTU in the API: a longer one goes as a long write) and whether the
// updater is found again without the chooser.
import { Pad, CMD, decodeUpdateInfo } from "./pad.js";

const SET_UNKNOWN = 1;           // RESULTS[1] (padtool): a target past the last is not staged

// WRITE items {target, id, len, value} for target 0xff: the pad parses them
// all and stages none, and RESULT says how many it found.  Data lost or
// shifted on the way shows as a different count or a "bad length".
export function echoItems(size) {
  const out = new Uint8Array(size), lengths = [];
  let o = 0, i = 0;
  while (size - o >= 3) {
    let l = (i * 7) % 23;
    if (size - o - 3 - l < 3) l = size - o - 3;
    out.set([0xff, i & 0xff, l], o);
    for (let k = 0; k < l; k++) out[o + 3 + k] = (i + k) & 0xff;
    lengths.push(l);
    o += 3 + l, i++;
  }
  return { data: out, items: lengths.length };
}

export async function writeTest(pad, frameData, size, log) {
  const was = pad.frameData;
  pad.frameData = frameData;
  const { data, items } = echoItems(size);
  const t0 = performance.now();
  try {
    await pad.cmd(CMD.WRITE, { data });
    const ms = performance.now() - t0;
    const r = await pad.cmd(CMD.RESULT);
    const ok = r[0] === items && r.subarray(1, 1 + items).every((c) => c === SET_UNKNOWN);
    log(`${frameData + 5}-byte writes, ${size} bytes in ${Math.ceil(size / frameData)}: ` +
      (ok ? `intact, ${ms.toFixed(0)} ms (${(size / ms * 1000 / 1024).toFixed(1)} KiB/s)`
          : `CHANGED: pad found ${r[0]} items of ${items}, codes ${Array.from(r.subarray(1, 1 + r[0])).join(",")}`));
    return ok;
  } catch (error) {
    log(`${frameData + 5}-byte writes: FAILED after ${(performance.now() - t0).toFixed(0)} ms: ${error.message || error}`);
    return false;
  } finally {
    pad.frameData = was;
  }
}

export async function writeTests(pad, log) {
  log(`config mode takes ${pad.id.dataMax} bytes a command`);
  for (const frameData of [15, 91]) {             // 20-byte writes fit the smallest MTU; 96 is BT_WRITE_MAX
    if (!(await writeTest(pad, frameData, pad.id.dataMax, log)) && pad.gone) return;
  }
  log(`longest notification: ${pad.maxReply} bytes (pieces are at most 158)`);
}

// UPDATER, the same pad again without the chooser, its IDENTIFY and
// UPDATE_INFO, then REBOOT back into the app.  The link ends.
export async function updaterTrip(pad, links, log) {
  const { link, id } = pad;
  log("restarting into the updater…");
  let t0 = performance.now();
  await pad.restart(CMD.UPDATER);
  log(`link gone after ${(performance.now() - t0).toFixed(0)} ms; reconnecting (no chooser)…`);
  t0 = performance.now();
  let up;
  try {
    up = new Pad(await links.reconnect(link, 30));
  } catch (error) {
    log(`FAILED: no reconnect in 30 s: ${error.message || error}`);
    return;
  }
  log(`reconnected after ${(performance.now() - t0).toFixed(0)} ms`);
  try {
    // The updater says nothing to IDENTIFY until its MTU has room for it.
    let uid;
    for (let attempt = 1; !uid; attempt++) {
      try {
        uid = await up.identify(1000);
        log(`IDENTIFY answered on attempt ${attempt}, ${(performance.now() - t0).toFixed(0)} ms after the reconnect began`);
      } catch (error) {
        if (attempt === 8 || up.gone) {
          // STATUS (16 bytes) fits the smallest MTU's notification, IDENTIFY does not.
          try {
            const st = await up.cmd(CMD.STATUS, { timeoutMs: 1000 });
            log(`IDENTIFY: no answer in ${attempt} tries, but STATUS answered (${st.length} bytes): ` +
              "the updater's MTU is below 60, notifications work");
          } catch {
            log(`IDENTIFY: no answer in ${attempt} tries, and STATUS neither: no notification reaches the page`);
          }
          throw error;
        }
      }
    }
    log(`updater: ${uid.updater ? "yes" : "NO"}, flash ID ${uid.flashId === id.flashId ? "the same" : "DIFFERENT " + uid.flashId}, ` +
      `build ${uid.build}, name ${up.link.name}`);
    const info = decodeUpdateInfo(await up.cmd(CMD.UPDATE_INFO));
    log(`update info: app at 0x${info.at.toString(16)}, room ${info.room}, ${info.valid ? "an app is there" : "no app"}, ` +
      `data max ${info.dataMax}`);
    log(`longest notification: ${up.maxReply} bytes (one piece; IDENTIFY needs 89)`);
  } catch (error) {
    log(`FAILED in the updater: ${error.message || error}`);
  }
  log("rebooting into the app…");
  await up.restart(CMD.REBOOT);
  log("done: the pad is back in its app (not config mode)");
}
