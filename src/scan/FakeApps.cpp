#include "scan/FakeApps.h"

#include "scan/AppParse.h"

#include <algorithm>
#include <set>

namespace ps {

namespace {

struct Spec {
    const char* package;
    const char* label;
    const char* installer; // "" = sideloaded
    double mb;              // app + data
    std::vector<const char*> perms;
    bool boot = false, accessibility = false, notif = false, admin = false, launcher = true;
};

} // namespace

std::vector<AppInfo> FakeApps()
{
    constexpr const char* kPlay = "com.android.vending";
    const std::vector<Spec> specs = {
        { "com.whatsapp", "WhatsApp", kPlay, 2100, { "android.permission.READ_CONTACTS", "android.permission.CAMERA", "android.permission.RECORD_AUDIO", "android.permission.ACCESS_FINE_LOCATION" }, true },
        { "com.instagram.android", "Instagram", kPlay, 1400, { "android.permission.CAMERA", "android.permission.RECORD_AUDIO", "android.permission.ACCESS_FINE_LOCATION" }, true },
        { "com.zhiliaoapp.musically", "TikTok", kPlay, 1900, { "android.permission.CAMERA", "android.permission.RECORD_AUDIO", "android.permission.READ_CONTACTS" }, true },
        { "com.spotify.music", "Spotify", kPlay, 950, { "android.permission.RECORD_AUDIO" }, true },
        { "com.google.android.youtube", "YouTube", kPlay, 640, {} },
        { "org.telegram.messenger", "Telegram", kPlay, 1250, { "android.permission.READ_CONTACTS", "android.permission.CAMERA" }, true },
        { "com.netflix.mediaclient", "Netflix", kPlay, 520, {} },
        { "com.snapchat.android", "Snapchat", kPlay, 870, { "android.permission.CAMERA", "android.permission.ACCESS_FINE_LOCATION" }, true },
        { "com.facebook.katana", "Facebook", kPlay, 1100, { "android.permission.CAMERA", "android.permission.ACCESS_FINE_LOCATION", "android.permission.READ_CONTACTS" }, true },
        { "com.duolingo", "Duolingo", kPlay, 310, {} },
        { "com.ubercab", "Uber", kPlay, 420, { "android.permission.ACCESS_FINE_LOCATION" } },
        { "com.discord", "Discord", kPlay, 380, { "android.permission.RECORD_AUDIO" } },
        { "com.microsoft.teams", "Microsoft Teams", kPlay, 610, { "android.permission.RECORD_AUDIO", "android.permission.CAMERA" }, true },
        { "com.google.android.apps.maps", "Maps", kPlay, 470, { "android.permission.ACCESS_FINE_LOCATION" } },
        { "com.amazon.mShop.android.shopping", "Amazon Shopping", "com.amazon.venezia", 290, {} },
        { "com.sec.android.app.sbrowser", "Samsung Internet", "com.sec.android.app.samsungapps", 260, {} },
        // The ones PhoneSweep is meant to catch (made-up packages).
        { "com.fastcleaner.boost.free", "Super Cleaner Boost", "", 85,
            { "android.permission.READ_SMS", "android.permission.READ_CALL_LOG", "android.permission.SYSTEM_ALERT_WINDOW", "android.permission.REQUEST_INSTALL_PACKAGES" }, true, true, true },
        { "com.flashlight.ledpro", "LED Flashlight Pro", "", 34,
            { "android.permission.READ_CONTACTS", "android.permission.ACCESS_FINE_LOCATION", "android.permission.SYSTEM_ALERT_WINDOW" }, true, false, true },
        { "net.qrscan.reader.x", "QR Scanner X", "com.android.packageinstaller", 21,
            { "android.permission.CAMERA", "android.permission.READ_SMS" }, true, true },
        { "com.wallpaper.hd.live4k", "4K Live Wallpapers", "", 140, { "android.permission.SYSTEM_ALERT_WINDOW" }, true, false, false, false, false },
    };

    std::set<std::string, std::less<>> launcher, boot, accessibility, notif, admins;
    for (const auto& s : specs) {
        if (s.launcher) launcher.insert(s.package);
        if (s.boot) boot.insert(s.package);
        if (s.accessibility) accessibility.insert(s.package);
        if (s.notif) notif.insert(s.package);
        if (s.admin) admins.insert(s.package);
    }
    RiskContext ctx;
    ctx.launcher = &launcher;
    ctx.boot = &boot;
    ctx.accessibility = &accessibility;
    ctx.notifListeners = &notif;
    ctx.deviceAdmins = &admins;
    ctx.deviceSdk = 34;

    std::vector<AppInfo> out;
    int uid = 10200;
    for (const auto& s : specs) {
        AppInfo a;
        a.package = s.package;
        a.label = s.label;
        a.installer = s.installer;
        a.versionName = "1.0";
        a.targetSdk = 34;
        a.uid = uid++;
        a.firstInstall = "2025-03-14 10:22:05";
        a.lastUpdate = "2026-10-30 08:01:44";
        a.codePath = std::string("/data/app/") + s.package + "-1";
        a.apkPaths = { a.codePath + "/base.apk" };
        a.launcher = s.launcher;
        a.codeBytes = static_cast<uint64_t>(s.mb * 0.35 * 1'000'000);
        a.dataBytes = static_cast<uint64_t>(s.mb * 0.55 * 1'000'000);
        a.cacheBytes = static_cast<uint64_t>(s.mb * 0.10 * 1'000'000);
        for (const char* p : s.perms)
            a.grantedPermissions.emplace_back(p);
        std::ranges::sort(a.grantedPermissions);
        ComputeFlags(a, ctx);
        out.push_back(std::move(a));
    }
    return out;
}

} // namespace ps
