#include "backup/M9SelfTest.h"

#include "adb/AdbClient.h"
#include "adb/Device.h"
#include "adb/Wireless.h"
#include "backup/ZipBackup.h"
#include "restore/Restore.h"
#include "transfer/PhoneIO.h"
#include "util/Log.h"
#include "util/Md5.h"
#include "util/Strings.h"
#include "util/Utf8.h"

#include <format>
#include <fstream>
#include <thread>

namespace ps {

int RunM9SelfTest(const std::filesystem::path& backupCopySource)
{
    const auto exe = AdbClient::Locate();
    if (!exe) {
        LogError("m9 selftest: adb not found");
        return 2;
    }
    const AdbClient adb(*exe);
    int failures = 0;
    auto check = [&](bool ok, std::string_view what) {
        if (ok)
            LogInfo("m9 selftest: OK   {}", what);
        else {
            LogError("m9 selftest: FAIL {}", what);
            ++failures;
        }
    };
    std::stop_source ss;
    const auto st = ss.get_token();

    std::string usb;
    for (const auto& d : ParseDevicesList(adb.Run({ "devices", "-l" }).out))
        if (d.state == DeviceEntry::State::Device && d.serial.find(':') == std::string::npos)
            usb = d.serial;
    if (usb.empty()) {
        LogError("m9 selftest: no USB device");
        return 3;
    }
    const auto usbInfo = QueryDeviceInfo(adb, usb);
    check(usbInfo && !usbInfo->hwSerial.empty(), "hardware serial over USB");

    // ---- Wireless ------------------------------------------------------------------
    const auto w = SwitchToWifi(adb, usb, st);
    check(w.ok, "switch to Wi-Fi: " + w.message + " " + w.address);
    if (w.ok) {
        bool listed = false;
        for (int i = 0; i < 10 && !listed; ++i) {
            for (const auto& d : ParseDevicesList(adb.Run({ "devices", "-l" }).out))
                listed |= d.serial == w.address && d.state == DeviceEntry::State::Device;
            if (!listed)
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        check(listed, "phone listed as " + w.address);
        const auto wInfo = QueryDeviceInfo(adb, w.address);
        check(wInfo && wInfo->wireless && usbInfo && wInfo->hwSerial == usbInfo->hwSerial, "same hardware serial over Wi-Fi, marked wireless");

        // A real transfer over Wi-Fi: push (MD5-verified on the phone), pull back, compare.
        const auto tmp = std::filesystem::temp_directory_path() / L"ps_m9";
        std::error_code ec;
        std::filesystem::create_directories(tmp, ec);
        std::string data(3'000'000, '\0');
        for (size_t i = 0; i < data.size(); ++i)
            data[i] = static_cast<char>((i * 2654435761u) >> 24);
        std::ofstream(tmp / L"w.bin", std::ios::binary).write(data.data(), static_cast<std::streamsize>(data.size()));
        const std::string remote = "/storage/emulated/0/PhoneSweepSelfTest/m9/w.bin";
        const auto t0 = std::chrono::steady_clock::now();
        check(PushVerified(adb, w.address, tmp / L"w.bin", remote, Md5Hex(data), 1600000000, st).empty(), "push 3 MB over Wi-Fi (MD5-verified)");
        const auto pulled = PullToFile(adb, w.address, remote, tmp / L"back.bin", st);
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        LogInfo("m9 selftest: pull ok={} bytes={} md5={} want={} err={}", pulled.ok, pulled.bytes, pulled.md5, Md5Hex(data), pulled.error);
        check(pulled.ok && pulled.md5 == Md5Hex(data), std::format("pull back over Wi-Fi matches ({:.1f} s round trip)", secs));
        adb.Shell(w.address, ShellCommand("rm").arg("-rf").arg("/storage/emulated/0/PhoneSweepSelfTest"));
        std::filesystem::remove_all(tmp, ec);

        // Put the phone back the way it was: USB only.
        // "adb usb" restarts adbd in USB-only mode (must go over the Wi-Fi link while it exists).
        const auto back = adb.Run({ "-s", w.address, "usb" }, { .timeout = std::chrono::seconds(10) });
        LogInfo("m9 selftest: adb usb: {}", Trim(back.out + back.err));
        DisconnectWireless(adb, w.address);
        std::this_thread::sleep_for(std::chrono::seconds(3));
        bool usbBack = false;
        for (int i = 0; i < 10 && !usbBack; ++i) {
            for (const auto& d : ParseDevicesList(adb.Run({ "devices", "-l" }).out))
                usbBack |= d.serial == usb && d.state == DeviceEntry::State::Device;
            if (!usbBack)
                std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        check(usbBack, "phone back on USB");
    }

    // ---- Zip a copy of a real backup, then plan a restore from the zip --------------
    if (!backupCopySource.empty() && std::filesystem::exists(backupCopySource)) {
        const auto root = std::filesystem::temp_directory_path() / L"ps_m9_zip";
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
        std::filesystem::create_directories(root, ec);
        const auto folder = root / backupCopySource.filename();
        std::filesystem::copy(backupCopySource, folder, std::filesystem::copy_options::recursive, ec);
        check(!ec, "copy backup folder");
        ZipProgress p;
        const auto t0 = std::chrono::steady_clock::now();
        ZipBackupFolder(folder, true, p, st);
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        check(p.ok, std::format("zip + verify ({} files, {:.1f} s){}", p.files, secs, p.ok ? "" : ": " + p.error));
        check(p.folderRemoved && !std::filesystem::exists(folder), "loose folder removed after verification");
        const auto items = PlanRestore(adb, usb, p.zip, PhoneVolumeRoots(adb, usb, st), st);
        size_t present = 0, missing = 0, noLocal = 0;
        for (const auto& it : items) {
            present += it.state == RestoreState::Present;
            missing += it.state == RestoreState::Missing;
            noLocal += it.state == RestoreState::NoLocal;
        }
        LogInfo("m9 selftest: restore plan from zip: {} present, {} missing, {} no-local", present, missing, noLocal);
        check(!items.empty() && noLocal == 0, "restore plan reads the zip");
        std::filesystem::remove_all(root, ec);
    }

    LogInfo("m9 selftest: {}", failures == 0 ? "PASS" : std::format("{} FAILURE(S)", failures));
    return failures == 0 ? 0 : 1;
}

} // namespace ps
