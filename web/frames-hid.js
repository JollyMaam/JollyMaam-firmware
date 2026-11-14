// The tool device's commands over a HID link (link-hid.js, or the Android
// app's): the framing of platform_jieli_usb.c ("the tool device") and
// platform_telink_usb.c, as tools/padtool/padtransport.py speaks it.
//
// A command is one feature report: the CDB (16 bytes: 0xf5, the
// sub-command, u32 argument), u16 length, u8 flags, u8 tag, u32 client,
// then the data to the pad.  Without data, length is the answer's size: the
// pad sends that many bytes, zero-padded.  The answer comes on input
// reports, which every program with the device open receives: u32 client,
// u8 tag, u8 state, u16 offset, then up to 56 bytes.  A program takes only
// its client's and tag's; REFUSED (another program's command runs) means
// send it again.  hidraw loses reports when no one reads, never doubles
// them: a piece missing sends the command again.
//
// The raw link: { kind: "hid", reportSize, sendFeature(bytes) -> Promise,
// onInput(fn(bytes)), onDisconnect(fn), close() }.

export const OP = 0xf5;
const HEAD = 24;                 // JM_HID_CLIENT_HEAD_SIZE: CDB, length, flags, tag, client
const IN_HEAD = 8;               // JM_HID_INPUT_HEAD_SIZE: client, tag, state, offset
const OUT = 1, INPUT = 2;        // enum jm_hid_flags
const MORE = 2, LAST = 3, REFUSED = 4;   // enum jm_hid_state
const TRIES_MAX = 200;           // refused (another program's commands) or a piece lost
const SMALL = 64, GATHER = 512;  // JM_HID_REPORT_SMALL, JM_HID_GATHER
const ANSWER_MAX = 512;          // what a command answers at most, on any report size

// The data one report carries: the report less its head.
export const dataMax = (link) => link.reportSize - HEAD;
// The data a command may carry: a small report's device (the U2CB's)
// gathers it from several reports, each with the command's head.
export const commandMax = (link) => (link.reportSize <= SMALL ? GATHER : dataMax(link));

export class HidCommands {
  constructor(link) {
    this.link = link;
    this.client = crypto.getRandomValues(new Uint32Array(1))[0];   // this page, in the answers it takes
    this.tag = crypto.getRandomValues(new Uint8Array(1))[0];
    this.pending = null;
    link.onInput((r) => this.#input(r));
  }

  #input(r) {
    const p = this.pending;
    if (!p || r.length < IN_HEAD) return;
    const v = new DataView(r.buffer, r.byteOffset, r.byteLength);
    if (v.getUint32(0, true) !== this.client || r[4] !== p.tag) return;
    const state = r[5], offset = v.getUint16(6, true);
    if (state === REFUSED) return p.done(REFUSED);
    if ((state !== MORE && state !== LAST) || offset !== p.got) return p.done(MORE);   // a piece lost
    const d = r.subarray(IN_HEAD, IN_HEAD + Math.max(0, Math.min(r.length - IN_HEAD, p.size - offset)));
    p.answer.set(d, offset);
    p.got += d.length;
    if (state === LAST) p.done(LAST);
  }

  // One command; size: the answer's (capped at the report).  wanted false:
  // sent, not waited for (a reboot: the device goes).
  async run(sub, arg, data, size, timeoutMs, wanted) {
    const max = commandMax(this.link), piece = dataMax(this.link);
    if (data.length > max) throw new Error(`command 0x${sub.toString(16)}: ${data.length} bytes, the tool device takes ${max}`);
    size = data.length ? 0 : Math.min(size, ANSWER_MAX);
    const end = Date.now() + timeoutMs;
    for (let tries = 1; ; tries++) {
      const tag = this.tag = (this.tag + 1) & 0xff;
      // One report, or for data past one report several, each with the
      // head (the length the whole's).
      const reports = [];
      for (let at = 0; at === 0 || at < data.length; at += piece) {
        const part = data.subarray(at, at + piece);
        const report = new Uint8Array(HEAD + part.length);
        const v = new DataView(report.buffer);
        report[0] = OP;
        report[1] = sub;
        v.setUint32(2, arg >>> 0, true);
        v.setUint16(16, data.length || size, true);
        report[18] = INPUT | (data.length ? OUT : 0);
        report[19] = tag;
        v.setUint32(20, this.client, true);
        report.set(part, HEAD);
        reports.push(report);
      }
      let timer, p;
      const got = new Promise((resolve) => {
        p = this.pending = { tag, size, got: 0, answer: new Uint8Array(size), done: resolve };
        timer = setTimeout(() => resolve(null), Math.max(1, end - Date.now()));
      });
      try {
        for (const report of reports) await this.link.sendFeature(report);
        if (!wanted) return new Uint8Array(0);
        const state = await got;
        if (state === LAST) return p.answer;
        if (state === null) throw new Error(`no answer to command 0x${sub.toString(16)} in ${timeoutMs} ms`);
      } finally {
        clearTimeout(timer);
        if (this.pending === p) this.pending = null;
      }
      if (tries >= TRIES_MAX || Date.now() > end) {
        throw new Error(`command 0x${sub.toString(16)}: refused or cut short ${tries} times (another program keeps the pad busy)`);
      }
      await new Promise((r) => setTimeout(r, 1 + Math.random() * 10));
    }
  }
}
