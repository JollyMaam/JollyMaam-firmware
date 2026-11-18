import test from "node:test";
import assert from "node:assert/strict";
import { ask, tell, useDialogs } from "../dialogs.js";

test("production uses native dialogs and review mode captures them without showing OS dialogs", () => {
  const savedConfirm = globalThis.confirm, savedAlert = globalThis.alert, calls = [];
  globalThis.confirm = (message) => { calls.push(["native-confirm", message]); return true; };
  globalThis.alert = (message) => calls.push(["native-alert", message]);
  try {
    assert.equal(ask("Write?"), true); tell("Saved");
    useDialogs((kind, message) => { calls.push([kind, message]); return false; });
    assert.equal(ask("Write?"), false); tell("Failed");
    assert.deepEqual(calls, [["native-confirm", "Write?"], ["native-alert", "Saved"], ["confirm", "Write?"], ["alert", "Failed"]]);
  } finally { useDialogs(null); globalThis.confirm = savedConfirm; globalThis.alert = savedAlert; }
});
