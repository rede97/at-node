# build.ps1 - Build and upload esp32c3_kbd sketch (Windows)
#
# Prerequisites:
#   - arduino-cli on PATH
#   - esp32:esp32 core >= 3.3.5 installed
#   - ESP32-BLE-Keyboard library installed:
#       arduino-cli lib install ESP32-BLE-Keyboard
#
# Usage:
#   .\build.ps1 [-Port COM4]
#   (omit -Port to compile only)

param(
    [string]$Port = ""
)

$ErrorActionPreference = "Stop"
$fqbn = "esp32:esp32:esp32c3:CDCOnBoot=cdc"

Write-Host "Compiling esp32c3_kbd ..." -ForegroundColor Cyan
arduino-cli compile --fqbn $fqbn .

if ($Port -ne "") {
    Write-Host "Uploading to $Port ..." -ForegroundColor Cyan
    arduino-cli upload --fqbn $fqbn -p $Port .
} else {
    Write-Host "No -Port given; skipping upload." -ForegroundColor Yellow
}
