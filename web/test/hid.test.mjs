// pad.js over a HID link (frames-hid.js) against a simulated tool device:
// platform_jieli_usb.c's feature report in, its 64-byte input reports out
// (client, tag, state, offset, 56 bytes), the answer as long as asked for.
import { test } from "node:test";
import assert from "node:assert/strict";
import { Pad, CMD } from "../pad.js";

const REPORT = 512, IN_DATA = 56;
const MORE = 2, LAST = 3, REFUSED = 4;

function simulatedTool() {
  let input = () => {};
  const inputs = [];               // every program's: each sees all
  const link = {
    kind: "hid", name: "Cyclone 2", id: "sim", reportSize: REPORT, reports: [], refuse: 0, lose: null,
    onInput(fn) { input = fn; }, onDisconnect() {}, close() {},
    emit(r) { inputs.push(r); setTimeout(() => input(r), 0); },
    async sendFeature(report) {
      assert.ok(report.length <= REPORT);
      const v = new DataView(report.buffer, report.byteOffset);
      const sub = report[1], arg = v.getUint32(2, true), len = v.getUint16(16, true), flags = report[18];
      const tag = report[19], client = v.getUint32(20, true);
      assert.equal(report[0], 0xf5);
      assert.ok(flags & 2, "every command asks for input-report answers");
      const data = report.subarray(24);
      link.reports.push({ sub, arg, len, flags, data: data.slice() });
      const head = (state, offset) => {
        const r = new Uint8Array(64), w = new DataView(r.buffer);
        w.setUint32(0, client, true); r[4] = tag; r[5] = state; w.setUint16(6, offset, true);
        return r;
      };
      // another program's answer first: never taken
      const other = head(LAST, 0);
      other[0] ^= 1;
      link.emit(other);
      if (link.refuse) { link.refuse--; return link.emit(head(REFUSED, 0)); }
      let body = new Uint8Array(0);
      if (flags & 1) assert.equal(len, data.length);
      if (sub === CMD.IDENTIFY) {
        body = new Uint8Array(88);
        body.set(new TextEncoder().encode("C2PD"));
        body[4] = 2;
        body.set(new TextEncoder().encode("CYCLONE2"), 40);
        body.set(new TextEncoder().encode("Cyclone 2"), 56);
      } else if (sub === CMD.STATUS) {
        body = Uint8Array.of(1, 0xff, 1, 0, 0, 0, 0, 0, 0x10, 0x27, 0, 0, 0, 0, 0, 0);
      } else if (sub === CMD.RESULT) {
        body = Uint8Array.of(2, 0, 0);
      } else if (sub === 0x02) {   // something long: 0, 1, 2, ...
        body = Uint8Array.from({ length: 300 }, (_, i) => i & 0xff);
      }
      const n = flags & 1 ? 0 : len, answer = new Uint8Array(n);
      answer.set(body.subarray(0, n));
      for (let o = 0, i = 0; ; o += IN_DATA, i++) {
        const piece = answer.subarray(o, o + IN_DATA), r = head(o + IN_DATA < n ? MORE : LAST, o);
        r.set(piece, 8);
        if (link.lose === i) link.lose = null;
        else link.emit(r);
        if (o + IN_DATA >= n) break;
      }
    },
  };
  return link;
}

test("usb: identify and status, answers of the size asked for", async () => {
  const link = simulatedTool();
  const pad = new Pad(link);
  const id = await pad.identify();
  assert.equal(id.boardName, "Cyclone 2");
  assert.equal(id.dataMax, 488, "the report less its head, not config mode's u16");
  assert.equal(link.reports[0].len, 88);
  assert.equal((await pad.status()).state, "idle");
  assert.equal(link.reports[1].len, 16);
});

test("usb: a long answer in pieces, and one with a piece lost goes again", async () => {
  const link = simulatedTool();
  const pad = new Pad(link);
  const want = Uint8Array.from({ length: 300 }, (_, i) => i & 0xff);
  assert.deepEqual(await pad.cmd(0x02, { size: 300 }), want);
  link.lose = 2;
  assert.deepEqual(await pad.cmd(0x02, { size: 300 }), want);
  assert.equal(link.reports.length, 3);
});

test("usb: refused while another program's command runs: sent again", async () => {
  const link = simulatedTool();
  const pad = new Pad(link);
  link.refuse = 3;
  assert.equal((await pad.status()).state, "idle");
  assert.equal(link.reports.length, 4);
});

test("usb: data in one report, at most the report less its head", async () => {
  const link = simulatedTool();
  const pad = new Pad(link);
  await pad.identify();
  await pad.write([{ target: 0, id: 1, value: Uint8Array.of(2) }, { target: 0, id: 2, value: Uint8Array.of(1, 2) }]);
  const w = link.reports.find((r) => r.sub === CMD.WRITE);
  assert.equal(w.flags, 3);
  assert.deepEqual(Array.from(w.data), [0, 1, 1, 2, 0, 2, 2, 1, 2]);
  assert.deepEqual(link.reports.map((r) => r.sub).slice(1), [CMD.WRITE, CMD.RESULT, CMD.COMMIT]);
  await assert.rejects(pad.cmd(CMD.WRITE, { data: new Uint8Array(489) }), /takes 488/);
});
