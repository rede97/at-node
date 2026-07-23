# build.ps1 - Build and upload esp32_at_node sketch (Windows)
#
# Prerequisites:
#   - arduino-cli on PATH
#   - esp32:esp32 core >= 3.3.5 installed
#   - NimBLE-Arduino library (usually bundled with core 3.x)
#
# Usage:
#   .\build.ps1 [-Port COM3]
#   (omit -Port to compile only)

param(
    [string]$Port = ""
)

$ErrorActionPreference = "Stop"
$fqbn = "esp32:esp32:esp32c3:CDCOnBoot=cdc"

Write-Host "Compiling esp32_at_node ..." -ForegroundColor Cyan
arduino-cli compile --fqbn $fqbn .

if ($Port -ne "") {
    Write-Host "Uploading to $Port ..." -ForegroundColor Cyan
    arduino-cli upload --fqbn $fqbn -p $Port .
} else {
    Write-Host "No -Port given; skipping upload." -ForegroundColor Yellow
}
