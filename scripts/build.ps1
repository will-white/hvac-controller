<#
.SYNOPSIS
  Build the firmware in Docker from Windows. No host toolchain required.

.DESCRIPTION
  Windows-safe equivalent of scripts/docker-build.sh. The shell script cannot be
  used from Git Bash on Windows: MSYS rewrites the container-side "-w /workspace"
  into a host path ("C:/Program Files/Git/workspace") and docker run fails.

  Shares the "platformio-cache" named volume with the devcontainer, so
  platforms/toolchains download once and are reused by both.

.EXAMPLE
  scripts\build.ps1                       # default env (hvac_controller)
.EXAMPLE
  scripts\build.ps1 -Env test_relays      # relay hardware test
.EXAMPLE
  scripts\build.ps1 -Env blank            # all-relays-off sketch
.EXAMPLE
  scripts\build.ps1 -Target clean         # clean build artifacts
#>
[CmdletBinding()]
param(
    [string[]]$Env = @('hvac_controller'),
    [string]$Target,
    [string]$Image = 'hvac-controller-build'
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot

# Docker Desktop takes ~75s from a cold start before the engine answers.
docker info --format '{{.ServerVersion}}' 2>$null | Out-Null
if ($LASTEXITCODE -ne 0) {
    throw "Docker engine is not responding. Start Docker Desktop and wait for it to finish starting, then re-run."
}

Write-Host "Building image $Image ..." -ForegroundColor Cyan
docker build -q -t $Image (Join-Path $repo '.devcontainer') | Out-Null
if ($LASTEXITCODE -ne 0) { throw "docker build failed" }

$pioArgs = @('run')
foreach ($e in $Env) { $pioArgs += @('-e', $e) }
if ($Target) { $pioArgs += @('-t', $Target) }

Write-Host "pio $($pioArgs -join ' ')" -ForegroundColor Cyan
docker run --rm `
    -v "${repo}:/workspace" `
    -w /workspace `
    -v platformio-cache:/home/vscode/.platformio `
    $Image `
    pio @pioArgs
if ($LASTEXITCODE -ne 0) { throw "pio run failed (exit $LASTEXITCODE)" }
