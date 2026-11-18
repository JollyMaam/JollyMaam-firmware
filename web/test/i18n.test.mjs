import test from "node:test";
import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import { text, fieldText, useCatalog, loadCatalog, language } from "../i18n.js";

const read = async (name) => JSON.parse(await readFile(new URL(`../strings/${name}.json`, import.meta.url)));

test("catalog labels do not change enum wire values; unknown fields retain firmware text", async () => {
  const en = await read("en"), mapping = await read("settings");
  useCatalog(en, { "settings.slot.radio.options.bluetooth": "BT translated" }, mapping);
  const field = { kind: 0, id: 24, help: "", names: ["off", "bluetooth", "dongle"] };
  assert.equal(fieldText(field, "options.bluetooth", "bluetooth"), "BT translated");
  assert.equal(fieldText(field, "help", field.help), en["settings.slot.radio.help"]);
  assert.deepEqual(field.names, ["off", "bluetooth", "dongle"]);
  assert.equal(fieldText(field, "label", "radio"), en["settings.slot.radio.label"]);
  assert.equal(fieldText({ kind: 0, id: 254 }, "label", "future_setting"), "future_setting");
  assert.equal(fieldText(field, "options.future", "future"), "future");
  assert.equal(fieldText({ kind: 3, id: 3, size: 40 }, "help", ""),
    en["settings.remap.rules.help"].replace("{count}", "4"));
});

test("regional language falls back to base, missing entries to English, and missing locale to English", async () => {
  const seen = [];
  const fetcher = async (url) => {
    const name = url.pathname.split("/").at(-1).replace(/\.json$/, "");
    seen.push(name);
    if (name === "de") return { ok: true, json: async () => ({ "ui.page.save": "Speichern" }) };
    if (!["en", "settings"].includes(name)) return { ok: false };
    return { ok: true, json: () => read(name) };
  };
  await loadCatalog("de-DE", fetcher);
  assert.equal(language, "de");
  assert.equal(text("ui.page.save"), "Speichern");
  assert.equal(text("ui.page.discard"), "Discard");
  assert.ok(seen.includes("de-de"));
  await loadCatalog("../../invalid", fetcher);
  assert.equal(language, "en");
  assert.equal(text("ui.page.save"), "Save");
  assert.equal(text("ui.editors.invalid_rules", { error: "Broken" }), 'Broken: use "All to default" to start again');
});
