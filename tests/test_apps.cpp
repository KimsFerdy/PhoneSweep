#include "report/AppListFormat.h"
#include "scan/AppParse.h"

#include <doctest/doctest.h>

#include <ostream>

using namespace ps;

TEST_CASE("ParsePmList: '=' inside the path, null installer, missing uid")
{
    const char* text =
        "package:/data/app/cz.hipercalc-DOzYflhD1UeztglX5cPWbQ==/base.apk=cz.hipercalc  installer=com.android.vending uid:10272\n"
        "package:/data/app/com.evil-AAA=/base.apk=com.evil  installer=null uid:10300\r\n"
        "package:/system/app/Foo/Foo.apk=com.foo\n"
        "garbage\n";
    const auto l = ParsePmList(text);
    REQUIRE(l.size() == 3);
    CHECK(l[0].package == "cz.hipercalc");
    CHECK(l[0].apkPath == "/data/app/cz.hipercalc-DOzYflhD1UeztglX5cPWbQ==/base.apk");
    CHECK(l[0].installer == "com.android.vending");
    CHECK(l[0].uid == 10272);
    CHECK(l[1].package == "com.evil");
    CHECK(l[1].installer.empty());
    CHECK(l[2].package == "com.foo");
    CHECK(l[2].uid == -1);
}

TEST_CASE("ParseDumpsysPackages: fields, install + user-0 runtime permissions")
{
    const char* text = "Packages:\n"
                       "  Package [com.whatsapp.w4b] (3c8975f):\n"
                       "    userId=10238\n"
                       "    codePath=/data/app/com.whatsapp.w4b-wvw==\n"
                       "    versionCode=242212 minSdk=21 targetSdk=34\n"
                       "    versionName=2.24.22.12\n"
                       "    splits=[base, config.arm64_v8a, config.xxhdpi]\n"
                       "    firstInstallTime=2023-01-02 10:00:00\n"
                       "    lastUpdateTime=2024-11-01 09:30:00\n"
                       "    installerPackageName=com.android.vending\n"
                       "    install permissions:\n"
                       "      android.permission.INTERNET: granted=true\n"
                       "      android.permission.SYSTEM_ALERT_WINDOW: granted=true\n"
                       "      android.permission.REQUEST_INSTALL_PACKAGES: granted=false\n"
                       "    User 0: ceDataInode=134328 installed=true hidden=false enabled=0 instant=false\n"
                       "      gids=[3003]\n"
                       "      runtime permissions:\n"
                       "        android.permission.READ_SMS: granted=true\n"
                       "        android.permission.CAMERA: granted=false\n"
                       "      disabledComponents:\n"
                       "        com.x.Foo\n"
                       "    User 95: ceDataInode=0 installed=false enabled=0\n"
                       "      runtime permissions:\n"
                       "        android.permission.READ_CALL_LOG: granted=true\n"
                       "  Package [com.disabled] (1):\n"
                       "    versionName=1.0\n"
                       "    User 0: installed=true enabled=3\n"
                       "\n"
                       "Hidden system packages:\n"
                       "  Package [com.whatsapp.w4b] (old):\n"
                       "    versionName=0.0.1\n";
    const auto m = ParseDumpsysPackages(text);
    REQUIRE(m.size() == 2);
    const auto& w = m.at("com.whatsapp.w4b");
    CHECK(w.versionName == "2.24.22.12"); // not overwritten by the hidden-system copy
    CHECK(w.versionCode == 242212);
    CHECK(w.targetSdk == 34);
    CHECK(w.codePath == "/data/app/com.whatsapp.w4b-wvw==");
    CHECK(w.firstInstall == "2023-01-02 10:00:00");
    CHECK(w.installer == "com.android.vending");
    CHECK(w.splits == std::vector<std::string>{ "base", "config.arm64_v8a", "config.xxhdpi" });
    CHECK(w.grantedInstall == std::vector<std::string>{ "android.permission.INTERNET", "android.permission.SYSTEM_ALERT_WINDOW" });
    CHECK(w.requestedInstall.size() == 3);
    CHECK(w.grantedRuntime == std::vector<std::string>{ "android.permission.READ_SMS" }); // user 95 ignored
    CHECK(w.user0Enabled == 0);
    CHECK(m.at("com.disabled").user0Enabled == 3);
}

TEST_CASE("Component, colon-list and device-admin parsers")
{
    const char* q = "117 activities found:\n"
                    "  Activity #0:\n"
                    "    priority=500 preferredOrder=0 match=0x108000\n"
                    "    com.sec.android.gallery3d/com.samsung.android.gallery.app.activity.GalleryActivity\n"
                    "  Activity #1:\n"
                    "    com.whatsapp/.Main\n";
    const auto pk = ParseComponentPackages(q);
    CHECK(pk.size() == 2);
    CHECK(pk.contains("com.whatsapp"));

    const auto acc = ParseColonComponents("com.a/com.a.Svc:com.rise.clicker/x.Y:com.b/.Z\n");
    CHECK(acc.size() == 3);
    CHECK(acc.contains("com.rise.clicker"));
    CHECK(ParseColonComponents("null").empty());
    CHECK(ParseColonComponents("").empty());

    const char* dp = "Current Device Policy Manager state:\n"
                     "  Enabled Device Admins (User 0, provisioningState: 0):\n"
                     "    com.google.android.gms/.mdm.receivers.MdmDeviceAdminReceiver:\n"
                     "      testOnlyAdmin=false\n"
                     "    com.evil.admin/.Rcv:\n"
                     "      policies:\n"
                     "  Other section:\n"
                     "    com.not.admin/.X:\n";
    const auto admins = ParseDeviceAdmins(dp);
    CHECK(admins.size() == 2);
    CHECK(admins.contains("com.evil.admin"));
    CHECK_FALSE(admins.contains("com.not.admin"));
}

TEST_CASE("ParseDiskStats")
{
    const char* text = "Latency: 5ms [512B Data Write]\n"
                       "Package Names: [\"com.a\",\"com.b\"]\n"
                       "App Sizes: [100,200]\n"
                       "App Data Sizes: [10,20]\n"
                       "Cache Sizes: [1,2]\n";
    const auto d = ParseDiskStats(text);
    REQUIRE(d.size() == 2);
    CHECK(d.at("com.b").app == 200);
    CHECK(d.at("com.b").data == 20);
    CHECK(d.at("com.b").cache == 2);
    CHECK(ParseDiskStats("nothing here").empty());
}

TEST_CASE("ParseBadgingLabel")
{
    CHECK(ParseBadgingLabel("package: name='x' versionCode='1'\napplication-label:'WhatsApp Business'\napplication-label-fr:'WA'\n")
        == "WhatsApp Business");
    CHECK(ParseBadgingLabel("application: label='Old Style' icon='res/a.png'\n") == "Old Style");
    CHECK(ParseBadgingLabel("application-label:'Kim\\'s App'\n") == "Kim's App");
    CHECK(ParseBadgingLabel("").empty());
}

TEST_CASE("ComputeFlags + risk score")
{
    std::set<std::string, std::less<>> launcher = { "com.good" };
    std::set<std::string, std::less<>> boot = { "com.evil" };
    std::set<std::string, std::less<>> access = { "com.evil" };
    std::set<std::string, std::less<>> none;
    RiskContext ctx{ &launcher, &boot, &access, &none, &none, 28 };

    AppInfo good;
    good.package = "com.good";
    good.installer = "com.android.vending";
    good.codePath = "/data/app/com.good-1";
    good.grantedPermissions = { "android.permission.INTERNET" };
    ComputeFlags(good, ctx);
    CHECK(good.flags == 0);
    CHECK(good.risk == 0);

    AppInfo evil;
    evil.package = "com.evil";
    evil.codePath = "/data/app/com.evil-1"; // installer "" + /data/app = sideloaded
    evil.grantedPermissions = { "android.permission.ACCESS_FINE_LOCATION", "android.permission.READ_SMS",
        "android.permission.SYSTEM_ALERT_WINDOW" }; // sorted
    ComputeFlags(evil, ctx);
    CHECK(evil.Has(AppFlag_Accessibility));
    CHECK(evil.Has(AppFlag_Sms));
    CHECK(evil.Has(AppFlag_Sideloaded));
    CHECK(evil.Has(AppFlag_NoIcon));
    CHECK(evil.Has(AppFlag_Overlay));
    CHECK(evil.Has(AppFlag_BgLocation)); // SDK 28: any location grant is background
    CHECK(evil.Has(AppFlag_Boot));
    CHECK(evil.risk == 30 + 25 + 15 + 15 + 10 + 8);

    // Preinstalled (system partition, no installer) is not "sideloaded".
    AppInfo pre;
    pre.package = "com.samsung.thing";
    pre.codePath = "/system/app/Thing";
    ComputeFlags(pre, RiskContext{ nullptr, nullptr, nullptr, nullptr, nullptr, 28 });
    CHECK_FALSE(pre.Has(AppFlag_Sideloaded));
    CHECK_FALSE(pre.Has(AppFlag_NoIcon)); // launcher unknown -> not flagged
    CHECK(InstallerName(pre) == "Preinstalled");

    // On Android 10+ plain location is foreground-only.
    AppInfo loc;
    loc.package = "com.loc";
    loc.installer = "com.android.vending";
    loc.grantedPermissions = { "android.permission.ACCESS_FINE_LOCATION" };
    ComputeFlags(loc, RiskContext{ &launcher, nullptr, nullptr, nullptr, nullptr, 34 });
    CHECK_FALSE(loc.Has(AppFlag_BgLocation));
}

TEST_CASE("App list format is stable and compact")
{
    AppInfo a;
    a.package = "com.example.thing";
    a.label = "Thing";
    a.versionName = "2.3.1";
    a.firstInstall = "2025-08-14 10:11:12";
    a.codePath = "/data/app/com.example.thing-1";
    a.codeBytes = 100'000'000;
    a.dataBytes = 43'000'000;
    a.launcher = false;
    a.grantedPermissions = { "android.permission.INTERNET", "android.permission.READ_SMS" };
    a.flags = AppFlag_Sideloaded | AppFlag_NoIcon | AppFlag_Accessibility | AppFlag_Sms;
    CHECK(FormatAppBlock(a) ==
        "package: com.example.thing\n"
        "label: Thing\n"
        "installer: sideloaded\n"
        "version: 2.3.1\n"
        "installed: 2025-08-14\n"
        "size: 143 MB\n"
        "launcher_icon: no\n"
        "flags: ACCESSIBILITY, SMS, SIDELOADED, NO_ICON\n"
        "permissions: android.permission.INTERNET, android.permission.READ_SMS\n");

    AppListOptions sensitiveOnly;
    sensitiveOnly.fullPermissions = false;
    CHECK(FormatAppBlock(a, sensitiveOnly).find("permissions: android.permission.READ_SMS\n") != std::string::npos);

    AppInfo b;
    b.package = "com.b";
    b.installer = "com.android.vending";
    const AppInfo* both[] = { &a, &b };
    const std::string list = FormatAppList(both);
    CHECK(list.find("\n\npackage: com.b\n") != std::string::npos);
    CHECK(list.find("label: unknown") != std::string::npos);
    CHECK(list.find("installer: play_store") != std::string::npos);
    CHECK(list.find("flags: none") != std::string::npos);
}
