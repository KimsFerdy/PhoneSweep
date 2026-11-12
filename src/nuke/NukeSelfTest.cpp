#include "nuke/NukeSelfTest.h"

#include "adb/AdbClient.h"
#include "adb/Device.h"
#include "app/Settings.h"
#include "nuke/Nuker.h"
#include "scan/ScanParse.h"
#include "util/Log.h"
#include "util/Strings.h"

#include <format>

namespace ps {

int RunNukeSelfTest()
{
    const auto exe = AdbClient::Locate();
    if (!exe) {
        LogError("selftest: adb not found");
        return 2;
    }
    const AdbClient adb(*exe);
    const auto devices = ParseDevicesList(adb.Run({ "devices", "-l" }).out);
    const DeviceEntry* dev = nullptr;
    for (const auto& d : devices)
        if (d.state == DeviceEntry::State::Device)
            dev = &d;
    if (!dev) {
        LogError("selftest: no authorized device");
        return 3;
    }
    const std::string serial = dev->serial;

    std::vector<std::string> roots = { "/storage/emulated/0/" };
    for (const auto& id : ParseStorageVolumes(adb.Shell(serial, ShellCommand("ls").arg("-1").arg("/storage")).out))
        roots.push_back("/storage/" + id + "/");

    // Throwaway files only, under a folder we create.
    std::vector<std::string> paths;
    for (const auto& root : roots) {
        const std::string base = root + "PhoneSweepSelfTest/";
        for (const char* rel : { "plain.txt", "with space.jpg", "it's here.mp4", "emoji \xF0\x9F\x98\x80.png", "nested/deeper/file.bin",
                 "nested/deeper/line\nbreak.txt", "nested/other.dat" })
            paths.push_back(base + rel);
    }
    ShellCommand make("true");
    for (const auto& p : paths) {
        const auto dir = p.substr(0, p.rfind('/'));
        make.raw(";").then("", ShellCommand("mkdir").arg("-p").arg(dir));
        make.raw("&&").then("", ShellCommand("echo").arg("phonesweep selftest").raw(">").arg(p));
    }
    const auto mk = adb.Shell(serial, make, { .timeout = 60s });
    LogInfo("selftest: created {} files on {} volume(s) (exit {})", paths.size(), roots.size(), mk.exitCode);

    // Confirm they exist before deleting (if creation failed, the test proves nothing).
    std::vector<std::string> before;
    {
        ShellCommand ls("ls");
        ls.arg("-d").arg("--");
        for (const auto& p : paths)
            ls.arg(p);
        const auto r = adb.Shell(serial, ls.raw("2>/dev/null"), { .timeout = 30s });
        size_t n = 0;
        for (const auto& p : paths)
            n += r.out.find(p.substr(p.rfind('/') + 1).substr(0, 6)) != std::string::npos;
        LogInfo("selftest: {} of {} files visible before nuke", n, paths.size());
    }

    NukeTask t;
    t.serial = serial;
    t.deletePaths = paths;
    t.volumeRoots = roots;
    t.clearCaches = false; // not part of this test
    t.mediaRescan = true;
    t.removeEmptyFolders = true;
    t.tmpDir = Settings::DataDir() / L"tmp";
    NukeProgress progress;
    RunNuke(adb, t, progress, {});

    // Verify: every test file gone; nested folders removed; top-level test folder now empty.
    int failures = 0;
    for (const auto& root : roots) {
        const auto r = adb.Shell(serial, ShellCommand("find").arg(root + "PhoneSweepSelfTest/"), { .timeout = 30s });
        std::vector<std::string> left;
        ForEachLine(r.out, [&](std::string_view l) {
            if (!l.empty() && l != root + "PhoneSweepSelfTest/")
                left.emplace_back(l);
        });
        if (!left.empty()) {
            ++failures;
            for (const auto& l : left)
                LogError("selftest: still present: {}", l);
        }
        // The top-level folder is never removed by Nuke by design; tidy it here.
        adb.Shell(serial, ShellCommand("rmdir").arg(root + "PhoneSweepSelfTest"), { .timeout = 15s });
    }
    std::lock_guard l(progress.mutex);
    LogInfo("selftest: deleted={} failed={} folders={} phase={}", progress.deleted, progress.deleteFailed, progress.foldersRemoved,
        PhaseName(progress.phase));
    const bool ok = failures == 0 && progress.deleted == paths.size() && progress.deleteFailed == 0;
    LogInfo("selftest: {}", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

} // namespace ps
