# esp32/zephyr/ — ESP32 Zephyr variant (TODO)

> **Status: TODO — no code yet.** This directory is the reserved home of the
> Zephyr-based AT Node for high-performance ESP32 chips.

## Target hardware

| Chip | Why Zephyr |
|---|---|
| **ESP32-S3** (primary) | Dual-core 240 MHz, 8 MB PSRAM. Blocked on Arduino — precompiled `esp32s3-libs` ship `CONFIG_SPIRAM_USE_MALLOC=y`, which crashes mbedTLS (`WiFiClientSecure`) at boot and cannot be fixed from app-level config. Root cause analysis: [../COMPAT_REPORT.md](../COMPAT_REPORT.md) |
| Other PSRAM ESP32 variants (ESP32-P4 etc.) | Same reasoning: full control over Kconfig, no opaque precompiled blobs |

## Planned scope

- Same AT command semantics as the other variants (see [../../REQUIREMENTS.md](../../REQUIREMENTS.md)).
- WiFi HTTP + MQTT (TLS) control plane, BLE HID keyboard output (NimBLE host on Zephyr).
- PSRAM-backed workloads that do not fit the C3: larger web assets, TLS with full CA bundles, more concurrent tunnels.

## Entry criteria

- Arduino variant ([../arduino/](../arduino/)) remains the reference implementation for AT semantics.
- Port starts when S3-class hardware is actually needed; until then this stays a placeholder.
