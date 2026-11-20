// A guided tour drives production controls. Each checkpoint waits for Next;
// Previous reloads and replays to the requested checkpoint with fresh devices.
export function tourSteps({ scenario, app, outcome, packageState = "present", workflow = "", answer = "accept" }) {
  const step = (name, description, action = "none") => ({ name, description, action });
  const steps = [step("Home", "Start here. Next performs the next action; Play advances every three seconds.")];
  if (["install", "backup", "upgrade", "restore"].includes(workflow)) {
    const prefix = [step("Home", "Device discovery."), step("Devices", "Devices discovered.", "discovery"),
      step("Device", "Device selected: its page.", "manage")];
    if (["manual", "probe_error", "no_devices"].includes(scenario) || packageState !== "present") return prefix;
    prefix.push(step("Action", "Action options before starting.", "jobOpen"));
    if (answer === "cancel") return [...prefix, step("Confirmation", "The real confirmation text; the mock declines it.", "jobPrompt")];
    prefix.push(step("In progress", "Operation held at 30%.", "jobStart"));
    return [...prefix, step(outcome === "failure" ? "Failed" : "Completed", "The real action result.", outcome === "busy" ? "jobBusy" : "jobFinish")];
  }
  if (workflow === "package") {
    const prefix = app
      ? [step("Home", "Devices."), step("Devices", "Devices discovered.", "discovery"), step("Device", "A device page.", "manage")]
      : [step("Home", "Before connecting."), step("Connected", "Controller connected.", "connect"), step("Firmware", "Package information.", "firmware")];
    prefix.push(step("Package result", "The normal package reader handles the selected fixture.", "packageLoad"));
    return [...prefix, step("Package unloaded", "Return to the bundled package if available.", "packageUnload")];
  }
  if (workflow === "pairing") return [step("Home", "Before connecting."), step("Connected", "Controller settings.", "connect"), step("Changed radio", "A radio edit clears pairing.", "radioEdit"), step("Pairing warning", "Confirmation before saving.", "savePrompt")];
  if (workflow === "discard") return [step("Home", "Before connecting."), step("Connected", "Controller settings.", "connect"), step("Editor", "Base remap.", "help"), step("Unsaved", "An edit is pending.", "edit"), step("Discard warning", "Confirmation before leaving.", "discardPrompt")];
  if (["update", "remap", "macros", "quickmacro"].includes(workflow)) {
    const prefix = [step("Home", "Before connecting."), step("Connected", "Controller settings loaded.", "connect")];
    if (workflow === "update") {
      prefix.push(step("Firmware", "Firmware availability.", "firmware"));
      if (packageState !== "present") return prefix;
      if (answer === "cancel") return [...prefix, step("Confirmation", "The browser confirmation text.", "updatePrompt")];
      return [...prefix, step("Writing", "Updater held while writing.", "updateStart"),
        step(outcome === "failure" ? "Update failed" : "Updated", "The updater result.", "updateFinish")];
    }
    prefix.push(step("Editor", "Base remap editor.", "help"));
    return [...prefix, step("Editor message", "An actual import or validation outcome.", workflow === "remap" ? "presetLoad" : workflow === "quickmacro" ? "quickMacroError" : "macroError")];
  }
  if (scenario === "unavailable") return [...steps, step("Unavailable APIs", "The normal connection choices explain the missing browser APIs.")];
  if (app && workflow !== "settings") {
    steps.push(step("Devices", ["stock", "manual"].includes(scenario) ? "Stock pads are listed too. Next opens its device page." : "The app discovers simulated devices.", "discovery"),
      step("Device", "The device determines which actions are available.", "manage"));
    if (["no_devices", "probe_error", "manual", "permission"].includes(scenario)) return steps;
    if (packageState !== "present") return [...steps, step("Package unavailable", "The package state disables firmware actions or explains the mismatch.")];
    steps.push(step("Backup", "The real backup action page, before starting the operation.", "backup"),
      step("In progress", "A mock backup is held here so you can inspect progress and disabled controls.", "jobStart"));
    if (outcome === "busy") {
      steps.push(step("Still busy", "The operation stays busy. Restart the tour to leave this state.", "jobBusy"));
    } else {
      steps.push(step(outcome === "failure" ? "Failed" : "Completed",
        outcome === "failure" ? "The operation reports an error and opens its details." : "The result includes a mock backup filename.", "jobFinish"));
    }
    return steps;
  }
  if (scenario === "no_devices") return [...steps, step("No device chosen", "No connection is established.", "connect")];
  if (["incompatible", "read_error", "permission"].includes(scenario))
    return [...steps, step("Connection error", "The normal UI displays the connection or settings error.", "connect")];
  if (["updater", "no_app", "empty"].includes(scenario))
    return [...steps, step("Connected", "An updater or dongle has no editable settings.", "connect"),
      step("Firmware", "Inspect the update information and package availability.", "firmware")];
  steps.push(step("Loading", "Settings are held while loading, so this usually brief state can be inspected.", "loading"),
    step("Settings", "The full settings editor is ready.", "ready"),
    step("Help", "The base remap and one setting's help are expanded.", "help"),
    step("Invalid edit", "An out-of-range value shows validation and disables Save.", "invalid"),
    step("Unsaved edit", "A valid edit enables Save and Discard.", "edit"),
    step("Saving", "The simulated device holds the write while the real UI shows Saving.", "saveStart"));
  if (scenario === "unknown") steps.splice(3, 0, step("Unknown setting", "A newer setting and its choices fall back to firmware names.", "future"));
  if (scenario === "populated") steps.splice(4, 0, step("Rules and macros", "Saved rules and macros are inherited from the base remap.", "macros"));
  if (outcome === "busy") steps.push(step("Still busy", "The write stays pending for inspection. Restart to leave.") );
  else steps.push(step(outcome === "failure" ? "Save refused" : "Saved",
    outcome === "failure" ? "The device refuses the change; the edit is retained." : "The change is saved in memory and a confirmation appears.", "saveFinish"),
    step("Disconnected", "The mock device disappears and the UI returns home.", "disconnect"));
  return steps;
}

export function installTour(panel, options, perform) {
  const steps = tourSteps(options), params = new URLSearchParams(location.search);
  const requested = Math.max(0, Math.min(steps.length - 1, Number(params.get("step")) || 0));
  const tour = document.createElement("section"); tour.id = "mock-tour";
  tour.style.cssText = "width:100%;padding:12px;border:1px solid var(--line);background:var(--panel)";
  const heading = document.createElement("strong"), description = document.createElement("p");
  heading.id = "mock-tour-step"; description.id = "mock-tour-description";
  tour.append(heading, description);
  let index = -1, resume, playing = false, timer, running = false;
  const button = (id, label, action) => {
    const b = document.createElement("button"); b.id = id; b.textContent = label; b.type = "button";
    b.style.marginRight = "8px"; b.onclick = action; tour.append(b); return b;
  };
  function pause() { playing = false; clearTimeout(timer); play.textContent = "Play"; }
  function reload(step) {
    pause(); const url = new URL(location.href); url.searchParams.set("step", step);
    location.replace(url);
  }
  const previous = button("mock-previous", "Previous", () => reload(index - 1));
  const next = button("mock-next", "Next", advance);
  const play = button("mock-play", "Play", () => {
    if (playing) return pause();
    playing = true; play.textContent = "Pause"; advance();
  });
  button("mock-restart", "Restart", () => reload(0));
  panel.prepend(tour);
  function advance() { clearTimeout(timer); resume?.(); }
  function update() {
    heading.textContent = `${index + 1} / ${steps.length} — ${steps[index].name}`;
    description.textContent = steps[index].description;
    previous.disabled = index <= 0; next.disabled = running || index >= steps.length - 1;
    play.disabled = (running && !playing) || index >= steps.length - 1;
    tour.dataset.step = String(index); tour.dataset.ready = String(!running);
  }
  async function run() {
    for (index = 0; index < steps.length; index++) {
      running = true; update();
      await perform(steps[index].action);
      running = false; update();
      const url = new URL(location.href); url.searchParams.set("step", index); history.replaceState(history.state, "", url);
      if (index < requested) continue;
      if (index === steps.length - 1) { pause(); break; }
      await new Promise((resolve) => { resume = () => { resume = null; resolve(); }; if (playing) timer = setTimeout(advance, 3000); });
    }
  }
  run().catch((error) => { pause(); running = false; description.textContent = `Tour stopped: ${error.message}`; next.disabled = play.disabled = true; });
}
