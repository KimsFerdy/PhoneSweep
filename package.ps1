# Builds a release and packs it into dist\PhoneSweep-<version>-win64.zip:
#   PhoneSweep.exe, fonts\, icons\, platform-tools\ (adb + its NOTICE), README.txt, licences.
#   .\package.ps1                 # release build + package
#   .\package.ps1 -NoBuild        # package the existing release build
#   .\package.ps1 -AdbDir <dir>   # platform-tools folder to bundle (default: Android SDK)
param(
    [switch]$NoBuild,
    [string]$AdbDir = "$env:LOCALAPPDATA\Android\Sdk\platform-tools"
)
$ErrorActionPreference = 'Stop'
Push-Location $PSScriptRoot
try {
    $version = (Select-String -Path CMakeLists.txt -Pattern 'project\(PhoneSweep VERSION ([0-9.]+)').Matches[0].Groups[1].Value
    if (-not $NoBuild) {
        .\build.ps1 -Config release
    }
    $bin = 'build\windows-msvc-release'
    if (-not (Test-Path "$bin\PhoneSweep.exe")) { throw "No release build at $bin" }
    foreach ($f in 'adb.exe', 'AdbWinApi.dll', 'AdbWinUsbApi.dll', 'NOTICE.txt') {
        if (-not (Test-Path (Join-Path $AdbDir $f))) { throw "platform-tools incomplete: $f missing in $AdbDir" }
    }

    $name = "PhoneSweep-$version-win64"
    $stage = "dist\$name"
    if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
    New-Item -ItemType Directory -Force "$stage\platform-tools" | Out-Null

    Copy-Item "$bin\PhoneSweep.exe" $stage
    Copy-Item -Recurse "$bin\fonts" "$stage\fonts"
    Copy-Item -Recurse "$bin\icons" "$stage\icons"
    foreach ($f in 'adb.exe', 'AdbWinApi.dll', 'AdbWinUsbApi.dll', 'NOTICE.txt') {
        Copy-Item (Join-Path $AdbDir $f) "$stage\platform-tools\"
    }
    if (Test-Path LICENSE) { Copy-Item LICENSE "$stage\LICENSE.txt" }

    @"
PhoneSweep $version
===================

Scan, save and clean an Android phone from your PC.

1. On the phone: Settings > About phone > tap "Build number" 7 times,
   then Settings > Developer options > turn on "USB debugging".
2. Plug the phone in and run PhoneSweep.exe.
3. Tap "Allow" on the phone when it asks about this computer.

Nothing to install: everything PhoneSweep needs is in this folder
(adb from Google's Android platform-tools is in platform-tools\).

Optional extras
- Video thumbnails and video compression: install ffmpeg (winget install ffmpeg).
- Real app names: install Android SDK build-tools (aapt2).

Settings and logs: %LOCALAPPDATA%\PhoneSweep\
Backups (default):  Documents\PhoneSweep\

Project, help and updates: https://github.com/KimsFerdy/PhoneSweep
Made by Kims Ferdy and his humble friend Claude.

Third-party: Inter and JetBrains Mono fonts (SIL OFL 1.1), Font Awesome Free
(SIL OFL 1.1 / CC BY 4.0), see fonts\. adb: Android Open Source Project
(Apache 2.0), see platform-tools\NOTICE.txt. Dear ImGui (MIT), stb (public
domain/MIT), nlohmann/json (MIT), FreeType (FTL).
"@ | Set-Content -Encoding utf8 "$stage\README.txt"

    $zip = "dist\$name.zip"
    if (Test-Path $zip) { Remove-Item -Force $zip }
    Compress-Archive -Path $stage -DestinationPath $zip
    $size = [math]::Round((Get-Item $zip).Length / 1MB, 1)
    "Packaged $zip ($size MB)"
} finally {
    Pop-Location
}
