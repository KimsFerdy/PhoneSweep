#include "apps/AppActions.h"
#include "bg/Usage.h"
#include "scan/Categorize.h"
#include "scan/FileStore.h"
#include "transfer/Manifest.h"

#include <doctest/doctest.h>

#include <ostream>

using namespace ps;

TEST_CASE("ParseTopSize")
{
    CHECK(ParseTopSize("4.8M") == static_cast<uint64_t>(4.8 * 1024 * 1024));
    CHECK(ParseTopSize("397M") == 397ull * 1024 * 1024);
    CHECK(ParseTopSize("1.2G") == static_cast<uint64_t>(1.2 * 1024 * 1024 * 1024));
    CHECK(ParseTopSize("876K") == 876ull * 1024);
    CHECK(ParseTopSize("0") == 0u);
    CHECK(ParseTopSize("123456") == 123456ull * 1024); // bare = KB
    CHECK_FALSE(ParseTopSize("abc"));
    CHECK_FALSE(ParseTopSize(""));
}

TEST_CASE("ParseTop + GroupTop on a real toybox top (Android 9)")
{
    const char* text = "Tasks: 728 total,   5 running, 716 sleeping,   0 stopped,   0 zombie\n"
                       "Mem:   5339752k total,  5212348k used,   127404k free,    93660k buffers\n"
                       "Swap:  2097148k total,  2096452k used,      696k free,  2001172k cached\n"
                       "800%cpu  29%user   0%nice  37%sys 723%idle   0%iow   3%irq   9%sirq   0%host\n"
                       "  PID USER        [%CPU] RES NAME                       \n"
                       "25697 shell        28.5 4.8M top\n"
                       "   15 root         11.4    0 [ksoftirqd/1]\n"
                       "20468 u0_a2714      8.5  54M com.sparkine.muvizedge\n"
                       "  703 system        8.5  14M surfaceflinger\n"
                       "23525 u0_a274       2.8 397M com.zhiliaoapp.musically.go\n"
                       "23600 u0_a274       1.0  20M com.zhiliaoapp.musically.go:push\n"
                       " 2663 u0_a54       29.7 216M com.android.systemui\n";
    const auto s = ParseTop(text);
    CHECK(s.cores == 8);
    REQUIRE(s.processes.size() == 7);
    CHECK(s.processes[2].name == "com.sparkine.muvizedge");
    const auto apps = GroupTop(s);
    REQUIRE(apps.size() == 3); // shell/root/system rows are not apps
    CHECK(apps[0].package == "com.android.systemui"); // heaviest CPU first
    CHECK(apps[2].package == "com.zhiliaoapp.musically.go");
    CHECK(apps[2].processes == 2);
    CHECK(apps[2].cpu == doctest::Approx(3.8));
    CHECK(IsHeavy(apps[0]));          // 29.7% CPU
    CHECK_FALSE(IsHeavy(apps[2]));    // 417 MB, 3.8%
}

TEST_CASE("ParseTop keeps only the last frame of -n 2")
{
    const char* text = "Tasks: 728 total\n"
                       "800%cpu  29%user\n"
                       "  PID USER        [%CPU] RES NAME\n"
                       " 2663 u0_a54        0.0 216M com.android.systemui\n"
                       "20468 u0_a2714      0.0  54M com.sparkine.muvizedge\n"
                       "Tasks: 728 total\n"
                       "800%cpu  31%user\n"
                       "  PID USER        [%CPU] RES NAME\n"
                       " 2663 u0_a54       21.5 216M com.android.systemui\n";
    const auto s = ParseTop(text);
    CHECK(s.cores == 8);
    REQUIRE(s.processes.size() == 1);
    CHECK(s.processes[0].cpu == doctest::Approx(21.5));
}

TEST_CASE("ParseBatteryStats (Android 9 format) + UsageStore attribution")
{
    const char* text = "  Estimated power use (mAh):\n"
                       "    Capacity: 3300, Typical: 0, Computed drain: 113, actual drain: 66.0-99.0\n"
                       "    Screen: 29.8 Excluded from smearing\n"
                       "    Uid 0: 18.2 ( cpu=6.73 wake=3.08 wifi=8.43 ) Excluded from smearing\n"
                       "    Cell standby: 8.45 ( radio=8.45 ) Excluded from smearing\n"
                       "    Uid 1000: 8.03 ( cpu=7.45 wake=0.0559 ) Excluded from smearing\n"
                       "    Uid u0a274: 4.46 ( cpu=0.706 wake=0.0145 wifi=3.74 ) Including smearing: 6.70 ( proportional=2.24 )\n"
                       "    Uid u95a274: 1.00 ( cpu=0.6 ) Including smearing: 1.5 ( proportional=0.5 )\n"
                       "\n"
                       "  Per-app mobile ms per packet:\n"
                       "    Uid u0a999: 99.0\n";
    const auto d = ParseBatteryStats(text);
    REQUIRE(d.found);
    CHECK(d.capacityMah == 3300);
    CHECK(d.computedMah == doctest::Approx(113));
    REQUIRE(d.entries.size() == 6); // stops at the blank line
    CHECK(d.entries[0].key == "Screen");
    CHECK(DrainLabel(d.entries[0]) == "Screen");
    const auto& u = *std::ranges::find(d.entries, std::string("u0a274"), &DrainEntry::key);
    CHECK(u.appId == 10274);
    CHECK(u.userId == 0);
    CHECK(DrainLabel(*std::ranges::find(d.entries, 1000, &DrainEntry::appId)) == "Android System");
    CHECK(DrainLabel(*std::ranges::find(d.entries, 0, &DrainEntry::appId)) == "Kernel (root)");

    std::vector<AppInfo> apps(1);
    apps[0].package = "com.zhiliaoapp.musically.go";
    apps[0].uid = 10274;
    UsageStore store;
    store.SetBattery(d, apps);
    const auto usage = store.Find("com.zhiliaoapp.musically.go");
    REQUIRE(usage);
    CHECK(*usage->batteryMah == doctest::Approx(5.46)); // main user + Secure Folder copy
    CHECK(*usage->batteryShare == doctest::Approx(5.46 / 113));
}

TEST_CASE("ParseBatteryStats (Android 12+ format)")
{
    const char* text = "Estimated power use (mAh):\n"
                       "    Capacity: 4500, Computed drain: 512, actual drain: 450-480\n"
                       "    Global\n"
                       "      screen: 120 apps: 120 duration: 3h 2m\n"
                       "    UID u0a123: 42.5 fg: 30.1 bg: 10.0 fgs: 2.4 cached: 0\n"
                       "\n";
    const auto d = ParseBatteryStats(text);
    REQUIRE(d.found);
    const auto it = std::ranges::find(d.entries, std::string("u0a123"), &DrainEntry::key);
    REQUIRE(it != d.entries.end());
    CHECK(it->appId == 10123);
    CHECK(it->mAh == doctest::Approx(42.5));
}

TEST_CASE("PlayStoreUrl only for real package names")
{
    CHECK(PlayStoreUrl("com.whatsapp") == "https://play.google.com/store/apps/details?id=com.whatsapp");
    CHECK(PlayStoreUrl("org.my_app.x1") == "https://play.google.com/store/apps/details?id=org.my_app.x1");
    CHECK(PlayStoreUrl("") == "");
    CHECK(PlayStoreUrl("nodot") == "");
    CHECK(PlayStoreUrl("com.x&y=1") == "");
    CHECK(PlayStoreUrl("com.x\"; calc") == "");
}

TEST_CASE("FormatAskAi includes the question, app block and measured usage")
{
    AppInfo a;
    a.package = "com.example.unknownsource";
    a.installer = "";
    a.grantedPermissions = { "android.permission.READ_SMS" };
    AppUsage u;
    u.cpu = 12.5;
    u.ramBytes = 300'000'000;
    u.batteryMah = 9.5;
    u.batteryShare = 0.084;
    const auto s = FormatAskAi(a, u, "Samsung SM-G960F");
    CHECK(s.find("Samsung SM-G960F") != std::string::npos);
    CHECK(s.find("Is it safe to uninstall") != std::string::npos);
    CHECK(s.find("com.example.unknownsource") != std::string::npos);
    CHECK(s.find("READ_SMS") != std::string::npos);
    CHECK(s.find("12.5% of one core") != std::string::npos);
    CHECK(s.find("8.4% of all drain") != std::string::npos);
    CHECK(s.find("Claude") == std::string::npos); // AI-neutral
    CHECK(FormatAskAi(a, std::nullopt, "").find("Measured on the phone") == std::string::npos);
}

TEST_CASE("AppStorageFolders + Nuke prompt unsaved count")
{
    FileStore s;
    s.SetVolumes({ { "/storage/emulated/0/", "Internal" }, { "/storage/0000-0000/", "SD card" } });
    std::vector<FileEntry> b(5);
    b[0].path = "/storage/emulated/0/Android/media/com.whatsapp/WhatsApp/Media/a.jpg";
    b[0].size = 1000;
    b[0].mtime = 5;
    b[1].path = "/storage/emulated/0/Android/media/com.whatsapp/WhatsApp/Media/b.jpg";
    b[1].size = 2000;
    b[2].path = "/storage/emulated/0/Android/obb/com.whatsapp/x.obb";
    b[2].size = 5000;
    b[3].path = "/storage/0000-0000/Android/data/com.whatsapp/cache.bin";
    b[3].size = 300;
    b[4].path = "/storage/emulated/0/Android/media/com.whatsapp.w4b/other.jpg"; // different app!
    b[4].size = 7;
    for (auto& e : b)
        Classify(e, e.path.starts_with("/storage/0000") ? "/storage/0000-0000/" : "/storage/emulated/0/");
    s.Append(std::move(b));
    VerifiedIndex vi;
    ManifestEntry m;
    m.devicePath = "/storage/emulated/0/Android/media/com.whatsapp/WhatsApp/Media/a.jpg";
    m.size = 1000;
    m.mtime = 5;
    vi.Add(m, "C:/backup");

    const auto f = AppStorageFolders("com.whatsapp", s, vi);
    REQUIRE(f.size() == 3);
    uint64_t bytes = 0;
    CHECK(UnsavedAppFiles(f, &bytes) == 2); // b.jpg + cache.bin; obb doesn't count
    CHECK(bytes == 2300);
    for (const auto& x : f)
        CHECK(x.path.ends_with("/com.whatsapp/"));
}
