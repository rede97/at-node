# build.ps1 - shared build/upload engine for the esp32/arduino sketch (Windows)
#
# Agents: do NOT call this directly. Use the board-specific wrappers:
#   .\build-c3.ps1    -Port COMx [-Variant ...]   (ESP32-C3 SuperMini)
#   .\build-esp32.ps1 -Port COMx [-Variant ...]   (standard ESP32)
#
# Prerequisites:
#   - arduino-cli on PATH
#   - esp32:esp32 core >= 3.3.5 installed
#   - NimBLE-Arduino library (usually bundled with core 3.x)
#
# Variants (feature macros in features.h):
#   full    - everything on (default)
#   remoter - FEATURE_RATHOLE=0 (Remoter: IR + BLE HID + MQTT + HTTP + I2C,
#             no rathole tunnel -> saves heap for BLE/MQTT)
#   base    - FEATURE_RATHOLE=0 FEATURE_HTTP=0 (production keyboard: BLE+MQTT+I2C,
#             no tunnel, no LAN HTTP control plane -> serial-only config)
#   rathole - FEATURE_BLE=0 FEATURE_MQTT=0 FEATURE_I2C=0 (Rathole: tunnel test unit;
#             I2C off also enables the GPIO8 breathing liveness LED)
#
# Board notes:
#   c3    - ESP32-C3 SuperMini. fqbn MUST carry CDCOnBoot=cdc; any other flash
#           path routes Serial to UART0 pads (native-USB COM shows only the ROM
#           boot log, AT dead, WiFi/HTTP fine). Symptom -> reflash, do not debug.
#   esp32 - standard ESP32 (no native USB; Serial = UART0 via onboard USB-UART
#           bridge, CDCOnBoot not applicable).

param(
    [Parameter(Mandatory = $true)]
    [ValidateSet("c3", "esp32")]
    [string]$Board,
    [string]$Port = "",
    [ValidateSet("full", "remoter", "base", "rathole")]
    [string]$Variant = "full"
)

$ErrorActionPreference = "Stop"
$fqbn = switch ($Board) {
    "c3"    { "esp32:esp32:esp32c3:CDCOnBoot=cdc,PartitionScheme=huge_app" }
    "esp32" { "esp32:esp32:esp32:PartitionScheme=huge_app" }
}

$defs = switch ($Variant) {
    "full"    { "" }
    "remoter" { "-DFEATURE_RATHOLE=0" }
    "base"    { "-DFEATURE_RATHOLE=0 -DFEATURE_HTTP=0" }
    "rathole" { "-DFEATURE_BLE=0 -DFEATURE_MQTT=0 -DFEATURE_I2C=0" }
}

Write-Host "Compiling esp32/arduino [$Board/$Variant] ..." -ForegroundColor Cyan
if ($defs -ne "") {
    arduino-cli compile --fqbn $fqbn --build-property "compiler.cpp.extra_flags=$defs" .
} else {
    arduino-cli compile --fqbn $fqbn .
}

if ($Port -ne "") {
    Write-Host "Uploading to $Port ..." -ForegroundColor Cyan
    arduino-cli upload --fqbn $fqbn -p $Port .
} else {
    Write-Host "No -Port given; skipping upload." -ForegroundColor Yellow
}
