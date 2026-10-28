<#
.SYNOPSIS
  Build in Docker, then flash the ESP32-C6-EVB from the Windows host.

.DESCRIPTION
  The build container has no USB access, so flashing happens on the host — but
  only with esptool, never a full toolchain, so the "all building happens in
  Docker" rule still holds. Uses the esptool that already ships inside
  ~/.platformio/packages/tool-esptoolpy.

  For hvac_controller it writes bootloader, partition table and app at their
  own offsets and erases otadata (so the board boots app0), leaving NVS alone:
  settings, learned data, safety latches and WiFi/MQTT credentials survive.
  This is the USB recovery path when a WiFi update goes wrong.

  -Factory instead writes firmware.factory.bin at 0x0. The merged image fills
  every gap with 0xFF, so that ERASES NVS. The standalone sketches (test_relays,
  blank, ...) always flash this way; they use the board's default table.

.EXAMPLE
  scripts\flash.ps1                              # build + flash main firmware
.EXAMPLE
  scripts\flash.ps1 -Env test_relays             # relay hardware test
.EXAMPLE
  scripts\flash.ps1 -Env blank -NoBuild          # flash an already-built sketch
.EXAMPLE
  scripts\flash.ps1 -Factory                     # full image: wipes settings too
#>
[CmdletBinding()]
param(
    [string]$Env = 'hvac_controller',
    [string]$Port,
    [int]$Baud = 921600,
    [switch]$NoBuild,
    [switch]$Factory
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot

if (-not $NoBuild) {
    & (Join-Path $PSScriptRoot 'build.ps1') -Env $Env
}

$build = Join-Path $repo ".pio\build\$Env"
$image = Join-Path $build 'firmware.factory.bin'
if (-not (Test-Path $image)) {
    throw "No firmware image at $image. Build env '$Env' first (drop -NoBuild)."
}
# Offsets from partitions_4mb.csv (main firmware only).
$parts = ($Env -eq 'hvac_controller') -and -not $Factory

# The EVB enumerates as a native USB-Serial-JTAG device: VID 303A / PID 1001.
if (-not $Port) {
    $dev = Get-CimInstance Win32_PnPEntity |
        Where-Object { $_.DeviceID -like '*VID_303A*' -and $_.Name -match '\(COM(\d+)\)' } |
        Select-Object -First 1
    if (-not $dev) { throw "No Espressif USB serial device found. Pass -Port COM# explicitly." }
    $Port = ([regex]::Match($dev.Name, '\((COM\d+)\)')).Groups[1].Value
    Write-Host "Auto-detected board on $Port" -ForegroundColor Cyan
}

$esptool = Join-Path $HOME '.platformio\packages\tool-esptoolpy\esptool.py'
if (-not (Test-Path $esptool)) {
    throw "esptool not found at $esptool. Install PlatformIO on the host, or flash from the devcontainer with usbipd."
}

$env:PYTHONIOENCODING = 'utf-8'
if ($parts) {
    Write-Host "Flashing $Env -> $Port (NVS preserved)" -ForegroundColor Cyan
    python $esptool --chip esp32c6 --port $Port --baud $Baud --after no_reset `
        erase_region 0x11000 0x2000
    if ($LASTEXITCODE -ne 0) { throw "esptool erase of otadata failed (exit $LASTEXITCODE)" }
    python $esptool --chip esp32c6 --port $Port --baud $Baud --after hard_reset `
        write_flash -z `
        0x0 (Join-Path $build 'bootloader.bin') `
        0x8000 (Join-Path $build 'partitions.bin') `
        0x20000 (Join-Path $build 'firmware.bin')
} else {
    Write-Host "Flashing $Env -> $Port (full image at 0x0)" -ForegroundColor Cyan
    python $esptool --chip esp32c6 --port $Port --baud $Baud --after hard_reset `
        write_flash -z 0x0 $image
}
if ($LASTEXITCODE -ne 0) { throw "esptool failed (exit $LASTEXITCODE)" }

Write-Host "Done. Monitor with: scripts\monitor.ps1 -Port $Port" -ForegroundColor Green
