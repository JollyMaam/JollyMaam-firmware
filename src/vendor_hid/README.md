# Vendor HID baseline

This directory is a source-identical import of the BR23 HID application from
`fw-AC63_BT_SDK/apps/hid`.  It exists to provide a known-working baseline that
can be changed and reduced without depending on files beneath the SDK's
`apps/hid` directory.

Imported files are limited to:

- the HID application's headers and application sources;
- the BR23 board sources and linker fragments;
- only the `.c` files listed by the vendor BR23 HID Makefile.

The SDK's shared `apps/common`, `cpu/br23`, headers, and binary archives remain
external inputs.  Their exact baseline source lists and flags are recorded in
the repository's `Makefile` and `sdk-br23.mk`.

The default build uses this baseline.  The earlier Cyclone 2 experiment remains
available with:

```sh
make clean
make APPLICATION=cyclone2
```
