// The page: connect to a pad in config mode (Home+M held for 3 s), show what
// it is and its settings.  Read only for now.
import { links } from "./link-web.js";
import { Pad, TARGETS, KINDS, kindOf, targetsFor, showValue } from "./pad.js";

const $ = (id) => document.getElementById(id);
const STATUS_EVERY_MS = 5000;    // also keeps config mode open (it ends a minute after the last command)

function h(tag, attrs = {}, ...children) {
  const e = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs)) {
    if (k === "class") e.className = v;
    else if (v !== undefined && v !== null && v !== false) e.setAttribute(k, v === true ? "" : v);
  }
  e.append(...children.flat().filter((c) => c !== null && c !== undefined));
  return e;
}

let pad = null, poll = null;

function say(message, error = false) {
  $("message").textContent = message;
  $("message").classList.toggle("error", error);
}

function showIdentity(id, link) {
  $("identity").replaceChildren(h("dl", {},
    h("dt", {}, "Board"), h("dd", {}, id.boardName || "?"),
    h("dt", {}, "Mode"), h("dd", {}, id.updater ? "updater" : "config mode"),
    h("dt", {}, "Product"), h("dd", {}, id.product),
    h("dt", {}, "Build"), h("dd", {}, id.build),
    h("dt", {}, "Flash ID"), h("dd", { class: "mono" }, id.flashId),
    h("dt", {}, "Protocol"), h("dd", {}, String(id.protocol)),
    h("dt", {}, "Bluetooth"), h("dd", {}, link.name)));
}

function showStatus(s) {
  const battery = [s.percent !== null ? `${s.percent}%` : null, s.mv !== null ? `${s.mv} mV` : null,
    s.charging, s.low ? "low" : null].filter(Boolean).join(", ");
  $("status").textContent = [`state ${s.state}`, s.slot ? `slot ${s.slot}` : null,
    battery ? `battery ${battery}` : null, `up ${s.uptime.toFixed(0)} s`,
    s.ephemeral ? "ephemeral" : null].filter(Boolean).join(" · ");
}

function targetSection(table, t, values, defaults) {
  const rows = table.filter((f) => f.kind === kindOf(t) && values.has(f.name)).map((f) => {
    const v = values.get(f.name), d = defaults.get(f.name);
    const shown = showValue(f, v.value), def = d ? showValue(f, d.value) : "?";
    return h("tr", { class: f.dev ? "dev" : null },
      h("th", { scope: "row", title: f.help || null }, f.name),
      h("td", { class: "mono" }, shown),
      h("td", { class: "mono muted" }, v.set && shown !== def ? def : ""),
      h("td", { class: "mark", title: v.set ? "saved on the pad" : "firmware default" }, v.set ? "✓" : ""));
  });
  return h("details", {},
    h("summary", {}, TARGETS[t], h("span", { class: "muted" }, ` ${KINDS[kindOf(t)]}`)),
    rows.length ? h("table", {}, h("thead", {}, h("tr", {}, h("th", {}, "Setting"), h("th", {}, "Value"),
      h("th", {}, "Default"), h("th", {}, ""))), h("tbody", {}, rows)) : h("p", { class: "muted" }, "Nothing here."));
}

async function connect() {
  disconnect();
  say("Choose your pad (hold Home+M for 3 s to put it in config mode).");
  let link;
  try {
    link = await links.choose();
  } catch (error) {
    say(error.name === "NotFoundError" ? "No pad chosen." : String(error.message || error), error.name !== "NotFoundError");
    return;
  }
  pad = new Pad(link);
  pad.onDisconnect = () => { stop(); say("The pad went away (config mode ends a minute after the last command, or on Home)."); };
  $("connect").hidden = true;
  $("disconnect").hidden = false;
  try {
    say("Asking the pad what it is…");
    const id = await pad.identify();
    showIdentity(id, link);
    if (id.updater) {
      say("The pad is in its updater.");
      return;
    }
    showStatus(await pad.status());
    poll = setInterval(async () => {
      try { showStatus(await pad.status()); } catch { /* the disconnect handler says it */ }
    }, STATUS_EVERY_MS);
    say("Reading its settings…");
    const table = await pad.fields();
    const sections = [];
    for (const t of targetsFor(table)) {
      sections.push(targetSection(table, t, await pad.read(table, t), await pad.read(table, t, true)));
    }
    $("settings").replaceChildren(...sections);
    say(`${table.length} settings in ${sections.length} groups.`);
  } catch (error) {
    say(String(error.message || error), true);
  }
}

function stop() {
  clearInterval(poll);
  poll = null;
  $("connect").hidden = false;
  $("disconnect").hidden = true;
}

function disconnect() {
  stop();
  if (pad) {
    pad.onDisconnect = null;
    pad.close();
    pad = null;
  }
  $("identity").replaceChildren();
  $("status").textContent = "";
  $("settings").replaceChildren();
}

$("connect").addEventListener("click", connect);
$("disconnect").addEventListener("click", () => { disconnect(); say("Disconnected."); });
$("dev").addEventListener("change", (e) => document.body.classList.toggle("show-dev", e.target.checked));
if (!links.available()) {
  $("connect").disabled = true;
  say("This browser has no Web Bluetooth: use Chrome or Edge (on Linux, enable it in chrome://flags).", true);
}
