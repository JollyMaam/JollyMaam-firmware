// remap.js against padremap.py and padcurves.py: the vectors the Python
// writes (test/generate_vectors.py), then the
// edge cases padremap's own tests check.
import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import * as remap from "../remap.js";
import { placePoints } from "../editors.js";

const vectors = JSON.parse(readFileSync(new URL("./python-vectors.json", import.meta.url)));
const hex = (d) => Buffer.from(d).toString("hex");
const bytes = (h) => Uint8Array.from(Buffer.from(h, "hex"));

test("maps, rules and curves as the Python makes them", () => {
  for (const v of vectors.maps) {
    assert.equal(hex(remap.parseMap(v.input)), v.hex, v.input);
    assert.equal(remap.showMap(bytes(v.hex)), v.text, v.input);
  }
  for (const v of vectors.rules) {
    assert.equal(hex(remap.parseRules(v.input)), v.hex, v.input);
    assert.equal(remap.showRules(bytes(v.hex)), v.text, v.input);
  }
  for (const v of vectors.curves) assert.equal(hex(remap.curvePreset(v.name)), v.hex, v.name);
  assert.deepEqual(Array.from(remap.curvePreset("linear")), [16, 32, 48, 64, 80, 96, 112, 128, 143, 159, 175, 191, 207, 223, 239]);
});

test("map: keys as they were, presets on top", () => {
  const raw = remap.parseMap("west=north north=west south=east east=south capture=none l4=south r4=r4");
  assert.equal(remap.showMap(raw), "south=east east=south capture=none west=north north=west l4=south");
  const base = remap.applyPreset(remap.decodeMap(remap.parseMap("capture=none l4=south south=up")), "swap-faces");
  assert.equal(remap.showMap(remap.encodeMap(base)), "south=east east=south capture=none west=north north=west l4=south");
  for (const bad of ["west", "q=south", "south=q"]) assert.throws(() => remap.parseMap(bad), remap.RemapError);
});

test("rules: what the firmware refuses is refused here", () => {
  const text = "r4 held hold l1+r1; south held turbo south@max; l4 pressed toggle_turbo south@15; " +
    "r4 tapped:300 tap south; r4 held_for:300 hold l1; capture held nothing; " +
    "r4 held turbo_modifier south+east@10; l4 released toggle_turbo_modifier west@max";
  assert.equal(remap.showRules(remap.parseRules(text)), text);
  assert.equal(remap.showRules(remap.parseRules("select held hold start")), "minus held hold plus");
  for (const bad of ["south held tap east", "south pressed hold east", "south tapped:0 tap east", "south held turbo east@31",
    "south sometimes hold east", "south held hold q", "south held", "south tapped:3000 tap east", "south tapped:x tap east"]) {
    assert.throws(() => remap.parseRules(bad), remap.RemapError, bad);
  }
  assert.throws(() => remap.parseRules(Array(17).fill("south held hold east").join("; ")), /at most 16/);
  assert.throws(() => remap.parseRules("south held hold east; east held hold south", 1), /at most 1/);
});

test("preset files round-trip", () => {
  const data = remap.exportPreset("Kart", remap.parseMap("south=east east=south"), remap.parseRules("r4 held turbo south@max"));
  assert.deepEqual(data, { format: 1, name: "Kart", map: "south=east east=south", rules: "r4 held turbo south@max" });
  const back = remap.importPreset(data);
  assert.equal(back.name, "Kart");
  assert.equal(remap.showRules(back.rules), "r4 held turbo south@max");
  for (const bad of [[], { format: 2 }, { format: 1, rules: "south held tap east" }]) assert.throws(() => remap.importPreset(bad), remap.RemapError);
});

test("curves: presets, custom points, order", () => {
  assert.equal(remap.identifyCurve(remap.curvePreset("s-curve")), "s-curve");
  assert.equal(remap.identifyCurve(Uint8Array.from({ length: 15 }, (_, i) => i)), "custom");
  assert.deepEqual(Array.from(remap.parseCurve("concave")), Array.from(remap.curvePreset("concave")));
  assert.throws(() => remap.parseCurve("1,2,3"), /15 nondecreasing/);
  assert.throws(() => remap.parseCurve(Array.from({ length: 15 }, (_, i) => 15 - i).join(",")), /nondecreasing/);
  assert.throws(() => remap.parseCurve(Array(15).fill(256).join(",")), /0-255/);
});

test("curve points move anywhere; the others make way", () => {
  const linear = remap.curvePreset("linear");
  const up = placePoints(linear, new Map([[2, 250]]), 2);
  assert.deepEqual(Array.from(up), [16, 32, 250, 250, 250, 250, 250, 250, 250, 250, 250, 250, 250, 250, 250]);
  const down = placePoints(linear, new Map([[12, 0]]), 12);
  assert.deepEqual(Array.from(down.subarray(0, 14)), [...Array(13).fill(0), 223]);
  const sweep = placePoints(linear, new Map([[0, 100], [1, 90], [2, 80]]), 2);
  assert.deepEqual(Array.from(sweep.subarray(0, 4)), [80, 80, 80, 80]);
  assert.deepEqual(Array.from(placePoints(linear, new Map([[5, 999]]), 5)).slice(5), Array(10).fill(255));
});
