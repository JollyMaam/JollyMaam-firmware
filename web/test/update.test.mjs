// package.js on packages the jollymaam command makes, and update.js against
// a simulated updater (src/updater/updater.c, updater_ble.c: one frame a
// command, answers in one notification, UPDATE_DATA only at its count).
import { test } from "node:test";
import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import { existsSync, mkdirSync, mkdtempSync, writeFileSync, readFileSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { Pad } from "../pad.js";
import { openPackage, findFile, checkUpdateImage, sha256 } from "../package.js";
import { intoUpdater, sendImage } from "../update.js";

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), "../..");
const JOLLYMAAM = process.env.JOLLYMAAM || join(ROOT, "build/host/jollymaam");
const SDK = join(ROOT, "external/jieli-fw-AC63_BT_SDK");
const PRODUCT = "GS_C2_ADC_DEVICE";

// update.bin as jm_update_image_make() lays it out (the CRCs left 0: the
// page does not check them, the updater does).
function mainImage(product, appSize) {
  const d = new Uint8Array(0x120 + appSize).fill(0xff, 0, 0x120);
  const v = new DataView(d.buffer);
  d.set(new TextEncoder().encode("MAIN"));
  v.setUint32(4, appSize, true);
  v.setUint32(16, 1, true);
  d.fill(0, 24, 40);
  d.set(new TextEncoder().encode(product), 24);
  for (let i = 0; i < appSize; i++) d[0x120 + i] = (i * 31 + 7) & 0xff;
  return d;
}

test("checkUpdateImage", () => {
  assert.equal(checkUpdateImage(mainImage(PRODUCT, 100), PRODUCT), null);
  assert.match(checkUpdateImage(mainImage("AC695X_TRANS", 100), PRODUCT), /for AC695X_TRANS/);
  assert.match(checkUpdateImage(new Uint8Array(400), PRODUCT), /not an update image/);
  assert.match(checkUpdateImage(mainImage(PRODUCT, 100).subarray(0, 0x150), PRODUCT), /size/);
});

test("a package jollymaam makes, as made and zipped again", { skip: !existsSync(JOLLYMAAM) && `no ${JOLLYMAAM}` }, async () => {
  const dir = mkdtempSync(join(tmpdir(), "webpkg-"));
  try {
    const update = mainImage(PRODUCT, 5000);
    mkdirSync(join(dir, "build/c2/pad"), { recursive: true });
    writeFileSync(join(dir, "build/c2/pad/firmware.img"), new Uint8Array(3000).fill(0x5a));
    writeFileSync(join(dir, "build/c2/pad/update.bin"), update);
    execFileSync(JOLLYMAAM, ["package", "--sdk", SDK, join(dir, "build"), join(dir, "fw.zip")], { stdio: "ignore" });
    const zips = [readFileSync(join(dir, "fw.zip"))];
    try {           // a zip program's: deflated, inside a folder
      execFileSync("python3", ["-I", "-c", `
import sys, zipfile
src = zipfile.ZipFile(sys.argv[1])
with zipfile.ZipFile(sys.argv[2], "w", zipfile.ZIP_DEFLATED) as out:
    for n in src.namelist(): out.writestr("firmware/" + n, src.read(n))`, join(dir, "fw.zip"), join(dir, "again.zip")]);
      zips.push(readFileSync(join(dir, "again.zip")));
    } catch { /* no python3: the stored zip only */ }
    for (const z of zips) {
      const pkg = await openPackage(new Uint8Array(z));
      const file = findFile(pkg, PRODUCT, "update");
      assert.equal(file.path, "c2/pad/update.bin");
      assert.equal(file.sha256, await sha256(update));
      assert.deepEqual(await pkg.read(file), update);
      assert.ok(findFile(pkg, PRODUCT, "install"));
      assert.ok(findFile(pkg, "BR23", "loader"));
      assert.equal(findFile(pkg, "AC695X_TRANS", "update"), null);
    }
    assert.equal(zips.length, 2, "python3 for the deflated zip");
  } finally {
    rmSync(dir, { recursive: true, force: true });
  }
});

test("a package whose file does not match its manifest", async () => {
  // A stored zip by hand: manifest.txt and a.bin, the SHA-256 stated wrong.
  const files = [["manifest.txt", `jollymaam-firmware 2\nX update a.bin 3 ${"0".repeat(64)}\n`], ["a.bin", "abc"]];
  const enc = (s) => new TextEncoder().encode(s);
  const parts = [], central = [];
  let at = 0;
  for (const [name, text] of files) {
    const n = enc(name), d = enc(text);
    const local = new Uint8Array(30 + n.length + d.length), lv = new DataView(local.buffer);
    lv.setUint32(0, 0x04034b50, true); lv.setUint32(18, d.length, true); lv.setUint32(22, d.length, true);
    lv.setUint16(26, n.length, true); local.set(n, 30); local.set(d, 30 + n.length);
    const c = new Uint8Array(46 + n.length), cv = new DataView(c.buffer);
    cv.setUint32(0, 0x02014b50, true); cv.setUint32(20, d.length, true); cv.setUint32(24, d.length, true);
    cv.setUint16(28, n.length, true); cv.setUint32(42, at, true); c.set(n, 46);
    parts.push(local); central.push(c); at += local.length;
  }
  const cdSize = central.reduce((s, c) => s + c.length, 0);
  const end = new Uint8Array(22), ev = new DataView(end.buffer);
  ev.setUint32(0, 0x06054b50, true); ev.setUint16(8, 2, true); ev.setUint16(10, 2, true);
  ev.setUint32(12, cdSize, true); ev.setUint32(16, at, true);
  const zip = new Uint8Array(await new Blob([...parts, ...central, end]).arrayBuffer());
  const pkg = await openPackage(zip);
  await assert.rejects(pkg.read(findFile(pkg, "X", "update")), /SHA-256/);
  await assert.rejects(openPackage(enc("not a zip at all, but long enough to look")), /not a zip/);
});

// The updater: writes up to mtu - 3 bytes reach it whole; a longer one
// arrives as a long write, of which it takes the first part only.
function simulatedUpdater(mtu) {
  const room = 64 * 1024, flash = new Uint8Array(room);
  let reply = () => {}, state = 0, error = 0, begun = 0, written = 0;
  const info = () => {
    const d = new Uint8Array(32), v = new DataView(d.buffer);
    d.set([1, state, error, 1]);
    v.setUint32(4, 0x23000, true); v.setUint32(8, room, true); v.setUint32(12, written, true);
    v.setUint32(16, begun, true); v.setUint32(24, 512, true);
    return d;
  };
  const link = {
    name: "Cyclone 2 Update", id: "sim", maxWrite: 96, flash, frames: 0,
    onReply(fn) { reply = fn; }, onDisconnect() {}, close() {},
    async write(frame) {
      link.frames++;
      assert.equal(frame[0] & 0x80, 0, "the updater takes one frame a command");
      const f = frame.subarray(0, Math.min(frame.length, mtu - 3));
      const sub = f[0], arg = new DataView(f.buffer, f.byteOffset).getUint32(1, true), data = f.subarray(5);
      let answer = null;
      if (sub === 0x00) {
        answer = new Uint8Array(88);
        answer.set(new TextEncoder().encode("C2PD"));
        answer[4] = 2; answer[7] = 1;
        answer.set(new TextEncoder().encode(PRODUCT), 40);
      } else if (sub === 0x41) {
        answer = info();
      } else if (sub === 0x42) {
        const pid = new TextDecoder().decode(data.subarray(24, 40)).replace(/\0.*$/s, "");
        if (arg > room) error = 1;
        else if (pid !== PRODUCT) error = 9;
        else state = 1, error = 0, begun = arg, written = 0;
      } else if (sub === 0x43) {
        if (state === 1 && arg === written) {
          flash.set(data, written);
          written += data.length;
        }
      } else if (sub === 0x44) {
        state = written === begun ? 2 : 1;
        error = written === begun ? 0 : 5;
      }
      if (answer) setTimeout(() => reply(Uint8Array.of(sub, ...answer)), 1);
    },
  };
  return link;
}

for (const mtu of [512, 185, 64]) {   // IDENTIFY needs 60, BEGIN 52
  test(`an update through a ${mtu}-byte MTU`, async () => {
    const link = simulatedUpdater(mtu);
    const pad = new Pad(link);
    await pad.identify();
    const up = await intoUpdater(pad, null, () => {});
    assert.equal(up, pad);
    const image = mainImage(PRODUCT, 20000);
    const lines = [];
    let last = 0;
    await sendImage(up, image, (l) => lines.push(l), (done) => { last = done; });
    assert.equal(last, image.length);
    assert.deepEqual(link.flash.subarray(0, image.length), image, lines.join("\n"));
  });
}

test("an image for another pad is refused before anything is written", async () => {
  const link = simulatedUpdater(512);
  const pad = new Pad(link);
  await pad.identify();
  await assert.rejects(sendImage(pad, mainImage("AC695X_TRANS", 1000), () => {}), /refused, nothing changed: the image is for another board/);
});
