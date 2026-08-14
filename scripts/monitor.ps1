<#
.SYNOPSIS
  Serial monitor that will not knock the C6 into the ROM bootloader.

.DESCRIPTION
  On the ESP32-C6's native USB-Serial-JTAG, DTR and RTS are wired to RESET and
  GPIO9/boot. A monitor that asserts them drops the RUNNING firmware into the
  ROM download bootloader — the app stops dead, and while the EVB's GPIO relays
  release on that reset, any MOD-IO2 relay stays LATCHED CLOSED on the
  expander's own 12V rail until firmware runs again. Symptom: an "ESP-ROM:" /
  "Build:Sep 19 2022" banner and then silence.

  So this holds RTS deasserted throughout. Output still flows because
  HWCDC::isCDC_Connected() detects the host from USB SOF activity, not DTR.

  If the board IS already stuck in the bootloader, reset it out with:
    scripts\flash.ps1 -Env <env>        (or any esptool run with --after hard_reset)

.EXAMPLE
  scripts\monitor.ps1                    # follow until Ctrl-C
.EXAMPLE
  scripts\monitor.ps1 -Seconds 20        # bounded capture, useful for scripting
.EXAMPLE
  scripts\monitor.ps1 -Match 'cycle|FAIL'
#>
[CmdletBinding()]
param(
    [string]$Port,
    [int]$Baud = 115200,
    [int]$Seconds = 0,          # 0 = run until Ctrl-C
    [string]$Match,             # optional regex filter
    [switch]$Timestamp
)

$ErrorActionPreference = 'Stop'

if (-not $Port) {
    $dev = Get-CimInstance Win32_PnPEntity |
        Where-Object { $_.DeviceID -like '*VID_303A*' -and $_.Name -match '\(COM(\d+)\)' } |
        Select-Object -First 1
    if (-not $dev) { throw "No Espressif USB serial device found. Pass -Port COM# explicitly." }
    $Port = ([regex]::Match($dev.Name, '\((COM\d+)\)')).Groups[1].Value
}

$sp = New-Object System.IO.Ports.SerialPort $Port, $Baud, 'None', 8, 'One'
$sp.ReadTimeout = 1000
$sp.DtrEnable = $true    # harmless: CDC connection is detected from USB SOF
$sp.RtsEnable = $false   # NEVER set true - this is the reset line

try {
    $sp.Open()
} catch {
    throw "Could not open ${Port}: $_"
}

Write-Host "Monitoring $Port at $Baud (RTS deasserted). Ctrl-C to stop." -ForegroundColor Cyan
$sw = [System.Diagnostics.Stopwatch]::StartNew()
try {
    while ($Seconds -le 0 -or $sw.Elapsed.TotalSeconds -lt $Seconds) {
        try {
            $line = $sp.ReadLine().TrimEnd()
        } catch [TimeoutException] {
            continue
        }
        if ($Match -and $line -notmatch $Match) { continue }
        if ($Timestamp) { "{0,5:N0}s | {1}" -f $sw.Elapsed.TotalSeconds, $line }
        else { $line }
    }
} finally {
    $sp.Close()
}
