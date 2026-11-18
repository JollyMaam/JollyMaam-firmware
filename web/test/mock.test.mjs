import test from "node:test";
import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import { Pad, T, targetsFor, showValue, parseValue } from "../pad.js";
import { MockDevice, BOARDS, createMock, mockPackage } from "../mock.js";
import { openPackage, checkUpdateImage } from "../package.js";
import { intoUpdater, sendImage, finish } from "../update.js";
const schema = JSON.parse(await readFile(new URL("../mock-fields.json", import.meta.url)));

for (const board of Object.keys(BOARDS)) for (const kind of ["hid", "ble"]) {
  test(`mock ${board} ${kind}: real protocol reads all settings and atomically saves/resets`, async () => {
    const device = new MockDevice(schema, { board, speed: 0 }), pad = new Pad(device.link(kind));
    const id = await pad.identify();
    assert.equal(id.product, BOARDS[board][0]);
    const fields = await pad.fields();
    assert.equal(fields.some((f) => f.kind === 1), !board.startsWith("u2c"));
    for (const target of targetsFor(fields)) {
      const values = await pad.read(fields, target);
      const defaults = await pad.read(fields, target, true);
      for (const f of fields.filter((f) => values.has(f.name))) {
        const value = values.get(f.name).value;
        assert.equal(value.length, f.size);
        assert.equal(f.help, "");
        if ([T.NUMBER, T.ENUM, T.TEXT, T.RGB, T.ENTRY, T.LIST].includes(f.type) || f.name === "map" || f.name === "rules" || f.name === "macros" || f.name.endsWith("_curve_points"))
          assert.deepEqual(parseValue(f, showValue(f, value)), value, `${target}: ${f.name}`);
        assert.deepEqual(defaults.get(f.name).value, value);
      }
    }
    const field = fields.find((f) => f.name === "tx_power");
    await pad.write([{ target: 0, id: field.id, value: Uint8Array.of(2), f: field }]);
    assert.equal((await pad.read(fields, 0)).get(field.name).set, true);
    device.outcome = "failure";
    await assert.rejects(pad.write([{ target: 0, id: field.id, value: Uint8Array.of(3), f: field }]), /refused/);
    assert.equal((await pad.read(fields, 0)).get(field.name).value[0], 2);
    assert.equal(device.staged.size, 0);
    device.outcome = "success";
    await pad.write([{ target: 0, id: field.id, value: new Uint8Array(0) }]);
    assert.equal((await pad.read(fields, 0)).get(field.name).set, false);
    pad.close();
  });
}

test("mock packages and updater run through real readers and updater flow", async () => {
  const pkg = await openPackage(await mockPackage());
  for (const [product] of Object.values(BOARDS)) {
    const file = pkg.files.find((f) => f.name === product);
    assert.equal(checkUpdateImage(await pkg.read(file), product), null);
  }
  for (const [product] of Object.values(BOARDS)) {
    const other = await openPackage(await mockPackage(product));
    assert.ok(other.files.every((f) => f.name !== product));
  }
  const env = await createMock(new URLSearchParams("mock=normal"), schema);
  env.device.speed = 0;
  const pad = new Pad(await env.bluetooth.choose());
  await pad.identify();
  const up = await intoUpdater(pad, env.bluetooth, () => {});
  const image = await pkg.read(pkg.files.find((f) => f.name === env.device.product));
  let done = 0;
  await sendImage(up, image, () => {}, (n) => { done = n; });
  assert.equal(done, image.length);
  const fresh = await finish(up, env.bluetooth, () => {});
  assert.equal(fresh.id.updater, false);
  fresh.close();
});

test("mock failure scenarios expose real UI inputs and protocol errors", async () => {
  for (const scenario of ["unknown", "empty", "incompatible", "read_error", "updater"]) {
    const device = new MockDevice(schema, { scenario, speed: 0 }), pad = new Pad(device.link());
    await pad.identify();
    if (scenario === "incompatible") await assert.rejects(pad.fields(), /protocol 99/);
    else if (scenario === "read_error") await assert.rejects(pad.fields(), /could not be read/);
    else if (scenario === "empty") assert.deepEqual(await pad.fields(), []);
    else if (scenario === "unknown") assert.ok((await pad.fields()).some((f) => f.name === "future_mode"));
    else assert.equal(pad.id.updater, true);
    pad.close();
  }
  const env = await createMock(new URLSearchParams("mock=permission&view=app"), schema);
  assert.equal(await env.bluetooth.permission(), false);
  await assert.rejects(env.bluetooth.choose(), /permission denied/);
});

test("populated mock data is inherited and app jobs expose failure without native hooks", async () => {
  const device = new MockDevice(schema, { scenario: "populated", speed: 0 });
  const pad = new Pad(device.link());
  await pad.identify();
  const fields = await pad.fields();
  const base = await pad.read(fields, 18), slot = await pad.read(fields, 10);
  assert.equal(base.get("rules").set, true);
  assert.equal(slot.get("rules").set, false);
  assert.deepEqual(slot.get("rules").value, base.get("rules").value);
  assert.equal(await pad.hasQuickMacro(), true);
  assert.ok((await pad.quickMacro()).steps.length > 0);
  const macros = await pad.read(fields, 27);
  assert.ok(macros.get("macros").value.some(Boolean));
  pad.close();
  const env = await createMock(new URLSearchParams("mock=stock&view=app&outcome=failure"), schema);
  assert.equal((await env.android.flasher.probe()).kind, 3);
  const progress = [];
  await assert.rejects(env.android.flasher.session("mock-usb", { backup: true },
    (_what, n) => progress.push(n), () => {}), /transfer failed/);
  assert.ok(progress.includes(50));
  assert.equal(env.device.saved.size, 0);
});

test("guided checkpoints hold writes without losing the connection", async () => {
  const device = new MockDevice(schema, { speed: 0 });
  const pad = new Pad(device.link()); await pad.identify();
  assert.equal(await pad.hasQuickMacro(), false);
  device.guided = true; device.hold = "write";
  const pending = pad.write([{ target: 0, id: 21, value: Uint8Array.of(2) }]);
  while (!device.waiting) await new Promise((r) => setTimeout(r, 1));
  assert.equal(device.saved.size, 0);
  assert.equal(pad.link.commandTimeoutMs, 0x7fffffff);
  device.release(); await pending;
  assert.equal(device.saved.get("0:21")[0], 2);
  assert.equal(pad.link.commandTimeoutMs, undefined);
  pad.close();
});

test("unavailable APIs override a stale app view and prevent mock connections", async () => {
  const env = await createMock(new URLSearchParams("mock=unavailable&view=app&step=5"), schema);
  assert.equal(env.android, null);
  for (const provider of [env.usb, env.bluetooth]) {
    assert.equal(provider.available(), false);
    await assert.rejects(provider.choose(), /no USB or Bluetooth API/);
  }
});
