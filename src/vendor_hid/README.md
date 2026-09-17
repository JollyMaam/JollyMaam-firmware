# Retained SDK application support

This directory started as a copy of `fw-AC63_BT_SDK/apps/hid`. It is now a
reduced, locally maintained adaptation, not a source-identical vendor copy.
The firmware does not include files from the SDK's `apps/hid` directory.

Retained pieces are:

- BR23 board initialization, power callbacks, and linker fragments;
- configuration globals consumed by the SDK archives;
- BLE HOGP and classic Bluetooth HID transport support;
- battery monitoring, syscfg setup, and shared SDK-facing headers.

The SDK's shared `apps/common`, `cpu/br23`, headers, and binary archives remain
external inputs.  Their exact baseline source lists and flags are recorded in
the repository's `Makefile` and `sdk-br23.mk`.

Build the single supported firmware with `make`. Application logic starts at
`../main.c` and continues into `../controller.c`; profiles live in `../profile*.c`.

Unused demo applications, their board variant, audio AEC implementation, and
the old `APPLICATION=cyclone2` experiment have been removed. Disabled branches
in retained C files were specialized for the current build after comparing
preprocessed output. Enabling a previously unsupported SDK feature may require
restoring its implementation from the SDK or git history, not just setting a macro.

`TRANSLATED:` identifies English translations of original Chinese comments.
Those comments describe the SDK's intent, not necessarily verified hardware
behavior. Some original SDK comments have mistakes, noted in the translation.

Do not remove a configuration global or callback solely because no C caller is
visible: binary archives, linker tables, and weak hooks also reference them.
Shared SDK-facing structure layouts and syscfg IDs are intentionally preserved.
