# build-c3.ps1 - Build and upload for ESP32-C3 SuperMini (Windows)
#
# Agent default entry point for the C3 board. Thin wrapper over build.ps1
# with the C3 fqbn pinned (CDCOnBoot=cdc,PartitionScheme=huge_app).
# NEVER flash the C3 with bare arduino-cli / Arduino IDE defaults — a missing
# CDCOnBoot=cdc silently routes Serial to UART0 pads: native-USB COM shows
# only the ROM boot log, AT dead, while WiFi/HTTP keep working.
#
# Usage:
#   .\build-c3.ps1 [-Port COM3] [-Variant full|remoter|base|rathole]
#   (omit -Port to compile only)

param(
    [string]$Port = "",
    [ValidateSet("full", "remoter", "base", "rathole")]
    [string]$Variant = "full"
)

& $PSScriptRoot/build.ps1 -Board c3 -Port $Port -Variant $Variant
