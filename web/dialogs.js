// Native dialogs in production; the review harness can inspect their text.
let handler = null;
export function useDialogs(value) { handler = value; }
export const ask = (message) => handler ? handler("confirm", message) : globalThis.confirm(message);
export const tell = (message) => handler ? handler("alert", message) : globalThis.alert(message);
