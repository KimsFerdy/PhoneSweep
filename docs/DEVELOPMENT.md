# Developing PhoneSweep

PhoneSweep is a C++20 Windows app: Dear ImGui (docking) on DirectX 11, talking to the phone through `adb.exe` as a child process. All non-UI logic lives in a static library (`PhoneSweepCore`) so it can be unit-tested without a window.

## Build

Requirements: Visual Studio 2022 with *Desktop development with C++* (ships MSVC, CMake, Ninja and vcpkg).

```powershell
.\build.ps1                    # configure + build + test (debug)
.\build.ps1 -Config release    # optimised, static C++ runtime (runs without the VC++ redistributable)
.\package.ps1                  # release build -> dist\PhoneSweep-<version>-win64.zip
```

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
src/cleanup    Duplicate finder, junk sweep
src/compress   Photo/video compression (ffmpeg / stb)
src/nuke       Nuke plan (pure decision table) and executor
src/platform   IPlatform + Win32/DX11 implementation
src/report     Exports for any AI assistant (app list, scan report)
src/restore    Push a backup back to a phone
src/scan       File scan, categories, app scan
src/screens    All pages and dialogs
src/thumbs     Thumbnails (EXIF fast path, ffmpeg frames), disk cache
src/transfer   Verified pulls/pushes, manifests, save queue
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

## Notes on Android quirks

- Toybox `find` has no `-printf` on older phones; the scanner falls back to `stat`. `/sdcard` is a symlink, so scans start at `/storage/emulated/0/`.
- Sizes are measured as space on disk: Samsung's sparse `.thumbdata` files report gigabytes but occupy kilobytes.
- `touch -d @epoch` isn't supported; dates are restored with `TZ=UTC touch -m -t "$(TZ=UTC date -d @N ...)"`.
- `top -n 1` measures a tiny interval; live CPU uses the second frame of `top -n 2 -d 1`.
- On Android 9 an uninstall also deletes the app's `Android/data` and `Android/media` folders.
