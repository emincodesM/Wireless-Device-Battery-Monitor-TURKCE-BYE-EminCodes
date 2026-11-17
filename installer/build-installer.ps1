# Builds dist\WirelessBattery-Setup-<version>.exe and dist\WirelessBattery-<version>-portable.zip.
param([string]$Version = "1.0.0")
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
Set-Location $root

if (-not (Test-Path (Join-Path $root "WirelessBattery.exe"))) { throw "Build WirelessBattery.exe first (make or build.bat)." }

New-Item -ItemType Directory -Force (Join-Path $root "dist") | Out-Null
New-Item -ItemType Directory -Force (Join-Path $root "build") | Out-Null

$iscc = foreach ($v in 7, 6) {
    foreach ($base in ${env:ProgramFiles(x86)}, $env:ProgramFiles, "$env:LOCALAPPDATA\Programs") { "$base\Inno Setup $v\ISCC.exe" }
}
$iscc = $iscc | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $iscc) { $iscc = (Get-Command ISCC.exe -ErrorAction SilentlyContinue).Source }

if ($iscc) {
    & $iscc "/DAppVersion=$Version" (Join-Path $PSScriptRoot "WirelessBattery.iss")
    if ($LASTEXITCODE -ne 0) { throw "ISCC failed." }
} else {
    Write-Host "Building standalone native Win32 installer (WirelessBattery-Setup-$Version.exe) with MinGW-w64..."
    & windres -i installer\setup.rc -o build\setup_res.o --include-dir installer
    if ($LASTEXITCODE -ne 0) { throw "windres failed for setup.rc" }
    & g++ -std=c++17 -Os -municode -mwindows -static -s -ffunction-sections -fdata-sections "-Wl,--gc-sections" `
        installer\setup.cpp build\setup_res.o -o "dist\WirelessBattery-Setup-$Version.exe" `
        -lole32 -lshell32 -luuid
    if ($LASTEXITCODE -ne 0) { throw "g++ failed for setup.cpp" }
}

# Portable zip: same files, no installer.
$stage = Join-Path $root "build\portable"
Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $stage | Out-Null
Copy-Item WirelessBattery.exe $stage
Copy-Item LICENSE (Join-Path $stage "LICENSE.txt")
Compress-Archive -Path "$stage\*" -DestinationPath "dist\WirelessBattery-$Version-portable.zip" -Force
Write-Host "Done: dist\WirelessBattery-Setup-$Version.exe and dist\WirelessBattery-$Version-portable.zip"
