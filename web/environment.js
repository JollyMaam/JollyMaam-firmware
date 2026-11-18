// Select all providers before the UI starts. Mock mode never imports the
// hardware transports or Android bridge.
const params = new URLSearchParams(location.search);
export let bluetooth, usb, android = null, mock = null;
if (params.has("mock")) {
  const { createMock } = await import("./mock.js");
  const response = await fetch(new URL("./mock-fields.json", import.meta.url));
  if (!response.ok) throw new Error("Cannot load mock setting schema.");
  mock = await createMock(params, await response.json());
  ({ bluetooth, usb, android } = mock);
} else {
  const [ble, hid, native] = await Promise.all([
    import("./link-web.js"), import("./link-hid.js"), import("./link-android.js"),
  ]);
  android = native.android;
  bluetooth = android?.bluetooth ?? ble.links;
  usb = android?.usb ?? hid.links;
}
