# Developing PhoneSweep

PhoneSweep is a C++20 Windows app: Dear ImGui (docking) on DirectX 11, talking to the phone through `adb.exe` as a child process. All non-UI logic lives in a static library (`PhoneSweepCore`) so it can be unit-tested without a window.

## Build

Requirements: Visual Studio 2022 with *Desktop development with C++* (ships MSVC, CMake, Ninja and vcpkg).

```powershell
.\build.ps1                    # configure + build + test (debug)
.\build.ps1 -Config release    # optimised, static C++ runtime (runs without the VC++ redistributable)
.\package.ps1                  # release build -> dist\ zip + setup.exe + msi
.\package.ps1 -NoBuild         # package the existing release build
```

`package.ps1` stages the app with `adb` (from the Android SDK's platform-tools), zips it, and builds the installers when the tools are installed:

- **setup.exe**: [Inno Setup 6](https://jrsoftware.org/isinfo.php) (`winget install JRSoftware.InnoSetup`), script `installer\PhoneSweep.iss`. Per-user by default, all users optional.
- **msi**: [WiX Toolset 5](https://wixtoolset.org) (`dotnet tool install --global wix --version 5.0.2`, then `wix extension add --global WixToolset.UI.wixext/5.0.2`), source `installer\PhoneSweep.wxs`. Per-machine, validated with `wix msi validate`.

When releasing a new version, bump it in `CMakeLists.txt`, `src/app/Version.h`, `res/PhoneSweep.rc` and `res/PhoneSweep.manifest`. Keep the `UpgradeCode` in the `.wxs` and the `AppId` in the `.iss` unchanged so installers upgrade in place.

### In-app updates (v1.1)

Settings → Updates asks `api.github.com/repos/KimsFerdy/PhoneSweep/releases/latest` (also once a day at startup unless turned off). It only accepts assets under `github.com/KimsFerdy/PhoneSweep/releases/download/`, picks the one that matches how this copy was installed, and checks the download against GitHub's SHA-256 `digest` before running it:

| Installed with | Detected by | Downloads | Runs |
|---|---|---|---|
| setup.exe, this user | `unins000.exe` next to the exe | `*-setup.exe` | `/SILENT /CURRENTUSER /RELAUNCH=1` |
| setup.exe, all users | the same, under Program Files | `*-setup.exe` | `/SILENT /ALLUSERS /RELAUNCH=1` (UAC prompt) |
| msi | our MSI UpgradeCode is registered, exe under Program Files | `*.msi` | `msiexec /i … /passive PS_RELAUNCH=1` (UAC prompt) |
| zip | none of the above | nothing | opens the release page |

So every release needs a `v<major>.<minor>.<patch>` tag and the three files `package.ps1` makes: `PhoneSweep-<v>-setup.exe`, `PhoneSweep-<v>-x64.msi`, `PhoneSweep-<v>-win64.zip`. `/RELAUNCH=1` (Inno `[Code]`) and `PS_RELAUNCH=1` (a WiX custom action) start PhoneSweep again after the silent install. The release notes (Markdown) are shown in the app, so write them for users.

`build.ps1` loads the MSVC environment itself and uses VS's bundled vcpkg unless `VCPKG_ROOT` is set. Dependencies come from `vcpkg.json` (manifest mode): imgui (docking, dx11, win32, freetype), stb, nlohmann-json, doctest.

Outputs: `build\windows-msvc-debug\PhoneSweep.exe`, `build\windows-msvc-release\PhoneSweep.exe`, and `PhoneSweepTests.exe` next to each.

From a Developer PowerShell the presets work directly:

```powershell
cmake --preset windows-msvc-debug
cmake --build --preset debug
ctest --preset debug
```

## Source layout

```
src/adb        adb process wrapper (quoting, timeouts, cancel), device parsers, device monitor, Wi-Fi (tcpip/connect/pair)
src/app        App state, settings, version + links
src/apps       App actions: Google Play link, Ask AI text, quick/full uninstall
src/backup     ZIP64 writer/reader, verified zip backups
src/bg         Background restrictions, live CPU/RAM (top), battery (batterystats)
src/cleanup    Duplicate finder, junk sweep, compare-window previews (text decoding)
src/compress   Photo/video compression (ffmpeg / stb)
src/nuke       Nuke plan (pure decision table) and executor
src/platform   IPlatform + Win32/DX11 implementation
src/report     Exports for any AI assistant (app list, scan report)
src/restore    Push a backup back to a phone
src/scan       File scan, categories, app scan
src/screens    All pages and dialogs
src/thumbs     Thumbnails (EXIF fast path, ffmpeg frames), disk cache
src/transfer   Verified pulls/pushes, manifests, save queue
src/update     GitHub release check, WinHTTP download, SHA-256, silent installer launch
src/ui         Theme, widgets
tests/         doctest unit tests (real adb output captured as fixtures)
assets/        Fonts (OFL) and the app logo
res/           Icon, manifest, version resource
```

## Safety rules the code enforces

- Every copy to the PC is checked (size, and MD5 when enabled) and recorded in `saved_manifest.json`; "saved" always means *verified and unchanged since*.
- **Nuke prompt** (default) never deletes a file that isn't saved, never deletes KEEP files, and blocks uninstalls while an app's folders hold unsaved files. **Nuke free** deletes what you select.
- Uninstalls back up the APK first; no backup, no uninstall.
- Pushes to the phone go to a hidden temp name, are MD5-checked on the phone, and only then renamed into place.
- Zips are read back entry by entry (CRC + manifest MD5) before a folder may be removed.

## Self-tests on a real phone

These touch only throwaway files they create, and clean up afterwards. Results go to the log (`%LOCALAPPDATA%\PhoneSweep\logs`).

| Command | What it checks |
|---|---|
| `PhoneSweep.exe --nuke-selftest` | The real delete path on throwaway files in `PhoneSweepSelfTest/` (internal + SD card) |
| `PhoneSweep.exe --m8-selftest` | Compress (all three outcomes) and restore: sizes, dates, EXIF, MD5s |
| `PhoneSweep.exe --m9-selftest[=<backup folder>]` | USB → Wi-Fi switch, verified push/pull over Wi-Fi, back to USB; zips a *copy* of the backup folder |
| `PhoneSweep.exe --m10-selftest[=<test.apk>]` | Live CPU/RAM + battery parsing, then quick and full uninstall of a throwaway APK |
| `PhoneSweep.exe --update-selftest` | No phone needed. Runs as 0.9.0, downloads the latest release, verifies it, installs it silently and exits. Use it on an *installed* copy, which ends up on the latest release |

## Dev flags

| Flag | Effect |
|---|---|
| `--fake=none\|unauthorized\|one\|two\|three\|noadb` | Fake device scenario (no phone needed) |
| `--screenshot=out.png` `--frames=N` `--size=WxH` | Render N frames, save a PNG, exit |
| `--scale=1.5` | Force UI scale |
| `--workspace[=files\|cleanup\|apps\|background\|queue\|nuke\|reports]` | Jump into the first device's workspace |
| `--log` | Start with the log panel open |
| `--backup-root=<dir>` | Use another backup folder for this run only |
| `--settings=N` | Open Settings on tab N |
| `--files=grid,images,camera,preview` | Files page presets |
| `--files=dups,junk,compress,restore,archives,wifi,running,battery,uninstall` | Page/tab presets (`restore` runs the read-only comparison, `uninstall` only opens the dialog) |
| `--files=dups:image\|video\|audio\|text\|any` | Find duplicates, then open the first group of that kind in the compare window |
| `--pretend-version=0.9.0` | Run as an older version, so the update check finds the real latest release |
| `--update-kind=inno\|inno-all\|msi\|portable` | Override the detected installation type |
| `--play-test=<file>` | Can Windows (Media Foundation) play this audio file? Exit code 0 = yes |

## Notes on Android quirks

- Toybox `find` has no `-printf` on older phones; the scanner falls back to `stat`. `/sdcard` is a symlink, so scans start at `/storage/emulated/0/`.
- Sizes are measured as space on disk: Samsung's sparse `.thumbdata` files report gigabytes but occupy kilobytes.
- `touch -d @epoch` isn't supported; dates are restored with `TZ=UTC touch -m -t "$(TZ=UTC date -d @N ...)"`.
- `top -n 1` measures a tiny interval; live CPU uses the second frame of `top -n 2 -d 1`.
- On Android 9 an uninstall also deletes the app's `Android/data` and `Android/media` folders.
