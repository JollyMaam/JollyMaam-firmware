// Small DOM helpers for the page.
export const $ = (id) => document.getElementById(id);

// An element: attributes ("class", others as given; false/null left out),
// then children (nested arrays flattened, null left out).
export function h(tag, attrs = {}, ...children) {
  const e = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs)) {
    if (k === "class") e.className = v;
    else if (v !== undefined && v !== null && v !== false) e.setAttribute(k, v === true ? "" : v);
  }
  e.append(...children.flat().filter((c) => c !== null && c !== undefined));
  return e;
}

export function svg(tag, attrs = {}) {
  const e = document.createElementNS("http://www.w3.org/2000/svg", tag);
  for (const [k, v] of Object.entries(attrs)) e.setAttribute(k, v);
  return e;
}

// Set a form control as if the person did, so its listeners run.
export function setControl(input, value, event = "input") {
  if (input.value === value) return;
  input.value = value;
  input.dispatchEvent(new Event(event));
}
