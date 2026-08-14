<#
.SYNOPSIS
  Build in Docker, then flash the ESP32-C6-EVB from the Windows host.

.DESCRIPTION
  The build container has no USB access, so flashing happens on the host — but
  only with esptool, never a full toolchain, so the "all building happens in
  Docker" rule still holds. Uses the esptool that already ships inside
  ~/.platformio/packages/tool-esptoolpy.

  Writes firmware.factory.bin (bootloader + partitions + boot_app0 + app) at
  offset 0x0, so a flash always lands the board in a fully consistent state.

.EXAMPLE
  scripts\flash.ps1                              # build + flash main firmware
.EXAMPLE
  scripts\flash.ps1 -Env test_relays             # relay hardware test
.EXAMPLE
  scripts\flash.ps1 -Env blank -NoBuild          # flash an already-built sketch
#>
[CmdletBinding()]
param(
    [string]$Env = 'hvac_controller',
    [string]$Port,
    [int]$Baud = 921600,
    [switch]$NoBuild
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot

if (-not $NoBuild) {
    & (Join-Path $PSScriptRoot 'build.ps1') -Env $Env
}

$image = Join-Path $repo ".pio\build\$Env\firmware.factory.bin"
if (-not (Test-Path $image)) {
    throw "No firmware image at $image. Build env '$Env' first (drop -NoBuild)."
}

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

Write-Host "Flashing $Env -> $Port" -ForegroundColor Cyan
$env:PYTHONIOENCODING = 'utf-8'
python $esptool --chip esp32c6 --port $Port --baud $Baud --after hard_reset `
    write_flash -z 0x0 $image
if ($LASTEXITCODE -ne 0) { throw "esptool failed (exit $LASTEXITCODE)" }

Write-Host "Done. Monitor with: scripts\monitor.ps1 -Port $Port" -ForegroundColor Green
