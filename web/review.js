import { WORKFLOWS, previewURL, applicable, legacyState } from "./review-flows.js";
const $ = (id) => document.getElementById(id);
const source = await (await fetch(new URL("./strings/en.json", import.meta.url))).json();
const entries = WORKFLOWS.flatMap((flow) => flow.nodes.map((node) => ({ flow, node, id: `${flow.id}.${node.id}` })));
const inspected = new Map(), notes = new Map();
let current, activeFrame = 0, scanning = false, scanGeneration = 0, observer = null, refresh = null;
const initial = new URL(location.href);
$("board").value = initial.searchParams.get("board") || "c2";
const legacy = legacyState(initial.searchParams.get("state") || "");
const requested = entries.find((e) => e.id === legacy.id);
$("platform").value = requested?.node.platform !== "shared" && requested?.node.platform ? requested.node.platform : initial.searchParams.get("platform") || "browser";
$("operation").value = initial.searchParams.get("operation") || legacy.operation || "install";
if (!["browser", "android"].includes($("platform").value)) $("platform").value = "browser";
if (!["install", "backup", "upgrade", "restore"].includes($("operation").value)) $("operation").value = "install";
let selectedVariant = initial.searchParams.get("variant") || legacy.variant || "";
const context = () => ({ platform: $("platform").value, operation: $("operation").value, variant: selectedVariant, board: $("board").value, lang: initial.searchParams.get("lang") || "en" });
const available = () => entries.filter((e) => applicable(e.node, context()));
const nodesFor = (flow) => flow.nodes.filter((node) => applicable(node, context()));
const edgesFor = (node, flow) => node.edges.filter((edge) => (!edge.operations || edge.operations.includes(context().operation)) && nodesFor(flow).some((target) => target.id === edge.to));
const el = (tag, text, className) => { const item = document.createElement(tag); if (text !== undefined) item.textContent = text; if (className) item.className = className; return item; };
const visible = (item) => {
  if (!item || item.closest("[hidden]") || !item.getClientRects().length) return false;
  for (let parent = item.parentElement; parent; parent = parent.parentElement) {
    if (parent.tagName === "DETAILS" && !parent.open && !parent.querySelector(":scope > summary")?.contains(item)) return false;
  }
  return true;
};

function messages(frame, node) {
  const doc = frame.contentDocument, result = [];
  const locations = {
    "#review-dialog p": "web/dialogs.js → native confirmation/alert",
    "#mock-notice": "web/app.js → problem",
    "#savecount": "web/app.js → validation / pending changes",
    "#toast": "web/app.js → confirmation toast",
    "#actionresult": "web/flash.js → operation result",
    "#padstep": "web/flash.js → operation progress",
    ".device-package-status": "web/flash.js → device card availability",
    ".ble-permission-help p": "web/flash.js → Bluetooth permission help",
    "#settings-error": "web/app.js → settings could not load",
    "#blenote": "web/flash.js → Bluetooth access",
    "#fwinfo": "web/app.js → firmware information",
    "#loadpkginfo": "web/app.js → Android loaded package",
    "#pkginfo": "web/app.js → package information",
    "#connect:disabled span": "web/app.js → Bluetooth unavailable",
    "#connectusb:disabled span": "web/app.js → USB unavailable",
    ".macroedit .error": "web/editors.js → macro error",
  };
  for (const [selector, location] of Object.entries(locations)) for (const item of doc.querySelectorAll(selector)) {
    const text = item.textContent.trim();
    if (visible(item) && text) result.push({ text, source: location, keys: Object.keys(source).filter((key) => source[key].trim() === text), kind: selector.includes("dialog") ? "confirmation" : node.kind });
  }
  for (const item of doc.querySelectorAll("input:invalid,textarea:invalid")) if (visible(item) && item.validationMessage)
    result.push({ text: item.validationMessage, source: "web/editors.js / native input validation", keys: [], kind: "error" });
  const log = doc.getElementById("fwlog");
  if (visible(log) && log.textContent.trim()) result.push({ text: log.textContent.trim().split("\n").at(-1),
    source: "web/update.js → last updater diagnostic", keys: [], kind: node.kind });
  return result.filter((item, i) => result.findIndex((other) => other.text === item.text) === i);
}

async function load(frame, entry, tokenValid = () => true) {
  const url = previewURL(entry.node, location.href, { ...context(), variant: entry.id === current?.id ? selectedVariant : "" });
  frame.src = url.href;
  const deadline = Date.now() + 20000;
  while (Date.now() < deadline && tokenValid()) {
    await new Promise((resolve) => setTimeout(resolve, 60));
    try {
      const doc = frame.contentDocument;
      if (!doc || frame.contentWindow.location.href !== url.href) continue;
      const tour = doc.getElementById("mock-tour");
      const failure = doc.getElementById("mock-tour-description")?.textContent;
      if (failure?.startsWith("Tour stopped:")) throw new Error(failure);
      if (tour?.dataset.ready === "true" && tour.dataset.step === url.searchParams.get("step")) return messages(frame, entry.node);
    } catch (error) { if (error.message.startsWith("Tour stopped:")) throw error; }
  }
  if (tokenValid()) throw new Error("Preview did not reach its checkpoint.");
  return null;
}

function drawMessages() {
  $("messages").replaceChildren();
  const record = inspected.get(current.id);
  if (!record) return $("messages").append(el("p", "Replaying the workflow to this state…", "hint"));
  if (record.error) return $("messages").append(el("p", record.error, "message error"));
  if (!record.messages.length) $("messages").append(el("p", "This state has no message on the tracked message surfaces. Inspect the live UI for labels and instructions.", "hint"));
  for (const item of record.messages) {
    const block = el("article", undefined, `message ${item.kind}`);
    block.append(el("div", item.text), el("code", item.keys.length ? item.keys.join(" · ") : item.source));
    $("messages").append(block);
  }
}
function coverage() {
  const failures = [...inspected.values()].filter((record) => record.error).length;
  $("coverage").textContent = `${inspected.size} / ${available().length} state shapes inspected${failures ? ` · ${failures} preview failures` : ""}. ${scanning ? "Collecting messages in a separate hidden mock preview…" : "Collection runs locally and uses no hardware."}`;
}
function drawIndex() {
  const query = $("search").value.toLowerCase(), filter = $("filter").value;
  $("index").replaceChildren();
  for (const entry of available()) {
    const record = inspected.get(entry.id);
    if (filter !== "all" && entry.node.kind !== filter) continue;
    if (!`${entry.id} ${entry.node.title} ${(record?.messages || []).map((m) => m.text).join(" ")}`.toLowerCase().includes(query)) continue;
    const button = el("button", undefined, `indexed ${entry.node.kind}`);
    button.append(el("span", entry.node.title), el("small", entry.id + (record?.error ? " · preview failed" : record ? " · inspected" : "")));
    button.setAttribute("aria-current", String(current?.id === entry.id)); button.onclick = () => select(entry);
    $("index").append(button);
  }
}
function drawMap() {
  const flow = current.flow, nodes = nodesFor(flow), ns = "http://www.w3.org/2000/svg";
  const svg = document.createElementNS(ns, "svg");
  const ranks = new Map([[nodes[0].id, 0]]), queue = [nodes[0]];
  while (queue.length) { const node = queue.shift(); for (const edge of edgesFor(node, flow)) if (!ranks.has(edge.to)) { ranks.set(edge.to, ranks.get(node.id) + 1); queue.push(nodes.find((n) => n.id === edge.to)); } }
  const isolated = nodes.every((node) => !node.edges.length), columns = new Map(), positions = new Map();
  nodes.forEach((node, i) => { const rank = isolated ? i % 4 : ranks.get(node.id) ?? 0; const column = columns.get(rank) || []; column.push(node); columns.set(rank, column); });
  const cols = Math.max(...columns.keys()) + 1, rows = Math.max(...[...columns.values()].map((column) => column.length));
  const w = cols * 205, h = rows * 72 + 20;
  for (const [rank, column] of columns) column.forEach((node, i) => positions.set(node.id, { x: rank * 205 + 8, y: i * 72 + (rows - column.length) * 36 + 10 }));
  svg.setAttribute("viewBox", `0 0 ${w} ${h}`); svg.style.height = `${Math.max(160, h)}px`;
  const defs = document.createElementNS(ns, "defs"), marker = document.createElementNS(ns, "marker"), arrow = document.createElementNS(ns, "path");
  marker.id = "arrow"; marker.setAttribute("viewBox", "0 0 10 10"); marker.setAttribute("refX", "9"); marker.setAttribute("refY", "5"); marker.setAttribute("markerWidth", "7"); marker.setAttribute("markerHeight", "7"); marker.setAttribute("orient", "auto-start-reverse"); arrow.setAttribute("d", "M0 0 L10 5 L0 10z"); arrow.setAttribute("fill", "var(--muted)"); marker.append(arrow); defs.append(marker); svg.append(defs);
  const position = (id) => positions.get(id);
  for (const node of nodes) for (const edge of edgesFor(node, flow)) {
    const from = position(node.id), to = position(edge.to), path = document.createElementNS(ns, "path");
    path.setAttribute("d", `M${from.x + 185},${from.y + 23} C${from.x + 220},${from.y + 23} ${to.x - 35},${to.y + 23} ${to.x},${to.y + 23}`);
    path.setAttribute("class", "edge"); path.setAttribute("marker-end", "url(#arrow)");
    const label = document.createElementNS(ns, "title"); label.textContent = edge.label; path.append(label);
    if (node.id === current.node.id || edge.to === current.node.id) { path.style.stroke = "var(--accent)"; path.style.opacity = "1"; path.style.strokeWidth = "2"; }
    svg.append(path);
  }
  for (const node of nodes) {
    const p = position(node.id), group = document.createElementNS(ns, "g"), rect = document.createElementNS(ns, "rect"), text = document.createElementNS(ns, "text");
    group.setAttribute("class", `node ${node.kind} ${node.id === current.node.id ? "active" : ""}`);
    group.setAttribute("tabindex", "0"); group.setAttribute("role", "button"); group.setAttribute("aria-label", node.title);
    rect.setAttribute("x", p.x); rect.setAttribute("y", p.y); rect.setAttribute("width", 185); rect.setAttribute("height", 45); rect.setAttribute("rx", 7);
    text.setAttribute("x", p.x + 8);
    const lines = [""]; for (const word of node.title.split(" ")) { if ((lines.at(-1) + " " + word).trim().length > 27 && lines.length < 2) lines.push(word); else lines[lines.length - 1] = (lines.at(-1) + " " + word).trim(); }
    text.setAttribute("y", p.y + (lines.length === 1 ? 27 : 19));
    lines.forEach((line, i) => { const span = document.createElementNS(ns, "tspan"); span.setAttribute("x", p.x + 8); span.setAttribute("dy", i ? 14 : 0); span.textContent = line; text.append(span); });
    group.append(rect, text); group.onclick = () => select(entries.find((e) => e.flow === flow && e.node === node));
    group.onkeydown = (event) => { if (["Enter", " "].includes(event.key)) { event.preventDefault(); group.onclick(); } };
    svg.append(group);
  }
  $("map").replaceChildren(svg);
}
export async function select(entry) {
  observer?.disconnect(); clearTimeout(refresh);
  if (entry.node.platform !== "shared" && entry.node.platform !== context().platform) { $("platform").value = entry.node.platform; inspected.clear(); }
  if (entry.node.operations && !entry.node.operations.includes(context().operation)) { $("operation").value = entry.node.operations[0]; inspected.clear(); }
  if (current && current.id !== entry.id) selectedVariant = "";
  current = entry; const generation = ++activeFrame;
  $("operation-label").hidden = entry.flow.id !== "firmware";
  $("variant-label").hidden = !entry.node.variants;
  $("variant").replaceChildren();
  for (const v of entry.node.variants || []) { const option = el("option", v.label); option.value = v.id; $("variant").append(option); }
  if (entry.node.variants) { selectedVariant = entry.node.variants.some((v) => v.id === selectedVariant) ? selectedVariant : entry.node.defaultVariant || entry.node.variants[0].id; $("variant").value = selectedVariant; }
  $("platform-info").textContent = `${entry.node.platform === "shared" ? "Shared UI" : entry.node.platform === "android" ? "Android only" : "Pure web UI only"} · Previewing ${context().platform === "android" ? "Android app" : "pure web UI"}. ${entry.node.description || (entry.flow.id === "firmware" ? "USB operations through libjollymaam. Shared states vary by the selected action; Upgrade requires our firmware." : entry.flow.id === "update" ? "Browser JavaScript updater over WebHID or Web Bluetooth." : "")}`;
  drawWorkflows();
  $("title").textContent = entry.node.title; $("identity").textContent = entry.id;
  $("notes").value = notes.get(entry.id) || "";
  const url = new URL(location.href); url.searchParams.set("state", entry.id); url.searchParams.set("board", $("board").value); url.searchParams.set("platform", context().platform); url.searchParams.set("operation", context().operation); if (entry.node.variants) url.searchParams.set("variant", selectedVariant); else url.searchParams.delete("variant"); history.replaceState(null, "", url);
  const full = previewURL(entry.node, location.href, context()); full.searchParams.delete("review"); $("open").href = full.href;
  $("transitions").replaceChildren();
  for (const edge of edgesFor(entry.node, entry.flow)) {
    const target = entries.find((e) => e.flow === entry.flow && e.node.id === edge.to), button = el("button", `${edge.label} → ${target.node.title}`);
    button.onclick = () => select(target); $("transitions").append(button);
  }
  if (!edgesFor(entry.node, entry.flow).length) $("transitions").append(el("small", "End state. Click any node in the map to inspect another branch."));
  for (const button of $("workflows").children) button.setAttribute("aria-current", String(button.dataset.flow === entry.flow.id));
  drawMap(); drawIndex(); drawMessages(); $("status").textContent = "Replaying to checkpoint…";
  try {
    const result = await load($("preview"), entry, () => generation === activeFrame);
    if (!result) return;
    inspected.set(entry.id, { messages: result });
    observer = new MutationObserver(() => {
      clearTimeout(refresh); refresh = setTimeout(() => {
        if (generation !== activeFrame) return;
        inspected.set(entry.id, { messages: messages($("preview"), entry.node) }); drawMessages(); drawIndex(); coverage();
      }, 100);
    });
    observer.observe($("preview").contentDocument.body, { subtree: true, childList: true, characterData: true, attributes: true });
    $("status").textContent = "Checkpoint ready — interact with the real UI below, or choose a branch above.";
  } catch (error) { if (generation !== activeFrame) return; inspected.set(entry.id, { error: error.message, messages: [] }); $("status").textContent = error.message; }
  if (generation === activeFrame) { drawMessages(); drawIndex(); coverage(); }
}

async function collect() {
  if (scanning) { scanning = false; scanGeneration++; $("scan").textContent = "Collect all messages"; coverage(); return; }
  scanning = true; const generation = ++scanGeneration; $("scan").textContent = "Stop collection";
  const frame = el("iframe"); frame.setAttribute("aria-hidden", "true"); frame.tabIndex = -1;
  frame.style.cssText = "position:fixed;left:-12000px;width:1000px;height:900px;opacity:0;pointer-events:none"; document.body.append(frame);
  coverage();
  for (const entry of available()) {
    if (generation !== scanGeneration) break;
    if (!inspected.has(entry.id)) {
      try { const result = await load(frame, entry, () => generation === scanGeneration); if (result) inspected.set(entry.id, { messages: result }); }
      catch (error) { inspected.set(entry.id, { error: error.message, messages: [] }); }
    }
    coverage(); drawIndex(); if (current.id === entry.id) drawMessages();
  }
  frame.remove();
  if (generation === scanGeneration) { scanning = false; $("scan").textContent = "Collect all messages"; coverage(); }
}
function drawWorkflows() {
  $("workflows").replaceChildren();
  for (const flow of WORKFLOWS) {
    const first = available().find((e) => e.flow === flow); if (!first) continue;
    const b = el("button"); b.append(el("span", flow.title), el("small", flow.platform === "shared" ? "Shared" : flow.platform === "android" ? "Android only" : "Browser only"));
    b.dataset.flow = flow.id; b.onclick = () => select(first); $("workflows").append(b);
  }
}
function changeContext() {
  if (scanning) collect(); inspected.clear();
  select(available().find((e) => e.id === current.id) || available().find((e) => e.flow === current.flow) || available()[0]); coverage();
}
$("platform").onchange = $("operation").onchange = changeContext;
$("variant").onchange = () => { selectedVariant = $("variant").value; inspected.delete(current.id); select(current); };
$("search").oninput = $("filter").onchange = drawIndex;
$("notes").oninput = () => notes.set(current.id, $("notes").value);
$("board").onchange = () => { if (scanning) collect(); inspected.clear(); select(current); coverage(); };
$("scan").onclick = collect;
$("copy").onclick = async () => {
  const text = `State: ${current.id}\n${location.href}\n\n${(inspected.get(current.id)?.messages || []).map((m) => `${m.text}\n${m.keys.join(", ") || m.source}`).join("\n\n")}\n\nRequested change: ${notes.get(current.id) || ""}`;
  try { await navigator.clipboard.writeText(text); $("notice").textContent = "State reference copied."; }
  catch { $("notes").value = text; $("notes").select(); $("notice").textContent = "Select and copy the reference from the text box."; }
};
$("export").onclick = () => {
  const url = URL.createObjectURL(new Blob([JSON.stringify({ platform: context().platform, operation: context().operation, variant: context().variant, board: context().board, language: context().lang, states: available().map((e) => ({ state: e.id, title: e.node.title, ...inspected.get(e.id), note: notes.get(e.id) || "" })) }, null, 2)], { type: "application/json" }));
  const a = el("a"); a.href = url; a.download = "ui-review.json"; a.click(); setTimeout(() => URL.revokeObjectURL(url), 1000);
};
function drawCatalog() {
  const query = $("catalog-search").value.toLowerCase(); $("catalog").replaceChildren();
  for (const [key, value] of Object.entries(source)) if (`${key} ${value}`.toLowerCase().includes(query)) {
    const row = el("article"); row.append(el("code", key), el("p", value)); $("catalog").append(row);
  }
}
$("catalog-search").oninput = drawCatalog; drawCatalog();
select(requested || available()[0]); coverage();
// Indexing is explicit: the current preview stays interactive during collection.
