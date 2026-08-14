<!-- DESIGN PHILOSOPHY: at-node is an AI agent peripheral — the hands and feet of LLMs. -->
# at-node Design Philosophy

> Cross-platform design decisions and technology rationale.
> **CH582 implementation details** (architecture, init sequence, constraints, extension guides):
> see [`wchble/mr2/DESIGN.md`](wchble/mr2/DESIGN.md).
> **AT command reference**: see [`wchble/mr2/USER-MANUAL.md`](wchble/mr2/USER-MANUAL.md).
> **ESP32-C3 variant**: see [`esp32/README.md`](esp32/README.md).

---

## 1. Platform Roadmap

| Platform | MCU | Stack | Status | Key Feature |
|----------|-----|-------|--------|-------------|
| **at-node** | CH582F | Bare-metal + WCH SDK | ✅ Active | BLE HID + USB CDC + AT Parser |
| **at-node-esp** | ESP32-C3 | Arduino + NimBLE | ✅ Active | WiFi HTTP + MQTT + BLE HID |
| **at-node-nrf** | nRF52840 | Zephyr RTOS | 📋 Planned | All BLE/USB via Kconfig |

### Why CH582F is the primary target

Despite the painful SDK, CH582F wins on cost and accessibility:

- **Chip cost**: ~$0.50 (vs nRF52840 ~$2.50, ESP32-C3 ~$1.50)
- **Family compatibility**: Code is portable to CH592 (lower power, ~$0.30) and CH572 (USB-only) with minimal changes
- **PCB simplicity**: QFN48, single 3.3V rail, few passives — 2-layer board is trivial
- **Replication cost**: Bare-minimum BOM under $3
- **Yield**: One reflow pass, no fine-pitch BGA, hand-solderable in small batches

**Verdict**: Develop on CH582F (cheap enough to fry), port to CH592 for production volume. Keep nRF/ESP as premium variants for users who need Zephyr/WiFi.

---

## 2. Platform Design Philosophy

All variants share the same AT command interface. Agent code (Python) works identically regardless of transport:

```
CH582F:  AT commands via USB CDC / UART1
ESP32-C3: AT commands via WiFi HTTP / MQTT / USB serial
nRF52:   AT commands via USB CDC / BLE NUS (Zephyr config)
```

The transport layer is abstracted; the AT parser and command handlers are the common core.

### Design invariants across platforms

| Invariant | Rationale |
|-----------|-----------|
| AT command semantics identical | Agent code portable, no per-platform translation |
| Response format: `OK` / `ERROR:<reason>` / `+<URC>:<data>` | Predictable parsing for LLM pipelines |
| Keyboard output = HID boot protocol (8-byte report) | Universal OS support, no driver needed |
| No GUI, no app, no cloud dependency | Device is a serial-attached tool, not a product |
| Security warning mandatory | Keyboard emulation = unrestricted input control |

---

## 3. Technology Experiment: Rust Core Logic (future)

Plan to implement portable business logic in Rust (`no_std`, `extern "C"` FFI) as an optional alternative to C.

| Module | Language | Reason |
|--------|----------|--------|
| AT parser, command dispatch | **Rust** | `match` enum, compile-time exhaustiveness |
| I²C sensor decoders | **Rust** | `Option`/`Result`, no null pointer bugs |
| USB ISR, GPIO, TIM, BLE stack | **C** | register access, WCH SDK, pre-compiled libs |

**Build**: `cargo build --target riscv32imac-unknown-none-elf` → `libat_rust.a` → link with C firmware. All Rust logic tested on host (`cargo test`) before deploying to CH582F.

**Status**: Not started. C implementation is stable and sufficient. Rust migration is a code quality experiment, not a product requirement.

---

## 4. Documentation Map

| Document | Scope | Audience |
|----------|-------|----------|
| `AGENTS.md` | AI agent quick-start: architecture, build commands, constraints | AI agents |
| `DESIGN.md` (this file) | Cross-platform philosophy, technology choices | Architects, AI agents |
| `wchble/mr2/DESIGN.md` | CH582 architecture, layers, init, extension guides, coding conventions | Firmware developers |
| `wchble/mr2/USER-MANUAL.md` | AT command reference (all variants, parameters, workflows) | Agent developers, users |
| `wchble/mr2/FIELD-NOTES.md` | Debugging war stories (root cause + fix + lesson) | Debuggers |
| `wchble/mr2/POWER.md` | Low-power design principles and quantitative analysis | Power engineers |
| `REQUIREMENTS.md` | Feature requirements and implementation status | PM, AI agents |
| `esp32/README.md` | ESP32-C3 variant overview and quick start | ESP32 developers |
| `esp32/API.md` | ESP32-C3 HTTP API reference | Agent developers |
| `tools/README.md` | Tool scripts directory guide | Testers |
| `.pi/skills/` | Platform-specific operational playbooks | AI agents (ops) |

**Rule**: implementation details live in the platform subdirectory (`wchble/`, `esp32/`, `nordic/`). Root documents are cross-platform only.
