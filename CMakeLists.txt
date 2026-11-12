cmake_minimum_required(VERSION 3.25)

project(PhoneSweep VERSION 1.1.1 LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# Release builds link the C++ runtime statically: one exe that runs on a clean Windows
# without the VC++ Redistributable. Debug keeps the DLL runtime (faster links).
option(PS_STATIC_CRT "Link the MSVC runtime statically" OFF)
if(PS_STATIC_CRT)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()

if(NOT WIN32)
    message(FATAL_ERROR "PhoneSweep targets Windows 10/11 only.")
endif()

find_package(imgui CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(doctest CONFIG REQUIRED)
find_path(STB_INCLUDE_DIRS "stb_image.h" REQUIRED)

# Common warnings / encoding for all our targets.
add_library(ps_options INTERFACE)
target_compile_options(ps_options INTERFACE /utf-8 /W4 /permissive- /Zc:__cplusplus /MP)
target_compile_definitions(ps_options INTERFACE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)

# ---- Core: everything testable without a window ------------------------------
add_library(PhoneSweepCore STATIC
    src/util/Log.cpp
    src/util/Utf8.cpp
    src/adb/ShellCommand.cpp
    src/adb/AdbClient.cpp
    src/adb/Device.cpp
    src/adb/DeviceMonitor.cpp
    src/app/Settings.cpp
    src/util/JobSystem.cpp
    src/scan/Categorize.cpp
    src/scan/ScanParse.cpp
    src/scan/FileScanner.cpp
    src/scan/FileStore.cpp
    src/scan/ScanSession.cpp
    src/scan/FakeFiles.cpp
    src/scan/FakeApps.cpp
    src/util/Process.cpp
    src/thumbs/Exif.cpp
    src/thumbs/ImageOps.cpp
    src/thumbs/ThumbFetch.cpp
    src/thumbs/StbImpl.cpp
    src/scan/AppParse.cpp
    src/scan/AppScanner.cpp
    src/scan/AppSession.cpp
    src/report/AppListFormat.cpp
    src/bg/Background.cpp
    src/util/Md5.cpp
    src/transfer/LocalPath.cpp
    src/transfer/Manifest.cpp
    src/transfer/Puller.cpp
    src/nuke/NukePlan.cpp
    src/nuke/Nuker.cpp
    src/nuke/NukeSelfTest.cpp
    src/report/Exporter.cpp
    src/cleanup/Cleanup.cpp
    src/transfer/PhoneIO.cpp
    src/compress/Compress.cpp
    src/restore/Restore.cpp
    src/compress/M8SelfTest.cpp
    src/adb/Wireless.cpp
    src/backup/Zip.cpp
    src/backup/ZipBackup.cpp
    src/backup/M9SelfTest.cpp
    src/bg/Usage.cpp
    src/apps/AppActions.cpp
    src/apps/M10SelfTest.cpp
    src/cleanup/Preview.cpp
    src/update/Update.cpp
    src/update/Http.cpp
)
target_include_directories(PhoneSweepCore PUBLIC src ${STB_INCLUDE_DIRS})
target_link_libraries(PhoneSweepCore PUBLIC ps_options nlohmann_json::nlohmann_json winhttp bcrypt msi shell32 ole32)

# ---- App ---------------------------------------------------------------------
add_executable(PhoneSweep WIN32
    src/main.cpp
    src/app/App.cpp
    src/platform/Win32Dx11Platform.cpp
    src/ui/Theme.cpp
    src/ui/Widgets.cpp
    src/screens/HomeScreen.cpp
    src/screens/WorkspaceScreen.cpp
    src/screens/LogPanel.cpp
    src/screens/FilesPage.cpp
    src/screens/OverviewPage.cpp
    src/screens/AppsPage.cpp
    src/screens/BackgroundPage.cpp
    src/screens/SaveQueuePage.cpp
    src/screens/NukePage.cpp
    src/screens/ReportsPage.cpp
    src/screens/CleanupPage.cpp
    src/screens/CompressPanel.cpp
    src/screens/BackupPage.cpp
    src/screens/WirelessDialog.cpp
    src/screens/DupCompare.cpp
    src/screens/PreviewKit.cpp
    src/screens/FilePreview.cpp
    src/screens/FileDeleter.cpp
    src/platform/AudioPlayer.cpp
    src/thumbs/ThumbCache.cpp
    res/PhoneSweep.manifest
    res/PhoneSweep.rc
)
target_link_libraries(PhoneSweep PRIVATE PhoneSweepCore imgui::imgui d3d11 dxgi dwmapi comdlg32 shell32 ole32 mfplay propsys)

# Ship fonts next to the exe.
add_custom_command(TARGET PhoneSweep POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/assets/fonts $<TARGET_FILE_DIR:PhoneSweep>/fonts
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/assets/icons $<TARGET_FILE_DIR:PhoneSweep>/icons
    COMMENT "Copying fonts and icons")

# ---- Tests -------------------------------------------------------------------
enable_testing()
add_executable(PhoneSweepTests
    tests/test_main.cpp
    tests/test_shell_quote.cpp
    tests/test_device_parse.cpp
    tests/test_categorize.cpp
    tests/test_scan_parse.cpp
    tests/test_thumbs.cpp
    tests/test_apps.cpp
    tests/test_background.cpp
    tests/test_transfer.cpp
    tests/test_nuke.cpp
    tests/test_reports.cpp
    tests/test_cleanup.cpp
    tests/test_compress_restore.cpp
    tests/test_m9.cpp
    tests/test_m10.cpp
    tests/test_preview.cpp
    tests/test_update.cpp
)
target_link_libraries(PhoneSweepTests PRIVATE PhoneSweepCore doctest::doctest)
add_test(NAME PhoneSweepTests COMMAND PhoneSweepTests)
