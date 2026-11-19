# Licences of external code and tools

This project is Apache-2.0 (`LICENSE` at the top). What it builds on keeps
its own terms; a firmware image combines several of them.

## SDKs (submodules here)

| Directory | Upstream | Licence |
|---|---|---|
| `bluekitchen-btstack` | BlueKitchen BTstack, via the EBADBEEF/btstack fork | BlueKitchen's BSD-style licence with clause 4: redistribution, use or modification only for personal benefit, not for commercial purposes or monetary gain (`bluekitchen-btstack/LICENSE`). Every Jieli image links BTstack, so these terms apply to the firmware images. |
| `jieli-fw-AC63_BT_SDK` | Jieli-Tech/fw-AC63_BT_SDK | Apache-2.0 (`jieli-fw-AC63_BT_SDK/LICENSE`). The SDK also ships prebuilt libraries without source. |
| `telink-tc_ble_simple_sdk` | telink-semi/tc_ble_simple_sdk | Telink's Apache-2.0 (each source file is marked `TELINK_APACHE` in `telink-tc_ble_simple_sdk/license_list.txt`). |

`src/platform/jieli/btctrler/` is this project's own source for the Jieli
Bluetooth controller, which the SDK ships only as a prebuilt library; it was
written to behave the same as that library.

## Toolchains (`toolchains/`, fetched by `fetch-toolchains.sh`)

Not in the repository; downloaded from the vendors' servers.

| Archive | Contents | Licence |
|---|---|---|
| `jieli-linux-toolchains-20250324.1` | Jieli's clang 4.0.1 based compilers (`pi32`, `pi32v2`, `q32s`) | No licence file in the archive. |
| `tc32_gcc_v2.0` | Telink's GCC 4.5.1 for TC32 | GCC is GPL; no licence file in the archive. |

The YC3121 boards and the host tools build with compilers from nixpkgs
(`flake.nix`), under their own licences.

## Third-party code inside `src/` and `tools/`

Each keeps its notice in the file:

- `src/lib/proto_8bitdo.c`, `tools/padtool/pad8cryptdo.py`: tables from
  8CryptDo (AeroModes), MIT.
- `src/platform/jieli/btctrler/bd19/uECC_curves.inc`: micro-ecc (Kenneth
  MacKay), BSD-2-Clause.
- `android/gradlew`, `android/gradlew.bat`: the Gradle wrapper, Apache-2.0.
