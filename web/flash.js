// The Flash panel, in the Android app only: firmware through the app's
// flasher (link-android.js, libjollymaam over JNI), for what the page
// cannot reach itself: a pad in its vendor boot loader (the Jieli ROM
// loader is USB mass storage, the 8BitDo one a HID loader), and our
// firmware's way into that loader.  Our firmware or updater on USB can be
// updated here too (through our updater, as the Firmware update fold does).
import { $, h } from "./dom.js";

const KINDS = ["our firmware", "8BitDo boot loader", "Jieli boot loader"];

export function setupFlasher(flasher) {
  let targets = null, busy = false;

  const info = (text, error = false) => {
    $("flashinfo").textContent = text;
    $("flashinfo").classList.toggle("error", error);
  };
  const log = (line) => { $("flashlog").textContent += line + "\n"; };
  const lock = (on) => {
    busy = on;
    for (const b of document.querySelectorAll("#flasher button, #flasher input, #flasher select")) b.disabled = on;
  };

  async function scan() {
    $("flashactions").replaceChildren();
    $("flashdevices").replaceChildren(h("li", { class: "muted" }, "Scanning…"));
    let found;
    try {
      found = await flasher.scan();
    } catch (error) {
      return $("flashdevices").replaceChildren(h("li", { class: "error" }, error.message));
    }
    $("flashdevices").replaceChildren(...(found.length ? found.map((d) => h("li", {},
      button(`${d.name} · ${d.id} · ${KINDS[d.kind]}`, () => pick(d), "secondary"))) :
      [h("li", { class: "muted" }, "Nothing on USB the flasher knows: plug the pad in (in its boot loader, " +
        "or running our firmware), then Scan again.")]));
  }

  function button(text, click, cls = null) {
    const b = h("button", { type: "button", class: cls }, text);
    b.addEventListener("click", click);
    return b;
  }

  const forProduct = (p) => targets?.find((t) => t.product === p) ?? null;

  // What the device is, and what can be done to it.
  async function pick(d) {
    $("flashactions").replaceChildren(h("p", { class: "muted" }, `${d.name}: asking what it is…`));
    let id;
    try {
      lock(true);
      id = await flasher.probe(d.device);
    } catch (error) {
      return $("flashactions").replaceChildren(h("p", { class: "error" }, `${d.name}: ${error.message}`));
    } finally {
      lock(false);
    }
    const rows = [], actions = [];
    const need = targets ? null : h("p", { class: "muted" }, "Choose a firmware package to write one.");
    if (id.kind === 0) {
      rows.push(`${id.product}, ${id.updater ? "in its updater" : "running"}, build ${id.build}`, `flash ID ${id.uid}`);
      const t = forProduct(id.product);
      if (t) actions.push(button(`Update to the package's ${t.board}`, () => write(d, t, id.uid, `update ${id.product}`)));
      else if (targets) rows.push(`The package has no firmware for ${id.product}.`);
      actions.push(button("Into the stock boot loader", () => stockLoader(d, id), "secondary"));
    } else if (id.kind === 1) {
      rows.push("The 8BitDo Ultimate 2C Wireless boot loader.");
      const t = forProduct(id.product);
      if (t) actions.push(button(`Write ${t.board}`, () => write(d, t, null, "write it")));
      else if (targets) rows.push("The package has no firmware for it.");
    } else {
      rows.push(`A Jieli ${id.chip} boot loader: which pad it is cannot be told here; ` +
        "the flasher checks the installed firmware's product before it erases.");
      const choices = (targets ?? []).filter((t) => t.chip === id.chip);
      if (choices.length) {
        const select = h("select", { "aria-label": "Which pad" }, choices.map((t, i) => h("option", { value: i }, t.board)));
        actions.push(select, button("Write", () => write(d, choices[select.value], null, "write it")));
      } else if (targets) rows.push(`The package has no firmware for a ${id.chip} pad.`);
    }
    $("flashactions").replaceChildren(...rows.map((r) => h("p", {}, r)), need, h("div", { class: "actions" }, actions));
  }

  async function write(d, target, uid, what) {
    if (busy || !confirm(`${d.name}: ${what} with ${target.board}'s firmware? Keep the pad plugged in until it is done.`)) return;
    lock(true);
    $("flashlog").textContent = "";
    $("flashprogress").hidden = false;
    $("flashprogress").value = 0;
    log(`${target.board} (${target.product}) onto ${d.name}`);
    const t0 = performance.now();
    try {
      await flasher.run(d.device, target.product, uid, (done, total) => { $("flashprogress").value = total ? done / total : 0; });
      log(`written and checked in ${((performance.now() - t0) / 1000).toFixed(1)} s; the pad starts it`);
    } catch (error) {
      log(error.message);
    } finally {
      $("flashprogress").hidden = true;
      lock(false);
      scan();
    }
  }

  async function stockLoader(d, id) {
    if (busy || !confirm(`Restart ${id.product} into its stock boot loader? Nothing is written; Scan again after.`)) return;
    lock(true);
    try {
      await flasher.stockLoader(d.device, id.uid);
      log(`${id.product}: restarting into its boot loader; Scan again in a moment.`);
    } catch (error) {
      log(`${id.product}: ${error.message}`);
    } finally {
      lock(false);
      $("flashactions").replaceChildren();
    }
  }

  $("flashpkg").addEventListener("change", async (e) => {
    targets = null;
    const f = e.target.files[0];
    if (!f) return info("");
    try {
      lock(true);
      targets = await flasher.package(new Uint8Array(await f.arrayBuffer()));
      info(`${f.name}: ${targets.map((t) => t.board).join(", ")}`);
    } catch (error) {
      info(`${f.name}: ${error.message}`, true);
    } finally {
      lock(false);
    }
    $("flashactions").replaceChildren();
  });
  $("flashscan").addEventListener("click", scan);

  // The panel: shown by the Flash button, scanning at once.
  return {
    toggle() {
      const show = $("flasher").hidden;
      $("flasher").hidden = !show;
      if (show) scan();
    },
    hide() { if (!busy) $("flasher").hidden = true; },
  };
}
