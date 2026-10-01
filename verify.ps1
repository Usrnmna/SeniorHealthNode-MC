# Build both firmware images and run every native suite from this project root.
param(
    [string]$Python = (Join-Path $PSScriptRoot '.venv\Scripts\python.exe'),
    [string]$CompilerBin = (Join-Path $PSScriptRoot '.pio\tools\mingw64\bin')
)

$ErrorActionPreference = 'Stop'
$savedPath = $env:PATH
$savedCore = $env:PLATFORMIO_CORE_DIR
Push-Location $PSScriptRoot
try {
    if (-not (Test-Path -LiteralPath $Python)) {
        throw 'Create .venv and install PlatformIO as described in README.md, or pass -Python.'
    }
    if (Test-Path -LiteralPath $CompilerBin) {
        $env:PATH = "$CompilerBin;$env:PATH"
    }
    if (-not (Get-Command g++ -ErrorAction SilentlyContinue)) {
        throw 'A C++17-capable GCC compiler is required. Pass its bin directory with -CompilerBin.'
    }
    $env:PLATFORMIO_CORE_DIR = Join-Path $PSScriptRoot '.pio\core'
    & $Python -m platformio run -e heltec_v4_companion_radio_ble -e heltec_v4_sensor -t mergebin
    if ($LASTEXITCODE -ne 0) { throw 'Firmware build or binary merge failed.' }
    & $Python -m platformio test -e native
    if ($LASTEXITCODE -ne 0) { throw 'Native tests failed.' }
    & $Python -m platformio test -e native_delivery
    if ($LASTEXITCODE -ne 0) { throw 'Production message-delivery integration tests failed.' }
    Write-Host 'Both firmware builds, merged images, and all native suites passed.'
}
finally {
    $env:PATH = $savedPath
    $env:PLATFORMIO_CORE_DIR = $savedCore
    Pop-Location
}
