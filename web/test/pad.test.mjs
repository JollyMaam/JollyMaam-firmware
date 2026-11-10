// pad.js against a simulated pad in config mode: profile_config.c's framing
// (frames collected while bit 7 is set, answers in pieces of 157 bytes) and
// usb.c's IDENTIFY, STATUS, FIELDS (settings.c's set_describe) and READ.
import { test } from "node:test";
import assert from "node:assert/strict";
import { Pad, decodeField, showValue, targetsFor, T } from "../pad.js";
import { echoItems, writeTest } from "../linktest.js";

const PIECE = 157;               // CONFIG_REPLY_MAX - 1

const enc = (s) => new TextEncoder().encode(s);
const cat = (...parts) => {
  const out = new Uint8Array(parts.reduce((n, p) => n + p.length, 0));
  parts.reduce((o, p) => (out.set(p, o), o + p.length), 0);
  return out;
};
const u16 = (v) => [v & 0xff, v >> 8];

// set_describe(): version, kind, id, type, size, flags, u16 max, name, help,
// an enum's names, then defaults (8 for a slot field) when size < 4.
function describe(f) {
  const strings = [f.name, f.help, ...(f.names || [])].map((s) => cat(enc(s), [0]));
  const defs = f.size < 4 ? (f.defs || []).map((d) => Uint8Array.from({ length: f.size }, (_, i) => (d >> (8 * i)) & 0xff)) : [];
  return cat([1, f.kind, f.id, f.type, f.size, f.flags || 0, ...u16(f.max || 0)], ...strings, ...defs);
}

const FIELDS = [
  { kind: 0, id: 1, type: T.ENUM, size: 1, max: 2, name: "type", help: "what it is", names: ["none", "", "switch"], defs: [2, 0, 0, 0, 0, 0, 0, 0] },
  { kind: 0, id: 2, type: T.NUMBER, size: 4, max: 0, name: "sleep_ms", help: "", flags: 1 },
  { kind: 1, id: 1, type: T.RGB, size: 3, name: "red", help: "a colour", defs: [0xff0000] },
  { kind: 0, id: 3, type: T.TEXT, size: 12, name: "name", help: "x".repeat(150) },   // makes FIELDS take two pieces
];

function simulatedPad() {
  const values = { 0: [[1, 1, Uint8Array.of(2)], [2, 0, Uint8Array.of(0x10, 0x27, 0, 0)], [3, 1, cat(enc("Couch"), new Uint8Array(7))]] };
  let result = new Uint8Array(1);
  const answer = (sub, arg, data) => {
    if (sub === 0x22) {          // settings.c's set_write(), for targets past the last
      const codes = [];
      for (let o = 0; o + 3 <= data.length; ) {
        const l = data[o + 2];
        if (o + 3 + l > data.length) { codes.push(2); break; }
        codes.push(data[o] >= 19 ? 1 : 0);
        o += 3 + l;
      }
      result = Uint8Array.of(codes.length, ...codes);
      return new Uint8Array(0);
    }
    if (sub === 0x26) return result;
    if (sub === 0x00) {
      const d = new Uint8Array(90);
      d.set(enc("C2PD"));
      d.set(u16(2), 4);
      d.set(enc("abc1234"), 8);
      d.set(Array.from({ length: 16 }, (_, i) => i), 24);
      d.set(enc("CYCLONE2"), 40);
      d.set(enc("Cyclone 2"), 56);
      d.set(u16(488), 88);
      return d;
    }
    if (sub === 0x01) return Uint8Array.of(4, 1, 0, 0, ...u16(3900), 1 | 2 | 4 | 8 | 16, 80, 0x10, 0x27, 0, 0, 0, 0, 7, 0);
    if (sub === 0x28) {          // as many as fit in 512, from arg
      let out = [Uint8Array.of(0)], n = 1, k = 0;
      for (const f of FIELDS.slice(arg)) {
        const e = describe(f);
        if (n + 2 + e.length > 512) break;
        out.push(Uint8Array.from(u16(e.length)), e);
        n += 2 + e.length, k++;
      }
      if (!k) return new Uint8Array(0);
      out[0] = Uint8Array.of(k);
      return cat(...out);
    }
    if (sub === 0x21) {
      const v = values[arg] || [];
      return cat([1, arg, v.length], ...v.map(([id, set, value]) => cat([id, set, value.length], value)));
    }
    return new Uint8Array(0);
  };
  let reply = () => {}, held = [];
  const link = {
    name: "Cyclone 2 Config", id: "sim", maxWrite: 96, frames: [],
    onReply(fn) { reply = fn; },
    onDisconnect() {},
    close() {},
    async write(frame) {
      assert.ok(frame.length <= 96, "a frame is at most BT_WRITE_MAX");
      link.frames.push(frame);
      held.push(frame.subarray(5));
      if (frame[0] & 0x80) return;
      const sub = frame[0], arg = new DataView(frame.buffer, frame.byteOffset).getUint32(1, true);
      const data = cat(...held);
      held = [];
      link.last = { sub, arg, data };
      const a = answer(sub, arg, data);
      if (data.length) return;   // profile_config_service(): a command with data has no answer
      setTimeout(() => {         // notifications come after the write's response
        for (let o = 0; ; o += PIECE) {
          const more = o + PIECE < a.length;
          reply(cat([sub | (more ? 0x80 : 0)], a.subarray(o, o + PIECE)));
          if (!more) break;
        }
      }, 1);
    },
  };
  return link;
}

test("identify and status", async () => {
  const pad = new Pad(simulatedPad());
  const id = await pad.identify();
  assert.equal(id.boardName, "Cyclone 2");
  assert.equal(id.product, "CYCLONE2");
  assert.equal(id.build, "abc1234");
  assert.equal(id.flashId, "000102030405060708090a0b0c0d0e0f");
  assert.equal(id.updater, false);
  assert.equal(id.dataMax, 488);
  assert.equal(pad.frameData, 91);
  const s = await pad.status();
  assert.equal(s.state, "connected");
  assert.equal(s.slot, "B");
  assert.equal(s.mv, 3900);
  assert.equal(s.percent, 80);
  assert.equal(s.charging, "charging");
  assert.equal(s.uptime, 10);
  assert.equal(s.changes, 7);
});

test("fields and values", async () => {
  const pad = new Pad(simulatedPad());
  await pad.identify();
  const table = await pad.fields();
  assert.deepEqual(table.map((f) => f.name), ["type", "sleep_ms", "red", "name"]);
  const [type, sleep, red] = table;
  assert.deepEqual(type.names, ["none", "", "switch"]);
  assert.deepEqual(type.defaults.map((d) => d[0]), [2, 0, 0, 0, 0, 0, 0, 0]);
  assert.equal(sleep.dev, true);
  assert.deepEqual(sleep.defaults, []);
  assert.deepEqual(Array.from(red.defaults[0]), [0, 0, 0xff]);   // little-endian, as the firmware copies it
  assert.deepEqual(targetsFor(table), [...Array(10).keys()]);
  const v = await pad.read(table, 0);
  assert.equal(showValue(type, v.get("type").value), "switch");
  assert.equal(v.get("type").set, true);
  assert.equal(showValue(sleep, v.get("sleep_ms").value), "10000");
  assert.equal(showValue(table[3], v.get("name").value), "Couch");
});

test("data goes in frames, the last without bit 7", async () => {
  const link = simulatedPad();
  const pad = new Pad(link);
  await pad.identify();
  link.frames.length = 0;
  const data = Uint8Array.from({ length: 200 }, (_, i) => i);
  await pad.cmd(0x22, { arg: 5, data });
  assert.deepEqual(link.frames.map((f) => f[0]), [0xa2, 0xa2, 0x22]);
  assert.deepEqual(Array.from(link.last.data), Array.from(data));
  assert.equal(link.last.arg, 5);
});

test("commands run one at a time", async () => {
  const pad = new Pad(simulatedPad());
  await pad.identify();
  const [a, b] = await Promise.all([pad.status(), pad.identify()]);
  assert.equal(a.state, "connected");
  assert.equal(b.boardName, "Cyclone 2");
});

test("enum names the board does not take stay empty", () => {
  const f = decodeField(describe(FIELDS[0]));
  assert.equal(showValue(f, Uint8Array.of(1)), "1");
});

test("the write test's items fill the data exactly", async () => {
  for (const size of [488, 100, 3, 25]) {
    const { data, items } = echoItems(size);
    let o = 0, n = 0;
    while (o < data.length) o += 3 + data[o + 2], n++;
    assert.equal(o, size);
    assert.equal(n, items);
  }
  const pad = new Pad(simulatedPad());
  await pad.identify();
  const lines = [];
  assert.equal(await writeTest(pad, 15, 488, (l) => lines.push(l)), true, lines.join("\n"));
  assert.equal(await writeTest(pad, 91, 488, (l) => lines.push(l)), true, lines.join("\n"));
});
