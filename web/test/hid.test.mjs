// pad.js over a HID link (frames-hid.js) against a simulated tool device:
// platform_jieli_usb.c's feature report in, its 64-byte input reports out
// (client, tag, state, offset, 56 bytes), the answer as long as asked for.
import { test } from "node:test";
import assert from "node:assert/strict";
import { Pad, CMD } from "../pad.js";
import { simulatedTool } from "./sim-tool.mjs";

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
