# Builds dist\WirelessBattery-Setup-<version>.exe (Native Win32 Turkish GUI Installer)
# and dist\WirelessBattery-<version>-portable.zip.
param([string]$Version = "")
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
Set-Location $root

if ([string]::IsNullOrWhiteSpace($Version)) {
    if (Test-Path (Join-Path $root "VERSION")) {
        $Version = (Get-Content (Join-Path $root "VERSION") -Raw).Trim()
    } else {
        $Version = "1.0.8"
    }
}

if (-not (Test-Path (Join-Path $root "WirelessBattery.exe"))) {
    throw "Build WirelessBattery.exe first (make or build.bat)."
}

New-Item -ItemType Directory -Force (Join-Path $root "build") | Out-Null
New-Item -ItemType Directory -Force (Join-Path $root "dist") | Out-Null

Write-Host "Compiling setup resources..."
& windres -i installer/setup.rc -o build/setup_res.o
if ($LASTEXITCODE -ne 0) { throw "windres failed for setup.rc" }

$setupExe = Join-Path $root "dist\WirelessBattery-Setup-$Version.exe"
Write-Host "Building native installer: $setupExe ..."
& g++ -std=c++17 -O2 -Wall -Wextra -Wno-missing-field-initializers -municode -DUNICODE -D_UNICODE `
    -finput-charset=UTF-8 -fexec-charset=UTF-8 `
    installer/setup.cpp build/setup_res.o `
    -o $setupExe `
    -mwindows -municode -s -static-libgcc -static-libstdc++ `
    -lole32 -luuid -lshell32 -lcomctl32
if ($LASTEXITCODE -ne 0) { throw "g++ failed for setup.cpp" }

# Portable zip: same files, no installer.
$stage = Join-Path $root "build\portable"
Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $stage | Out-Null
Copy-Item WirelessBattery.exe, installer\THIRD_PARTY_NOTICES.txt $stage
$hscExe = Join-Path $root "third_party\headsetcontrol\headsetcontrol.exe"
if (Test-Path $hscExe) {
    Copy-Item $hscExe $stage
}
Copy-Item LICENSE (Join-Path $stage "LICENSE.txt")
$zipOut = Join-Path $root "dist\WirelessBattery-$Version-portable.zip"
Compress-Archive -Path "$stage\*" -DestinationPath $zipOut -Force

Write-Host "Done: $setupExe and $zipOut"
