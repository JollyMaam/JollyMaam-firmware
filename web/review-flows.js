// Review states are recipes for reaching the production UI, not copies of it.
import { UPDATE_ERRORS, RESULTS } from "./pad.js";
import { tourSteps } from "./mock-tour.js";
const node = (id, title, goal, options = {}, kind = "state") => ({ id, title, goal, options, kind, edges: [] });
const flow = (id, title, nodes, edges) => {
  for (const [from, label, to] of edges) nodes.find((n) => n.id === from).edges.push({ label, to });
  return { id, title, nodes };
};
export const WORKFLOWS = [
  flow("connection", "Connect a controller", [
    node("home", "Connection choices", "Home"), node("loading", "Loading settings", "loading"),
    node("ready", "Connected", "ready", {}, "success"),
    node("unsupported", "USB/Bluetooth unavailable", "Unavailable APIs", { mock: "unavailable" }, "error"),
    node("protocol", "Unsupported protocol", "connect", { mock: "incompatible" }, "error"),
    node("read-error", "Settings read failed", "connect", { mock: "read_error" }, "error"),
    node("permission", "Bluetooth denied", "manage", { mock: "permission", view: "app" }, "error"),
    node("empty", "No settings / dongle", "connect", { mock: "empty" }),
    node("no-devices", "No devices found", "manage", { mock: "no_devices", view: "app" }),
    node("probe", "USB identification failed", "manage", { mock: "probe_error", view: "app" }, "error"),
    node("unknown", "Unknown setting fallback", "future", { mock: "unknown" }),
  ], [["home","Connect","loading"],["loading","Loaded","ready"],["home","APIs missing","unsupported"],
    ["loading","Wrong protocol","protocol"],["loading","Read failed","read-error"],["home","Access denied","permission"],
    ["loading","Dongle","empty"],["home","None found","no-devices"],["home","Probe failed","probe"],["ready","Newer firmware","unknown"]]),
  flow("settings", "Edit and save settings", [
    node("ready", "Settings", "ready"), node("dirty", "Unsaved changes", "edit"),
    node("invalid", "Invalid value", "invalid", {}, "error"), node("saving", "Saving", "saveStart"),
    node("saved", "Saved", "saveFinish", {}, "success"), node("refused", "Change refused", "saveFinish", { outcome: "failure" }, "error"),
    node("temporary", "Saved temporarily", "saveFinish", { mock: "ephemeral" }, "success"),
    node("pairing", "Pairing will be cleared", "savePrompt", { workflow: "pairing", answer: "cancel" }, "confirmation"),
    node("discard", "Discard unsaved changes?", "discardPrompt", { workflow: "discard", answer: "cancel" }, "confirmation"),
    node("disconnected", "Device disconnected", "disconnect"),
  ], [["ready","Edit","dirty"],["dirty","Invalid input","invalid"],["invalid","Correct value","dirty"],
    ["dirty","Save","saving"],["saving","Accepted","saved"],["saving","Refused","refused"],
    ["saving","Ephemeral mode","temporary"],["dirty","Change wireless type","pairing"],["dirty","Leave page","discard"],
    ["saved","Device disappears","disconnected"],["refused","Fix and retry","dirty"],["discard","Keep editing","dirty"]]),
];
const OPERATIONS = ["install", "backup", "upgrade", "restore"];
const androidNode = (id, title, goal, extra = {}, kind = "state", operations = OPERATIONS) =>
  ({ ...node(id, title, goal, { workflow: "install", view: "app", ...extra }, kind), sharedOperation: true, operations });
WORKFLOWS.push(flow("firmware", "Firmware actions", [
  androidNode("device", "Device and action", "manage"), androidNode("options", "Action options", "jobOpen"),
  androidNode("confirm", "Confirm writing", "jobPrompt", { answer: "cancel" }, "confirmation", ["install", "restore"]),
  androidNode("progress", "In progress", "jobStart"), androidNode("success", "Completed", "jobFinish", {}, "success"),
  androidNode("failure", "Operation failed", "jobFinish", { outcome: "failure" }, "error"),
  androidNode("busy", "Still busy", "jobBusy", { outcome: "busy" }),
  androidNode("missing", "No firmware package", "manage", { package: "missing" }, "error"),
  androidNode("mismatch", "No matching image", "manage", { package: "mismatch" }, "error", ["install", "upgrade"]),
  androidNode("manual", "Manual boot loader required", "manage", { mock: "manual" }, "error", ["install", "backup", "restore"]),
], [["device","Open action","options"],["options","Start","progress"],["options","Request write","confirm"],
  ["confirm","Confirm","progress"],["confirm","Cancel","options"],["progress","Completed","success"],
  ["progress","Failure (including updater errors)","failure"],["progress","Still running","busy"],
  ["device","No package","missing"],["device","No matching image","mismatch"],["device","Cannot enter loader","manual"],
  ["failure","Retry","options"]]));
for (const edge of WORKFLOWS.find((f) => f.id === "firmware").nodes.find((n) => n.id === "options").edges)
  edge.operations = edge.to === "confirm" ? ["install", "restore"] : ["backup", "upgrade"];
WORKFLOWS.push(flow("update", "Browser firmware update", [
  node("available", "Update available", "firmware", { workflow: "update" }),
  node("confirm", "Confirm firmware write", "updatePrompt", { workflow: "update", answer: "cancel" }, "confirmation"),
  node("writing", "Writing firmware", "updateStart", { workflow: "update" }),
  node("success", "New firmware started", "updateFinish", { workflow: "update" }, "success"),
  node("failure", "Update failed", "updateFinish", { workflow: "update", outcome: "failure" }, "error"),
  node("missing", "No package", "firmware", { workflow: "update", package: "missing" }, "error"),
  node("mismatch", "No update for this pad", "firmware", { workflow: "update", package: "mismatch" }, "error"),
  node("no-app", "Updater / recovery", "firmware", { workflow: "update", mock: "no_app" }),
], [["available","Update","confirm"],["confirm","Confirm","writing"],["confirm","Cancel","available"],
  ["writing","Restarted","success"],["writing","Write error","failure"],["available","Missing package","missing"],
  ["available","Wrong package","mismatch"],["failure","Reconnect updater","no-app"]]));
WORKFLOWS.push(flow("package", "Load a firmware package", [
  node("missing", "No package loaded", "firmware", { workflow: "package", package: "missing" }),
  node("loaded", "Package loaded", "packageLoad", { workflow: "package", package: "missing" }, "success"),
  node("invalid", "Invalid package file", "packageLoad", { workflow: "package", fixture: "invalid" }, "error"),
  node("unload", "Package unloaded", "packageUnload", { workflow: "package" }, "success"),
  node("corrupt", "Damaged bundled package", "firmware", { workflow: "package", package: "corrupt" }, "error"),
], [["missing","Choose valid ZIP","loaded"],["missing","Choose invalid file","invalid"],["invalid","Try valid package","loaded"],["loaded","Unload","unload"],["unload","Load again","loaded"],["missing","Damaged bundle","corrupt"]]));
WORKFLOWS.push(flow("remap", "Remap presets and macros", [
  node("ready", "Saved rules and macros", "macros", { mock: "populated" }),
  node("preset", "Preset imported", "presetLoad", { workflow: "remap" }, "success"),
  node("invalid-preset", "Invalid preset file", "presetLoad", { workflow: "remap", fixture: "invalid" }, "error"),
  node("invalid-macro", "Invalid macro text", "macroError", { workflow: "macros" }, "error"),
  node("quick-empty", "Quick Settings import unavailable", "quickMacroError", { workflow: "quickmacro" }),
], [["ready","Import preset","preset"],["ready","Invalid preset","invalid-preset"],["ready","Edit macro text","invalid-macro"],
  ["ready","Import missing quick macro","quick-empty"],["invalid-preset","Retry import","preset"]]));

// Error codes are message variants of one state, rather than more graph nodes.
WORKFLOWS.find((f) => f.id === "update").nodes.find((n) => n.id === "failure").variants = UPDATE_ERRORS.slice(1)
  .map((label, i) => ({ id: String(i + 1), label, options: { error: String(i + 1) } }));
WORKFLOWS.find((f) => f.id === "settings").nodes.find((n) => n.id === "refused").variants = RESULTS.slice(1)
  .map((label, i) => ({ id: String(i + 1), label, options: { refusal: String(i + 1) } }));
const connection = WORKFLOWS.find((f) => f.id === "connection");
const discovery = connection.nodes.filter((n) => n.options.view === "app");
connection.nodes = connection.nodes.filter((n) => !discovery.includes(n));
for (const n of connection.nodes) n.edges = n.edges.filter((e) => connection.nodes.some((target) => target.id === e.to));
WORKFLOWS.splice(1, 0, flow("discovery", "Device discovery", [node("home", "Devices", "Home", { view: "app" }), ...discovery],
  discovery.map((n) => ["home", n.title, n.id])));
for (const workflow of WORKFLOWS) {
  workflow.platform = ["discovery", "firmware"].includes(workflow.id) ? "android" : workflow.id === "update" ? "browser" : "shared";
  for (const n of workflow.nodes) n.platform = workflow.id === "connection" && n.id === "unsupported" ? "browser" : workflow.platform;
}
WORKFLOWS.find((f) => f.id === "settings").nodes.find((n) => n.id === "refused").defaultVariant = "3";
WORKFLOWS.find((f) => f.id === "update").nodes.find((n) => n.id === "failure").defaultVariant = "4";
const nativeFailure = WORKFLOWS.find((f) => f.id === "firmware").nodes.find((n) => n.id === "failure");
nativeFailure.description = "Android operation failure. All JM_UPDATE_ERR_* codes use the same failure handling; the log carries diagnostic details. Install, backup, upgrade and restore share this presentation, with the action name changing.";
for (const n of WORKFLOWS.find((f) => f.id === "package").nodes) if (n.goal === "firmware") n.appGoal = "manage";

export function applicable(node, { platform = "browser", operation = "install" } = {}) {
  return (node.platform === "shared" || node.platform === platform) && (!node.operations || node.operations.includes(operation));
}
export function recipe(node, context = {}) {
  const platform = node.platform === "shared" ? context.platform || "browser" : node.platform;
  const options = { mock: "normal", outcome: "success", package: "present", ...node.options, view: platform === "android" ? "app" : "browser" };
  if (node.sharedOperation) {
    const operation = context.operation || "install";
    if (!node.operations.includes(operation)) throw new Error(`${node.id} does not apply to ${operation}`);
    options.workflow = operation;
  }
  if (node.platform === "shared" && platform === "android" && !options.workflow) options.workflow = "settings";
  const variant = node.variants?.find((v) => v.id === context.variant) || node.variants?.find((v) => v.id === node.defaultVariant) || node.variants?.[0];
  if (variant) Object.assign(options, variant.options);
  const goal = platform === "android" ? node.appGoal || node.goal : node.goal;
  const steps = tourSteps({ scenario: options.mock, app: options.view === "app", outcome: options.outcome,
    packageState: options.package, workflow: options.workflow, answer: options.answer });
  const step = steps.findIndex((s) => s.action === goal || s.name === goal);
  if (step < 0) throw new Error(`Unreachable review state: ${node.id} (${goal})`);
  return { ...options, step: String(step), review: "1" };
}
export function previewURL(node, base, context = {}) {
  const url = new URL("index.html", base);
  for (const [key, value] of Object.entries({ ...recipe(node, context), board: context.board, lang: context.lang })) if (value) url.searchParams.set(key, value);
  return url;
}
export function legacyState(id = "") {
  const [family, state] = id.split(".");
  if (OPERATIONS.includes(family)) return { id: `firmware.${state}`, operation: family };
  if (family === "diagnostics") return state?.startsWith("updater-")
    ? { id: "update.failure", variant: state.slice(8) } : { id: "settings.refused", variant: state?.slice(8) };
  if (family === "connection" && ["permission", "probe", "no-devices"].includes(state)) return { id: `discovery.${state}` };
  return { id };
}
