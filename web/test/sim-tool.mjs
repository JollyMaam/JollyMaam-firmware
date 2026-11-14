// A simulated tool device for the tests: platform_jieli_usb.c's feature
// report in, its 64-byte input reports out (client, tag, state, offset, 56
// bytes), the answer as long as asked for.  report 64: the U2CB's
// (usb_yc3121.c), which gathers a command's data from several reports.
import assert from "node:assert/strict";
import { CMD } from "../pad.js";

const REPORT = 512, IN_DATA = 56;
const MORE = 2, LAST = 3, REFUSED = 4;

export function simulatedTool({ report: REPORT = 512 } = {}) {
  let input = () => {}, gathering = null;
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
      let data = report.subarray(24);
      if (gathering && gathering.tag === tag && gathering.client === client) {   // the next piece
        gathering.data = Uint8Array.of(...gathering.data, ...data);
        if (gathering.data.length < len) return;
        data = gathering.data;
        gathering = null;
      } else if ((flags & 1) && len > data.length) {
        assert.equal(REPORT, 64, "only a small report's device gathers");
        gathering = { tag, client, data: data.slice() };
        return;
      }
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
