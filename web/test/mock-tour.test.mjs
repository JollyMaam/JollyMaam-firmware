import test from "node:test";
import assert from "node:assert/strict";
import { tourSteps } from "../mock-tour.js";

test("tours walk through editing and saving in order and terminate at failure or busy states", () => {
  const normal = tourSteps({ scenario: "normal", app: false, outcome: "success" });
  assert.deepEqual(normal.map((s) => s.action), ["none", "loading", "ready", "help", "invalid", "edit", "saveStart", "saveFinish", "disconnect"]);
  const failure = tourSteps({ scenario: "normal", app: false, outcome: "failure" });
  assert.equal(failure[7].name, "Save refused");
  const busy = tourSteps({ scenario: "normal", app: true, outcome: "busy" });
  assert.equal(busy.at(-1).action, "jobBusy");
  const manual = tourSteps({ scenario: "manual", app: true, outcome: "success" });
  assert.equal(manual.at(-1).action, "manage");
  const missing = tourSteps({ scenario: "normal", app: true, outcome: "success", packageState: "missing" });
  assert.ok(!missing.some((s) => s.action === "jobStart"));
});

test("unavailable browser APIs never select an app firmware tour", () => {
  for (const app of [true, false]) {
    const steps = tourSteps({ scenario: "unavailable", app, outcome: "success" });
    assert.deepEqual(steps.map((s) => s.name), ["Home", "Unavailable APIs"]);
    assert.ok(steps.every((s) => s.action === "none"));
  }
});
