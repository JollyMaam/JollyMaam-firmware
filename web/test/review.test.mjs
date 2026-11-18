import test from "node:test";
import assert from "node:assert/strict";
import { WORKFLOWS, recipe, previewURL, applicable, legacyState } from "../review-flows.js";
import { tourSteps } from "../mock-tour.js";

const operations = ["install", "backup", "upgrade", "restore"];
test("every applicable platform, action and error variant reaches a valid checkpoint", () => {
  const ids = new Set();
  for (const flow of WORKFLOWS) for (const node of flow.nodes) {
    const id = `${flow.id}.${node.id}`; assert.ok(!ids.has(id), id); ids.add(id);
    for (const edge of node.edges) assert.ok(flow.nodes.some((n) => n.id === edge.to), `${id} → ${edge.to}`);
    for (const platform of ["browser", "android"]) for (const operation of operations) {
      const context = { platform, operation, board: "u2cw" };
      if (!applicable(node, context)) continue;
      for (const variant of node.variants || [{ id: "" }]) {
        const r = recipe(node, { ...context, variant: variant.id });
        const steps = tourSteps({ scenario: r.mock, app: r.view === "app", outcome: r.outcome, packageState: r.package,
          workflow: r.workflow, answer: r.answer });
        const goal = platform === "android" ? node.appGoal || node.goal : node.goal;
        assert.ok(steps[Number(r.step)]?.action === goal || steps[Number(r.step)]?.name === goal, `${id} ${platform} ${operation}`);
        const url = previewURL(node, "https://example.test/web/review.html", context);
        assert.equal(url.pathname, "/web/index.html"); assert.equal(url.searchParams.get("board"), "u2cw");
        assert.equal(url.searchParams.get("review"), "1");
        assert.equal(url.searchParams.get("view"), platform === "android" ? "app" : "browser");
      }
    }
  }
  assert.ok(ids.size < 55, `collapsed state space: ${ids.size}`);
});

test("shared Android firmware graph excludes impossible upgrade loader states and duplicated diagnostics", () => {
  const firmware = WORKFLOWS.find((f) => f.id === "firmware");
  assert.equal(WORKFLOWS.filter((f) => ["install", "backup", "upgrade", "restore"].includes(f.id)).length, 0);
  const manual = firmware.nodes.find((n) => n.id === "manual");
  assert.equal(applicable(manual, { platform: "android", operation: "upgrade" }), false);
  assert.throws(() => recipe(manual, { operation: "upgrade" }), /does not apply/);
  const confirmation = firmware.nodes.find((n) => n.id === "confirm");
  assert.equal(applicable(confirmation, { platform: "android", operation: "backup" }), false);
  assert.equal(applicable(confirmation, { platform: "android", operation: "restore" }), true);
  assert.equal(applicable(firmware.nodes[0], { platform: "browser" }), false);
  assert.equal(WORKFLOWS.some((f) => f.id === "diagnostics"), false);
  assert.equal(WORKFLOWS.find((f) => f.id === "update").nodes.find((n) => n.id === "failure").variants.length, 10);
  assert.deepEqual(legacyState("upgrade.manual"), { id: "firmware.manual", operation: "upgrade" });
  assert.deepEqual(legacyState("diagnostics.updater-4"), { id: "update.failure", variant: "4" });
});
