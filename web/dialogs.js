// Native dialogs in production; the review harness can inspect their text.
let handler = null;
export function useDialogs(value) { handler = value; }
export const ask = (message) => handler ? handler("confirm", message) : globalThis.confirm(message);
export const tell = (message) => handler ? handler("alert", message) : globalThis.alert(message);

// One of choices ([value, label]...), the last being the safe one (Esc).
export function choose(message, choices) {
  const safe = choices.at(-1)[0];
  if (handler) return Promise.resolve(handler("choose", message, choices) || safe);
  const box = document.getElementById("choosebox");
  box.querySelector("p").textContent = message;
  const row = box.querySelector(".actions");
  row.replaceChildren();
  return new Promise((resolve) => {
    box.onclose = () => resolve(box.returnValue || safe);
    choices.forEach(([value, label], i) => {
      const b = document.createElement("button");
      b.type = "button"; b.textContent = label; b.className = i ? "secondary" : "";
      b.onclick = () => box.close(value);
      row.append(b);
    });
    box.returnValue = "";
    box.showModal();
    row.querySelector("button").focus();
  });
}
