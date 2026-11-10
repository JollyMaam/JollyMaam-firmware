// pad.js against a simulated pad in config mode: profile_config.c's framing
// (frames collected while bit 7 is set, answers in pieces of 157 bytes) and
// usb.c's IDENTIFY, STATUS, FIELDS (settings.c's set_describe) and READ.
import { test } from "node:test";
import assert from "node:assert/strict";
import { Pad, decodeField, showValue, parseValue, targetsFor, slotLabel, T } from "../pad.js";
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
        codes.push(data[o] >= 19 ? 1 : data[o + 1] === 0x99 ? 3 : 0);
        if (!codes.at(-1)) link.staged.push([data[o], data[o + 1], data.slice(o + 3, o + 3 + l)]);
        o += 3 + l;
      }
      result = Uint8Array.of(codes.length, ...codes);
      return new Uint8Array(0);
    }
    if (sub === 0x23) link.committed.push(...link.staged.splice(0));
    if (sub === 0x24) link.staged.length = 0, link.drops++;
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
    if (sub === 0x06) {          // LAYOUT, at most 2 entries an answer
      const all = [[0, 0, "B"], [1, 0, "A"], [32, 4, ""]];
      const here = all.slice(arg, arg + 2);
      return cat([1, 12, all.length, here.length], ...here.map(([i, k, l]) => cat([i, k], enc(l.padEnd(8, "\0")), [0, 0])));
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
  let reply = () => {}, held = [], collecting = null;
  const link = {
    name: "Cyclone 2 Config", id: "sim", maxWrite: 96, frames: [], staged: [], committed: [], drops: 0,
    onReply(fn) { reply = fn; link.reply = (d) => reply(d); },
    onDisconnect() {},
    close() {},
    // profile_config.c: u8 sub (bit 7 more), u32 tag, u32 arg, data; the
    // answer's first piece u8 1, u8 number, u32 tag, u16 length, data; the
    // next u8 index << 1, u8 number, data; refused: u8 3, u32 tag.
    async write(frame) {
      assert.ok(frame.length <= 96, "a frame is at most BT_WRITE_MAX");
      link.frames.push(frame);
      const v = new DataView(frame.buffer, frame.byteOffset), tag = v.getUint32(1, true);
      const send = (d) => (link.emit ?? reply)(d), tagged = (h, extra = []) => cat([h, tag & 0xff, (tag >> 8) & 0xff, (tag >> 16) & 0xff, tag >>> 24], extra);
      if (link.drop) { link.drop--; return; }                    // lost on the way, or its refusal
      if (link.busy || (collecting !== null && collecting !== tag)) {   // another program's command runs
        setTimeout(() => send(tagged(3)), 0);
        return;
      }
      collecting = tag;
      held.push(frame.subarray(9));
      if (frame[0] & 0x80) return;
      collecting = null;
      const sub = frame[0], arg = v.getUint32(5, true);
      const data = cat(...held);
      held = [];
      link.last = { sub, arg, data, tag };
      const body = answer(sub, arg, data), pieces = [], number = link.number = ((link.number ?? 0) + 1) & 0xff;
      for (let o = 0; ; ) {
        const room = pieces.length ? PIECE - 1 : PIECE - 7;
        pieces.push(pieces.length ? cat([pieces.length << 1, number], body.subarray(o, o + room))
          : cat([1, number], tagged(4).subarray(1), u16(body.length), body.subarray(0, room)));
        o += room;
        if (o >= body.length) break;
      }
      setTimeout(() => {         // notifications come after the write's response
        const lose = link.lose ?? [];
        link.lose = [];          // once
        pieces.forEach((p, i) => { if (!lose.includes(i)) send(p); });
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
  assert.equal(pad.frameData, 87);
  const s = await pad.status();
  assert.equal(s.state, "connected");
  assert.equal(s.slot, "east");
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
  assert.equal(await writeTest(pad, 11, 488, (l) => lines.push(l)), true, lines.join("\n"));
  assert.equal(await writeTest(pad, 87, 488, (l) => lines.push(l)), true, lines.join("\n"));
});

test("values from text, as showValue shows them", () => {
  const f = (type, size, extra = {}) => ({ name: "x", type, size, max: 0, names: [], ...extra });
  const round = (field, text) => showValue(field, parseValue(field, text));
  assert.equal(round(f(T.ENUM, 1, { names: ["none", "", "switch"], max: 2 }), "switch"), "switch");
  assert.throws(() => parseValue(f(T.ENUM, 1, { names: ["none", "", "switch"] }), ""), /one of none, switch/);
  assert.equal(round(f(T.NUMBER, 2, { max: 3200 }), "3200"), "3200");
  assert.throws(() => parseValue(f(T.NUMBER, 2, { max: 3200 }), "3201"), /at most 3200/);
  assert.throws(() => parseValue(f(T.NUMBER, 2), "-1"), /not a number/);
  assert.deepEqual(Array.from(parseValue(f(T.NUMBER, 4), "70000")), [0x70, 0x11, 1, 0]);
  assert.equal(round(f(T.RGB, 3), "#ff3300"), "#ff3300");
  assert.equal(round(f(T.ENTRY, 1), "15"), "15");
  assert.throws(() => parseValue(f(T.ENTRY, 1), "16"), /at most 15/);
  assert.equal(round(f(T.TEXT, 12), "Couch"), "Couch");
  assert.throws(() => parseValue(f(T.TEXT, 4), "Couch"), /at most 4 bytes/);
  assert.equal(round(f(T.LIST, 3, { max: 100 }), "1, 2 3"), "1,2,3");
  assert.throws(() => parseValue(f(T.LIST, 3, { max: 100 }), "1,2"), /3 numbers/);
  assert.throws(() => parseValue(f(T.BYTES, 6), "00"), /not editable/);
});

test("write: pieces, each checked, then commit", async () => {
  const link = simulatedPad();
  const pad = new Pad(link);
  await pad.identify();
  const items = Array.from({ length: 50 }, (_, i) => ({ target: i % 8, id: 3, value: new Uint8Array(12).fill(i) }));
  await pad.write(items);     // 50 x 15 bytes: two WRITEs of 488 at most
  assert.equal(link.committed.length, 50);
  assert.deepEqual(Array.from(link.committed[49][2]), Array(12).fill(49));
  await pad.write([{ target: 1, id: 2, value: new Uint8Array(0) }]);
  assert.deepEqual(link.committed.at(-1), [1, 2, new Uint8Array(0)]);
});

test("write: a refusal drops everything", async () => {
  const link = simulatedPad();
  const pad = new Pad(link);
  await pad.identify();
  const error = await pad.write([{ target: 0, id: 1, value: Uint8Array.of(2) }, { target: 0, id: 0x99, value: Uint8Array.of(9) }])
    .then(() => null, (e) => e);
  assert.match(error.message, /out of range/);
  assert.deepEqual(error.refused.map((r) => [r.target, r.id, r.why]), [[0, 0x99, "out of range"]]);
  assert.equal(link.committed.length, 0);
  assert.equal(link.drops, 1);
});

test("layout: slots by position, with the printed label", async () => {
  const pad = new Pad(simulatedPad());
  await pad.identify();
  const layout = await pad.layout();
  assert.deepEqual(layout.map((e) => [e.input, e.label]), [[0, "B"], [1, "A"], [32, ""]]);
  assert.deepEqual([0, 1, 2, 4].map((i) => slotLabel(layout, i)), ["B (south)", "A (east)", "west", "up"]);
  assert.equal(slotLabel(null, 3), "north");
});

test("remap fields: map, rules and curve points as padtool's text", () => {
  const field = (name, size) => ({ kind: 3, type: T.BYTES, name, size, max: 0, names: [] });
  const map = field("map", 20), rules = field("rules", 40), curve = field("left_curve_points", 15);
  assert.equal(showValue(map, parseValue(map, "swap-faces")), "south=east east=south west=north north=west");
  assert.equal(showValue(rules, parseValue(rules, "r4 held turbo south@max")), "r4 held turbo south@max");
  assert.throws(() => parseValue(rules, Array(5).fill("south held hold east").join("; ")), /rules: at most 4 rules/);
  assert.equal(showValue(curve, parseValue(curve, "linear")), "16,32,48,64,80,96,112,128,143,159,175,191,207,223,239");
  assert.throws(() => parseValue(curve, "1,2"), /left_curve_points: a curve needs 15/);
  assert.throws(() => parseValue({ ...map, kind: 0, name: "address", size: 6 }, "00"), /not editable/);
});

test("copies of every piece (another program subscribed) are dropped", async () => {
  const link = simulatedPad();
  link.emit = (d) => { link.reply(d); link.reply(d); };   // BlueZ: one more per other subscriber
  const pad = new Pad(link);
  const id = await pad.identify();
  assert.equal(id.build, "abc1234");
  const table = await pad.fields();                        // FIELDS: two pieces, each twice
  assert.deepEqual(table.map((f) => f.name), ["type", "sleep_ms", "red", "name"]);
  link.emit = (d) => { for (let i = 0; i < 4; i++) link.reply(d); };
  assert.equal((await pad.status()).state, "connected");
});

test("a command refused while another program's runs goes again", async () => {
  const link = simulatedPad();
  const pad = new Pad(link);
  await pad.identify();
  link.busy = true;
  setTimeout(() => { link.busy = false; }, 60);
  const before = link.frames.length;
  assert.equal((await pad.status()).state, "connected");
  assert.ok(link.frames.length - before > 1, "sent more than once");
  const tags = link.frames.slice(before).map((f) => new DataView(f.buffer, f.byteOffset).getUint32(1, true));
  assert.ok(tags.every((t, i) => !i || t !== tags[i - 1]), "a new tag each time");
});

test("another program's answers (other tags) are not taken", async () => {
  const link = simulatedPad();
  const pad = new Pad(link);
  await pad.identify();
  const real = link.emit;
  link.emit = (d) => {
    if (d[0] === 1) {            // before ours: the end of another's answer, another's first piece, a refusal of another's
      link.reply(Uint8Array.of(2, d[1] - 1, 0xee, 0xee));
      link.reply(Uint8Array.of(1, 0x77, d[2] ^ 1, d[3], d[4], d[5], 2, 0, 0xee, 0xee));
      link.reply(Uint8Array.of(3, d[2] ^ 1, d[3], d[4], d[5]));
    }
    link.reply(d);
  };
  assert.equal((await pad.status()).state, "connected");
  link.emit = real;
});

test("a piece lost: the command goes again, never a wrong answer", async () => {
  const link = simulatedPad();
  const pad = new Pad(link);
  await pad.identify();
  const whole = await pad.fields();
  link.lose = [1, 2];                                      // two: a parity bit would miss them
  assert.deepEqual((await pad.fields()).map((f) => f.name), whole.map((f) => f.name));
});

test("a command nothing comes of (it or its refusal lost) goes again", async () => {
  const link = simulatedPad();
  const pad = new Pad(link);
  await pad.identify();
  link.drop = 1;
  const t0 = Date.now();
  assert.equal((await pad.status()).state, "connected");
  assert.ok(Date.now() - t0 >= 450, "after the silence");
});
