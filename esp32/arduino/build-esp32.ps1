# build-esp32.ps1 - Build and upload for standard ESP32 (Windows)
#
# Agent default entry point for the classic ESP32 board. Thin wrapper over
# build.ps1 with the standard ESP32 fqbn pinned (PartitionScheme=huge_app).
# Classic ESP32 has no native USB: Serial is UART0 through the onboard
# USB-UART bridge, so no CDCOnBoot option is needed (or available).
#
# Usage:
#   .\build-esp32.ps1 [-Port COM5] [-Variant full|remoter|base|rathole]
#   (omit -Port to compile only)

param(
    [string]$Port = "",
    [ValidateSet("full", "remoter", "base", "rathole")]
    [string]$Variant = "full"
)

& $PSScriptRoot/build.ps1 -Board esp32 -Port $Port -Variant $Variant
