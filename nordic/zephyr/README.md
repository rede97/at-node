# nordic/zephyr/ — nRF52840 Zephyr variant (TODO)

> **Status: TODO — no code yet.** Reserved home of the Zephyr-based AT Node
> for the Nordic nRF52840.

## Target hardware

| Chip | Rationale |
|---|---|
| **nRF52840** | Cortex-M4F 64 MHz, 1 MB Flash / 256 KB RAM, BLE 5 + USB device. The reference dongle/keyboard hardware of the BLE ecosystem; nRF Connect SDK (Zephyr) is the vendor-supported stack. |

## Planned scope

- Same AT command semantics as the other variants (see [../../REQUIREMENTS.md](../../REQUIREMENTS.md)).
- BLE HID keyboard (Peripheral) + USB CDC/HID composite — feature parity target is the CH582 variant ([../../wchble/mr2/](../../wchble/mr2/)), which is the closest role match.
- No WiFi on-chip; network control plane is out of scope here (that is the ESP32 series' role).

## Entry criteria

- CH582 variant ([../../wchble/mr2/](../../wchble/mr2/)) defines the AT/HID behavior to port.
- Port starts when nRF52840 hardware joins the test rig; until then this stays a placeholder.
