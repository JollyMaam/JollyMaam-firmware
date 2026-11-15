// The tool commands over a link (link-web.js): framing, then the commands
// and their answers.  The wire is src/lib/proto_jollymaam_wire.h and
// src/profiles/profile_config.c; padtool (tools/padtool) speaks the same.

import { showMap, parseMap, showRules, parseRules, parseCurve, RULE_SIZE } from "./remap.js";
import { HidCommands, dataMax as hidDataMax, commandMax as hidCommandMax } from "./frames-hid.js";

export const PROTOCOL = 2;
export const MORE = 0x80;
const FRAME_HEAD = 9;            // u8 sub-command, u32 tag, u32 argument
const FIRST = 0x01, REFUSAL = 0x03;   // a piece's u8 header, else index << 1 (profile_config.c)
const REFUSED = Symbol("refused"), SILENT = Symbol("silent"), LOST = Symbol("lost");
const LOSSES_MAX = 5;            // answers with a piece lost this often: give up
const REFUSALS_MAX = 100;        // refused this often (10-50 ms apart): give up
const SILENT_MS = 500;           // nothing of a command (config mode) this long: send it again
const CONFIG_FRAME_DATA = 96 - FRAME_HEAD;   // BT_WRITE_MAX less the head
const CONFIG_DATA_MAX = 488;     // IDENTIFY's u16 at 88 from firmware that has it

export const CMD = {
  IDENTIFY: 0x00, STATUS: 0x01, LAYOUT: 0x06, REBOOT: 0x10, READ: 0x21, WRITE: 0x22, COMMIT: 0x23, DROP: 0x24, RESULT: 0x26, DEFAULTS: 0x27,
  FIELDS: 0x28, UPDATER: 0x40, UPDATE_INFO: 0x41, UPDATE_BEGIN: 0x42, UPDATE_DATA: 0x43, UPDATE_COMMIT: 0x44,
};

export const UPDATE_ERRORS = ["", "bad size", "past the end", "not begun", "verify failed (read back differs)",
  "incomplete", "bad header", "CRC mismatch", "no chip key fits", "the image is for another board",
  "the image is for another updater layout"];

export const STATES = ["boot", "idle", "connecting", "pairing", "connected", "stopping", "sleeping", "rebooting"];
// Slots by the key that picks them: positions, not printed letters.
export const SLOT_NAMES = ["south", "east", "west", "north", "up", "down", "left", "right"];
export const TARGETS = [
  ...SLOT_NAMES.map((n) => "slot-" + n), "palette", "calibration",
  ...SLOT_NAMES.map((n) => "remap-" + n), "remap-base",
];
export const REMAP_TARGET = 10, BASE_TARGET = 18;
export const KINDS = ["slot", "palette", "calibration", "remap"];
export const T = { ENUM: 0, NUMBER: 1, RGB: 2, BYTES: 3, TEXT: 4, ENTRY: 5, LIST: 6 };
export const RESULTS = ["ok", "unknown setting", "bad length", "out of range", "full"];
export const PALETTE_TARGET = 8, CALIBRATION_TARGET = 9, PALETTE = 16;

// Over USB a command asks for its answer's size (the pad zero-pads to
// it): padtool's sizes; the rest have none.
const USB_ANSWER = {
  [CMD.IDENTIFY]: 88, [CMD.STATUS]: 16, [CMD.LAYOUT]: 4 + 12 * 20, [CMD.READ]: 512, [CMD.DEFAULTS]: 512,
  [CMD.FIELDS]: 512, [CMD.RESULT]: 256, [CMD.UPDATE_INFO]: 32,
};

// The input (IN_*, src/core/pad.h) of the key that picks each slot.
const SLOT_INPUTS = [0, 1, 3, 4, 16, 18, 19, 17];

// A slot, connection or remap, by its key: the position, after the pad's
// printed label where it has one ("B (south)").  layout: Pad.layout().
export function slotLabel(layout, slot) {
  const role = SLOT_NAMES[slot];
  const printed = layout?.find((e) => e.input === SLOT_INPUTS[slot] && e.label)?.label;
  return printed && printed.toLowerCase() !== role ? `${printed} (${role})` : role;
}

export const kindOf = (t) => (t < 8 ? 0 : t >= REMAP_TARGET ? 3 : t - 7);

// The targets a table describes: the palette and remap slots only where it
// has their fields (one-colour lights have no palette), the base remap
// before the remap slots; palette and calibration last (rarely touched).
export function targetsFor(table) {
  const has = (kind) => table.some((f) => f.kind === kind);
  const slots = [...Array(PALETTE_TARGET).keys()];
  const remaps = has(3) ? [BASE_TARGET, ...Array.from({ length: BASE_TARGET - REMAP_TARGET }, (_, i) => REMAP_TARGET + i)] : [];
  return [...slots, ...remaps, ...(has(1) ? [PALETTE_TARGET] : []), CALIBRATION_TARGET];
}

const u16 = (d, o) => d[o] | (d[o + 1] << 8);
const u32 = (d, o) => (d[o] | (d[o + 1] << 8) | (d[o + 2] << 16) | (d[o + 3] << 24)) >>> 0;
const text = (d) => new TextDecoder().decode(d.subarray(0, (d.indexOf(0) + 1 || d.length + 1) - 1));
const hex = (d) => Array.from(d, (b) => b.toString(16).padStart(2, "0")).join("");

// ---- answers -------------------------------------------------------------

export function decodeIdentify(d) {
  if (d.length < 56) throw new Error(`short IDENTIFY (${d.length} bytes)`);
  if (text(d.subarray(0, 4)) !== "C2PD") throw new Error("IDENTIFY does not start with C2PD");
  return {
    protocol: u16(d, 4),
    board: d[6],
    updater: d[7] === 1,
    build: text(d.subarray(8, 24)),
    flashId: hex(d.subarray(24, 40)),
    product: text(d.subarray(40, 56)),
    boardName: d.length >= 88 ? text(d.subarray(56, 88)) : text(d.subarray(40, 56)),
    dataMax: (d.length >= 90 && u16(d, 88)) || CONFIG_DATA_MAX,
  };
}

const BATTERY = { MV: 1, PERCENT: 2, CHARGE: 4, EXTERNAL: 8, CHARGING: 16, LOW_VALID: 32, LOW: 64, FULL: 128 };

export function decodeStatus(d) {
  if (d.length < 16) throw new Error("short STATUS");
  const flags = d[6];
  return {
    state: STATES[d[0]] ?? String(d[0]),
    slot: d[1] === 0xff ? null : SLOT_NAMES[d[1]] ?? String(d[1]),
    usb: !!d[2],
    mv: flags & BATTERY.MV ? u16(d, 4) : null,
    percent: flags & BATTERY.PERCENT ? d[7] : null,
    charging: !(flags & BATTERY.CHARGE) ? null
      : flags & BATTERY.CHARGING ? "charging" : flags & BATTERY.FULL ? "full"
      : flags & BATTERY.EXTERNAL ? "not charging" : "on battery",
    low: flags & BATTERY.LOW_VALID ? !!(flags & BATTERY.LOW) : !!d[3],
    uptime: u32(d, 8) / 1000,
    ephemeral: !!d[12],
    staged: d[13],
    changes: u16(d, 14),
  };
}

// One FIELD entry: u8 ?, kind, id, type, size, flags, u16 max, then the
// name, the help, an enum's names (NUL-terminated) and the defaults.
export function decodeField(d) {
  const [, kind, id, type, size, flags] = d;
  const max = u16(d, 6);
  let o = 8;
  const string = () => {
    const end = d.indexOf(0, o);
    const v = new TextDecoder().decode(d.subarray(o, end));
    o = end + 1;
    return v;
  };
  const name = string(), help = string();
  const names = type === T.ENUM ? Array.from({ length: max + 1 }, string) : [];   // "": not on this board
  const count = kind === 0 ? 8 : 1;
  const defaults = size < 4 ? Array.from({ length: count }, (_, i) => d.slice(o + i * size, o + (i + 1) * size)) : [];
  return { kind, id, type, size, max, dev: !!(flags & 1), name, help, names, defaults };
}

// FIELDS: u8 count, then each entry behind its u16 size.
export function decodeFields(d) {
  const out = [];
  let o = 1;
  for (let i = 0; i < d[0]; i++) {
    const size = u16(d, o);
    out.push(decodeField(d.subarray(o + 2, o + 2 + size)));
    o += 2 + size;
  }
  return out;
}

// READ / DEFAULTS: u8, u8, u8 count, then each u8 id, u8 set, u8 size, value.
export function decodeValues(d, table, target) {
  const byId = new Map();
  if (d.length >= 3) {
    let o = 3;
    for (let i = 0; i < d[2]; i++) {
      const [id, set, size] = d.subarray(o, o + 3);
      byId.set(id, { value: d.slice(o + 3, o + 3 + size), set: !!set });
      o += 3 + size;
    }
  }
  const out = new Map();
  for (const f of table) if (f.kind === kindOf(target) && byId.has(f.id)) out.set(f.name, byId.get(f.id));
  return out;
}

// What a remap slot's raw-bytes field is: "map", "rules", "curve" or null.
export function remapPart(f) {
  if (f.kind !== 3 || f.type !== T.BYTES) return null;
  return f.name === "map" ? "map" : f.name === "rules" ? "rules" : f.name.endsWith("_curve_points") ? "curve" : null;
}

export function showValue(f, raw) {
  switch (remapPart(f)) {
    case "map": return showMap(raw);
    case "rules": return showRules(raw);
    case "curve": return Array.from(raw).join(",");
  }
  switch (f.type) {
    case T.TEXT: return text(raw);
    case T.ENUM: return f.names[raw[0]] || String(raw[0]);
    case T.NUMBER: return String(raw.reduceRight((v, b) => v * 256 + b, 0));
    case T.ENTRY: return String(raw[0]);
    case T.RGB: return "#" + hex(raw);
    case T.LIST: return Array.from(raw).join(",");
  }
  if (f.name === "address") return raw.some((b) => b) ? hex(raw.slice().reverse()).match(/../g).join(":") : "derived";
  return raw.some((b) => b) ? hex(raw) : "none";
}

// Whether the page edits a field: all but the other raw bytes (address,
// bond, calibration).
export const editable = (f) => f.type !== T.BYTES || !!remapPart(f);

// A field's bytes from the text an editor holds (showValue's form); throws
// what is wrong.  The pad checks ranges again (RESULT).
export function parseValue(f, s) {
  s = s.trim();
  const fail = (why) => { throw new Error(`${f.name}: ${why}`); };
  const part = remapPart(f);
  if (part) {
    try {
      return part === "map" ? parseMap(s) : part === "rules" ? parseRules(s, f.size / RULE_SIZE) : parseCurve(s, f.size);
    } catch (error) {
      fail(error.message);
    }
  }
  const number = (x, max) => {
    if (!/^(0x[0-9a-f]+|\d+)$/i.test(x)) fail(`not a number: ${JSON.stringify(x)}`);
    const v = Number(x);
    if (max !== undefined && v > max) fail(`at most ${max}`);
    return v;
  };
  switch (f.type) {
    case T.ENUM: {
      const i = f.names.indexOf(s);
      if (!s || i < 0) fail(`one of ${f.names.filter(Boolean).join(", ")}`);
      return Uint8Array.of(i);
    }
    case T.NUMBER: {
      const v = number(s, f.max || 2 ** (8 * Math.min(f.size, 4)) - 1);
      return Uint8Array.from({ length: f.size }, (_, i) => Math.floor(v / 256 ** i) & 0xff);
    }
    case T.ENTRY: return Uint8Array.of(number(s, PALETTE - 1));
    case T.RGB: {
      const m = /^#?([0-9a-f]{6})$/i.exec(s);
      if (!m) fail("a colour #rrggbb");
      return Uint8Array.from(m[1].match(/../g), (x) => parseInt(x, 16));
    }
    case T.TEXT: {
      const raw = new TextEncoder().encode(s);
      if (raw.length > f.size) fail(`at most ${f.size} bytes`);
      const out = new Uint8Array(f.size);
      out.set(raw);
      return out;
    }
    case T.LIST: {
      const v = s.split(/[\s,]+/).filter(Boolean);
      if (v.length !== f.size) fail(`${f.size} numbers, not ${v.length}`);
      return Uint8Array.from(v, (x) => number(x, f.max));
    }
  }
  fail("not editable here");
}

// LAYOUT: u8 version, entry size, count, entries here, then each u8 input,
// kind, label[8], x, y (struct board_control).
export function decodeLayout(d) {
  if (d.length < 4 || d[0] !== 1 || d[1] < 12) return null;
  const [, size, count, here] = d;
  const entries = [];
  for (let i = 0; i < here && 4 + (i + 1) * size <= d.length; i++) {
    const e = d.subarray(4 + i * size, 4 + (i + 1) * size);
    entries.push({ input: e[0], kind: e[1], label: text(e.subarray(2, 10)), x: e[10], y: e[11] });
  }
  return { count, entries };
}

// UPDATE_INFO (the updater's): version, state, error, valid, then u32 at,
// room, written, begun, key, data_max.
export function decodeUpdateInfo(d) {
  if (d.length < 28) throw new Error("short UPDATE_INFO");
  return { state: d[1], error: d[2], valid: !!d[3], at: u32(d, 4), room: u32(d, 8), written: u32(d, 12),
    begun: u32(d, 16), key: u32(d, 20), dataMax: u32(d, 24) };
}

// ---- commands ------------------------------------------------------------

// A pad over a link: Bluetooth (link-web.js: the frames below) or USB
// (link-hid.js: frames-hid.js), the same commands either way.
export class Pad {
  constructor(link) {
    this.link = link;
    this.usb = link.kind === "hid" ? new HidCommands(link) : null;
    this.frameData = this.usb ? hidDataMax(link) : link.maxWrite - FRAME_HEAD;
    this.pending = null;
    this.maxReply = 0;           // the longest notification seen: the MTU less 3, at most
    this.chain = Promise.resolve();
    if (!this.usb) link.onReply((r) => this.#reply(r));
    link.onDisconnect(() => {
      this.gone = true;
      this.pending?.reject(new Error("link lost"));
      this.onDisconnect?.();
    });
  }

  // A piece (profile_config.c): the first u8 FIRST, u8 number, u32 tag,
  // u16 the answer's length, data; the next u8 index << 1, u8 number,
  // data; a refusal u8 REFUSAL, u32 tag.  A first piece or refusal with
  // another tag is another program's; after our first, only our answer's
  // number with the next index: a smaller one is a copy (BlueZ sends one
  // more per other program subscribed), a bigger one a piece lost: the
  // command goes again (LOST), so an answer is whole or none.
  #reply(r) {
    this.maxReply = Math.max(this.maxReply, r.length);
    const p = this.pending;
    if (!p || r.length < 2) return;
    if (r[0] === REFUSAL) {
      if (r.length >= 5 && u32(r, 1) === p.tag) p.refused();
      return;
    }
    let d;
    if (r[0] === FIRST) {
      if (p.length !== null || r.length < 8 || u32(r, 2) !== p.tag) return;
      p.number = r[1];
      p.length = u16(r, 6);
      p.answer = new Uint8Array(p.length);
      d = r.subarray(8);
    } else {
      if (p.length === null || r[0] & 1 || r[1] !== p.number) return;
      const index = r[0] >> 1;
      if (index < p.index) return;                   // a copy
      if (index > p.index) return p.lost();          // one before it never came
      d = r.subarray(2);
      p.index++;
    }
    p.progress?.();
    d = d.subarray(0, p.length - p.got);
    p.answer.set(d, p.got);
    p.got += d.length;
    if (p.got < p.length) return;
    this.pending = null;
    p.resolve(p.answer);
  }

  // A tag for the next command: 32 random bits, so another program's is
  // as good as never the same.
  #nextTag() {
    return crypto.getRandomValues(new Uint32Array(1))[0];
  }

  // A command: its data in frames, then its answer.  Config mode answers
  // every command (one with data, none, with nothing) or refuses it while
  // another program's runs: then it goes again, with a new tag, after a
  // short wait.  The updater answers a command without data in one piece,
  // or not at all when the answer does not fit its MTU, and none with data
  // or without an answer (UPDATE_COMMIT, REBOOT: answer false).  One at a
  // time.  quick: written without response (no pacing; the caller checks
  // the count).
  // size: over USB, the answer's (USB_ANSWER by default).
  cmd(sub, { arg = 0, data = new Uint8Array(0), timeoutMs = 3000, answer = !(this.id?.updater && data.length && !this.usb),
    quick = false, size = USB_ANSWER[sub] ?? 0 } = {}) {
    const run = () => this.usb ? this.#usb(sub, arg, data, size, timeoutMs, answer)
      : this.#run(sub, arg, data, timeoutMs, answer, quick);
    const result = this.chain.then(run, run);
    this.chain = result.catch(() => {});
    return result;
  }

  async #usb(sub, arg, data, size, timeoutMs, wanted) {
    if (this.gone) throw new Error("link lost");
    return this.usb.run(sub, arg, data, size, timeoutMs, wanted);
  }

  async #run(sub, arg, data, timeoutMs, wanted, quick) {
    for (let tries = 1, silent = 0, lost = 0; ; tries++) {
      const got = await this.#once(sub, arg, data, timeoutMs, wanted, quick, silent < 2);
      if (got === SILENT) { silent++; continue; }
      if (got === LOST) {
        if (++lost >= LOSSES_MAX) throw new Error(`command 0x${sub.toString(16)}: a piece of its answer lost ${lost} times`);
        continue;
      }
      if (got !== REFUSED) return got;
      if (tries >= REFUSALS_MAX) throw new Error(`command 0x${sub.toString(16)} refused ${tries} times: another program keeps the pad busy`);
      await new Promise((r) => setTimeout(r, 10 + Math.random() * 40));
    }
  }

  // One try.  watch: config mode, and resends left: no first piece nor
  // refusal within SILENT_MS is SILENT (the command or its refusal lost,
  // with another program busy on the pad), an answer stopping that long
  // LOST (its last pieces lost).
  async #once(sub, arg, data, timeoutMs, wanted, quick, watch) {
    if (this.gone) throw new Error("link lost");
    const tag = this.#nextTag();
    const answer = new Promise((resolve, reject) => {
      this.pending = { sub, tag, index: 1, number: null, length: null, got: 0, resolve, reject,
        refused: () => resolve(REFUSED), lost: () => { this.pending = null; resolve(LOST); } };
    });
    answer.catch(() => {});
    let timer;
    try {
      const n = Math.max(1, Math.ceil(data.length / this.frameData));
      for (let i = 0; i < n; i++) {
        const piece = data.subarray(i * this.frameData, (i + 1) * this.frameData);
        const frame = new Uint8Array(FRAME_HEAD + piece.length);
        frame[0] = sub | (i < n - 1 ? MORE : 0);
        const v = new DataView(frame.buffer);
        v.setUint32(1, tag, true);
        v.setUint32(5, arg >>> 0, true);
        frame.set(piece, FRAME_HEAD);
        await this.link.write(frame, quick);
      }
      if (!wanted) return new Uint8Array(0);
      const p = this.pending;
      let silence = null;
      try {
        return await Promise.race([answer, new Promise((resolve, reject) => {
          if (watch && !this.id?.updater) {
            p.progress = () => {
              clearTimeout(silence);
              silence = setTimeout(() => {
                if (this.pending === p) this.pending = null;
                resolve(p.length === null ? SILENT : LOST);
              }, SILENT_MS);
            };
            p.progress();
          }
          timer = setTimeout(() => reject(new Error(`no answer to command 0x${sub.toString(16)} in ${timeoutMs} ms`)), timeoutMs);
        })]);
      } finally {
        clearTimeout(silence);
      }
    } finally {
      clearTimeout(timer);
      this.pending = null;
    }
  }

  // A command that restarts the pad, then the link gone (at most 5 s).  No
  // subscription is left behind (link-web.js).
  async restart(sub) {
    const gone = new Promise((resolve) => {
      const was = this.onDisconnect;
      this.onDisconnect = () => { was?.(); resolve(); };
    });
    await this.link.quiet?.();
    await Promise.race([this.cmd(sub, { answer: false }).catch(() => {}), gone]);
    await Promise.race([gone, new Promise((r) => setTimeout(r, 5000))]);
    this.close();
  }

  // IDENTIFY; tries times, a second each, on a link that just came up
  // (the updater does not answer until it is ready).
  async identify(timeoutMs = 3000, tries = 1) {
    for (let attempt = 1; ; attempt++) {
      try {
        this.id = decodeIdentify(await this.cmd(CMD.IDENTIFY, { timeoutMs }));
        break;
      } catch (error) {
        if (attempt >= tries || this.gone) throw error;
      }
    }
    if (this.usb) this.id.dataMax = hidCommandMax(this.link);   // the 88 is config mode's
    else if (!this.id.updater) this.frameData = Math.min(this.frameData, CONFIG_FRAME_DATA);
    return this.id;
  }

  // The pad's controls and their printed labels; null from firmware without
  // LAYOUT.
  async layout() {
    const out = [];
    for (;;) {
      const got = decodeLayout(await this.cmd(CMD.LAYOUT, { arg: out.length }));
      if (!got) return out.length ? out : null;
      out.push(...got.entries);
      if (!got.entries.length || out.length >= got.count) return out;
    }
  }

  async status() {
    return decodeStatus(await this.cmd(CMD.STATUS));
  }

  // The field table, as the firmware describes it.
  async fields() {
    if (this.id.protocol !== PROTOCOL) {
      throw new Error(`the firmware speaks protocol ${this.id.protocol}, this page ${PROTOCOL}`);
    }
    const table = [];
    for (;;) {
      const got = decodeFields(await this.cmd(CMD.FIELDS, { arg: table.length }));
      if (!got.length) break;
      table.push(...got);
    }
    return table;                // none: a device without settings (a dongle)
  }

  // {name: {value, set}} of a target, or its defaults.
  async read(table, target, defaults = false) {
    return decodeValues(await this.cmd(defaults ? CMD.DEFAULTS : CMD.READ, { arg: target }), table, target);
  }

  // Change settings: items [{target, id, value}], value empty for the
  // default.  WRITE in pieces of at most dataMax, each checked (RESULT), then
  // COMMIT; nothing applies if the pad refuses any (DROP).
  async write(items) {
    const max = this.id?.dataMax ?? CONFIG_DATA_MAX, chunks = [];
    let chunk = [];
    for (const { target, id, value } of items) {
      const item = [target, id, value.length, ...value];
      if (item.length > max) throw new Error(`a setting of ${value.length} bytes does not fit a command`);
      if (chunk.length + item.length > max) chunks.push(chunk), chunk = [];
      chunk.push(...item);
    }
    if (chunk.length) chunks.push(chunk);
    let done = 0;
    for (const c of chunks) {
      await this.cmd(CMD.WRITE, { data: Uint8Array.from(c) });
      const r = await this.cmd(CMD.RESULT);
      const codes = Array.from(r.subarray(1, 1 + r[0]));
      const bad = codes.map((code, i) => ({ code, item: items[done + i] })).filter((x) => x.code);
      if (!codes.length || bad.length) {
        await this.cmd(CMD.DROP);
        const error = new Error("the pad refused the change, nothing changed: " +
          (bad.map((x) => RESULTS[x.code] ?? `code ${x.code}`).filter((x, i, a) => a.indexOf(x) === i).join(", ") || "nothing accepted"));
        error.refused = bad.map((x) => ({ ...x.item, why: RESULTS[x.code] ?? `code ${x.code}` }));
        throw error;
      }
      done += codes.length;
    }
    await this.cmd(CMD.COMMIT);
  }

  async updateInfo(timeoutMs = 3000) {
    return decodeUpdateInfo(await this.cmd(CMD.UPDATE_INFO, { timeoutMs }));
  }

  close() {
    this.link.close();
  }
}
