// The tool commands over a link (link-web.js): framing, then the commands
// and their answers.  The wire is src/lib/proto_jollymaam_wire.h and
// src/profiles/profile_config.c; padtool (tools/padtool) speaks the same.

export const PROTOCOL = 2;
export const MORE = 0x80;
const FRAME_HEAD = 5;            // u8 sub-command, u32 argument
const CONFIG_FRAME_DATA = 96 - FRAME_HEAD;   // BT_WRITE_MAX less the head
const CONFIG_DATA_MAX = 488;     // IDENTIFY's u16 at 88 from firmware that has it

export const CMD = {
  IDENTIFY: 0x00, STATUS: 0x01, READ: 0x21, DEFAULTS: 0x27, FIELDS: 0x28,
};

export const STATES = ["boot", "idle", "connecting", "pairing", "connected", "stopping", "sleeping", "rebooting"];
export const SLOT_NAMES = ["A", "B", "X", "Y", "up", "down", "left", "right"];
export const TARGETS = [
  ...SLOT_NAMES.map((n) => "slot-" + n.toLowerCase()), "palette", "calibration",
  ...SLOT_NAMES.map((n) => "remap-" + n.toLowerCase()), "remap-base",
];
export const REMAP_TARGET = 10, BASE_TARGET = 18;
export const KINDS = ["slot", "palette", "calibration", "remap"];
export const T = { ENUM: 0, NUMBER: 1, RGB: 2, BYTES: 3, TEXT: 4, ENTRY: 5, LIST: 6 };

export const kindOf = (t) => (t < 8 ? 0 : t >= REMAP_TARGET ? 3 : t - 7);

// The targets a table describes: remap slots only where it has their
// fields, the base remap before them.
export function targetsFor(table) {
  const first = [...Array(REMAP_TARGET).keys()];
  if (!table.some((f) => f.kind === 3)) return first;
  return [...first, BASE_TARGET, ...Array.from({ length: BASE_TARGET - REMAP_TARGET }, (_, i) => REMAP_TARGET + i)];
}

const u16 = (d, o) => d[o] | (d[o + 1] << 8);
const u32 = (d, o) => (d[o] | (d[o + 1] << 8) | (d[o + 2] << 16) | (d[o + 3] << 24)) >>> 0;
const text = (d) => new TextDecoder().decode(d.subarray(0, (d.indexOf(0) + 1 || d.length + 1) - 1));
const hex = (d) => Array.from(d, (b) => b.toString(16).padStart(2, "0")).join("");

// ---- answers -------------------------------------------------------------

export function decodeIdentify(d) {
  if (d.length < 56) throw new Error(`short IDENTIFY (${d.length} bytes)`);
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

export function showValue(f, raw) {
  switch (f.type) {
    case T.TEXT: return text(raw);
    case T.ENUM: return f.names[raw[0]] || String(raw[0]);
    case T.NUMBER: return String(raw.reduceRight((v, b) => v * 256 + b, 0));
    case T.ENTRY: return String(raw[0]);
    case T.RGB: return "#" + hex(raw);
    case T.LIST: return Array.from(raw).join(",");
  }
  if (f.name === "left_curve_points" || f.name === "right_curve_points") return Array.from(raw).join(",");
  if (f.name === "address") return raw.some((b) => b) ? hex(raw.slice().reverse()).match(/../g).join(":") : "derived";
  return raw.some((b) => b) ? hex(raw) : "none";
}

// ---- commands ------------------------------------------------------------

export class Pad {
  constructor(link) {
    this.link = link;
    this.frameData = link.maxWrite - FRAME_HEAD;
    this.pending = null;
    this.chain = Promise.resolve();
    link.onReply((r) => this.#reply(r));
    link.onDisconnect(() => {
      this.gone = true;
      this.pending?.reject(new Error("link lost"));
      this.onDisconnect?.();
    });
  }

  #reply(r) {
    const p = this.pending;
    if (!p || (r[0] & ~MORE) !== p.sub) return;    // the rest of an answer nobody waits for
    p.parts.push(r.subarray(1));
    if (r[0] & MORE) return;
    const answer = new Uint8Array(p.parts.reduce((n, x) => n + x.length, 0));
    p.parts.reduce((o, x) => (answer.set(x, o), o + x.length), 0);
    p.resolve(answer);
  }

  // A command: its data in frames, then its answer (every command in config
  // mode answers; the updater's answer is one piece).  One at a time.
  cmd(sub, { arg = 0, data = new Uint8Array(0), timeoutMs = 3000 } = {}) {
    const run = () => this.#run(sub, arg, data, timeoutMs);
    const result = this.chain.then(run, run);
    this.chain = result.catch(() => {});
    return result;
  }

  async #run(sub, arg, data, timeoutMs) {
    if (this.gone) throw new Error("link lost");
    const answer = new Promise((resolve, reject) => { this.pending = { sub, parts: [], resolve, reject }; });
    answer.catch(() => {});
    let timer;
    try {
      const n = Math.max(1, Math.ceil(data.length / this.frameData));
      for (let i = 0; i < n; i++) {
        const piece = data.subarray(i * this.frameData, (i + 1) * this.frameData);
        const frame = new Uint8Array(FRAME_HEAD + piece.length);
        frame[0] = sub | (i < n - 1 ? MORE : 0);
        new DataView(frame.buffer).setUint32(1, arg >>> 0, true);
        frame.set(piece, FRAME_HEAD);
        await this.link.write(frame);
      }
      return await Promise.race([answer, new Promise((_, reject) => {
        timer = setTimeout(() => reject(new Error(`no answer to command 0x${sub.toString(16)} in ${timeoutMs} ms`)), timeoutMs);
      })]);
    } finally {
      clearTimeout(timer);
      this.pending = null;
    }
  }

  async identify() {
    this.id = decodeIdentify(await this.cmd(CMD.IDENTIFY));
    if (!this.id.updater) this.frameData = Math.min(this.frameData, CONFIG_FRAME_DATA);
    return this.id;
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
    if (!table.length) throw new Error("the firmware lists no settings (FIELDS)");
    return table;
  }

  // {name: {value, set}} of a target, or its defaults.
  async read(table, target, defaults = false) {
    return decodeValues(await this.cmd(defaults ? CMD.DEFAULTS : CMD.READ, { arg: target }), table, target);
  }

  close() {
    this.link.close();
  }
}
