<#
.SYNOPSIS
  Run the native (host) tests in Docker from Windows. No board, no toolchain.

.DESCRIPTION
  Windows-safe equivalent of scripts/native-test.sh: compiles the firmware's
  control logic, the on-device unit suite and the scenario tests with g++
  (ASan + UBSan) inside the build image and runs them.

.EXAMPLE
  scripts\native-test.ps1                 # build + run
.EXAMPLE
  scripts\native-test.ps1 -Target clean   # remove test/native/build
#>
[CmdletBinding()]
param(
    [string]$Target = 'run',
    [string]$Image = 'hvac-controller-build'
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot

docker info --format '{{.ServerVersion}}' 2>$null | Out-Null
if ($LASTEXITCODE -ne 0) {
    throw "Docker engine is not responding. Start Docker Desktop and wait for it to finish starting, then re-run."
}

Write-Host "Building image $Image ..." -ForegroundColor Cyan
docker build -q -t $Image (Join-Path $repo '.devcontainer') | Out-Null
if ($LASTEXITCODE -ne 0) { throw "docker build failed" }

docker run --rm `
    -v "${repo}:/workspace" `
    -w /workspace/test/native `
    $Image `
    make $Target
if ($LASTEXITCODE -ne 0) { throw "native tests failed (exit $LASTEXITCODE)" }
