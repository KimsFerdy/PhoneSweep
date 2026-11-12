#include "scan/AppParse.h"

#include "util/Strings.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>

namespace ps {

// ---- Model helpers -----------------------------------------------------------

const std::vector<FlagInfo>& AllFlags()
{
    static const std::vector<FlagInfo> kFlags = {
        { AppFlag_Accessibility, "ACCESSIBILITY", "ACCESSIBILITY", FlagSeverity::High, 30,
            "Enabled accessibility service: can read everything on screen and tap for you." },
        { AppFlag_DeviceAdmin, "DEVICE_ADMIN", "DEVICE ADMIN", FlagSeverity::High, 30,
            "Device administrator: can lock or wipe the phone and blocks normal uninstall." },
        { AppFlag_Sms, "SMS", "SMS", FlagSeverity::High, 25, "Can read or send text messages, including login codes." },
        { AppFlag_NotifListener, "NOTIF_LISTENER", "NOTIFICATIONS", FlagSeverity::High, 20,
            "Notification listener: sees the content of every notification." },
        { AppFlag_CallLog, "CALL_LOG", "CALL LOG", FlagSeverity::High, 20, "Can read your call history." },
        { AppFlag_Sideloaded, "SIDELOADED", "SIDELOADED", FlagSeverity::Medium, 15, "Not installed from an app store." },
        { AppFlag_NoIcon, "NO_ICON", "NO ICON", FlagSeverity::Medium, 15, "Has no launcher icon, so it's hidden from the app drawer." },
        { AppFlag_Overlay, "OVERLAY", "OVERLAY", FlagSeverity::Medium, 10, "Can draw over other apps (used by both chat heads and phishing)." },
        { AppFlag_InstallPackages, "INSTALL_PACKAGES", "INSTALLS APPS", FlagSeverity::Medium, 10, "Can ask to install other apps." },
        { AppFlag_BgLocation, "BG_LOCATION", "LOCATION", FlagSeverity::Medium, 8, "Can get your location while in the background." },
        { AppFlag_Boot, "BOOT", "STARTS AT BOOT", FlagSeverity::Info, 0, "Starts automatically when the phone boots." },
        { AppFlag_Disabled, "DISABLED", "DISABLED", FlagSeverity::Info, 0, "Disabled on the phone." },
    };
    return kFlags;
}

const FlagInfo* FindFlag(AppFlag f)
{
    for (const auto& fi : AllFlags())
        if (fi.flag == f)
            return &fi;
    return nullptr;
}

bool IsStoreInstaller(std::string_view i)
{
    static constexpr std::array kStores = {
        std::string_view("com.android.vending"),             // Google Play
        std::string_view("com.sec.android.app.samsungapps"), // Galaxy Store
        std::string_view("com.amazon.venezia"),              // Amazon Appstore
        std::string_view("com.huawei.appmarket"),            // AppGallery
        std::string_view("com.xiaomi.market"),
        std::string_view("com.heytap.market"),
        std::string_view("com.oppo.market"),
        std::string_view("com.bbk.appstore"),
        std::string_view("org.fdroid.fdroid"),
        std::string_view("com.google.android.feedback"), // old Play installs
    };
    return std::ranges::find(kStores, i) != kStores.end();
}

namespace {

bool IsPreinstalledPath(std::string_view codePath)
{
    return codePath.starts_with("/system/") || codePath.starts_with("/product/") || codePath.starts_with("/vendor/")
        || codePath.starts_with("/preload/") || codePath.starts_with("/system_ext/") || codePath.starts_with("/prism/")
        || codePath.starts_with("/optics/");
}

} // namespace

std::string InstallerName(const AppInfo& app)
{
    const auto& i = app.installer;
    if (i == "com.android.vending" || i == "com.google.android.feedback")
        return "Play Store";
    if (i == "com.sec.android.app.samsungapps")
        return "Galaxy Store";
    if (i == "com.amazon.venezia")
        return "Amazon Appstore";
    if (i == "com.huawei.appmarket")
        return "AppGallery";
    if (i == "com.xiaomi.market")
        return "Xiaomi GetApps";
    if (i == "com.heytap.market" || i == "com.oppo.market")
        return "OPPO App Market";
    if (i == "com.bbk.appstore")
        return "vivo App Store";
    if (i == "org.fdroid.fdroid")
        return "F-Droid";
    if (i == "com.facebook.system")
        return "Facebook App Manager"; // preloaded on many phones; installs WhatsApp/Facebook updates
    if (i.empty())
        return app.system || IsPreinstalledPath(app.codePath) ? "Preinstalled" : "Sideloaded";
    if (i.find("packageinstaller") != std::string::npos)
        return "Sideloaded";
    return i; // installed by another app (file manager, Xender, ...)
}

bool IsSensitivePermission(std::string_view p)
{
    static constexpr std::array kSensitive = {
        std::string_view("android.permission.READ_SMS"), std::string_view("android.permission.RECEIVE_SMS"),
        std::string_view("android.permission.SEND_SMS"), std::string_view("android.permission.RECEIVE_MMS"),
        std::string_view("android.permission.RECEIVE_WAP_PUSH"), std::string_view("android.permission.READ_CALL_LOG"),
        std::string_view("android.permission.WRITE_CALL_LOG"), std::string_view("android.permission.PROCESS_OUTGOING_CALLS"),
        std::string_view("android.permission.READ_CONTACTS"), std::string_view("android.permission.WRITE_CONTACTS"),
        std::string_view("android.permission.GET_ACCOUNTS"), std::string_view("android.permission.ACCESS_FINE_LOCATION"),
        std::string_view("android.permission.ACCESS_COARSE_LOCATION"), std::string_view("android.permission.ACCESS_BACKGROUND_LOCATION"),
        std::string_view("android.permission.CAMERA"), std::string_view("android.permission.RECORD_AUDIO"),
        std::string_view("android.permission.READ_PHONE_STATE"), std::string_view("android.permission.CALL_PHONE"),
        std::string_view("android.permission.READ_EXTERNAL_STORAGE"), std::string_view("android.permission.WRITE_EXTERNAL_STORAGE"),
        std::string_view("android.permission.MANAGE_EXTERNAL_STORAGE"), std::string_view("android.permission.SYSTEM_ALERT_WINDOW"),
        std::string_view("android.permission.REQUEST_INSTALL_PACKAGES"), std::string_view("android.permission.BODY_SENSORS"),
        std::string_view("android.permission.READ_CALENDAR"), std::string_view("android.permission.WRITE_CALENDAR"),
        std::string_view("android.permission.BIND_ACCESSIBILITY_SERVICE"), std::string_view("android.permission.PACKAGE_USAGE_STATS"),
        std::string_view("android.permission.WRITE_SETTINGS"), std::string_view("android.permission.QUERY_ALL_PACKAGES"),
    };
    return std::ranges::find(kSensitive, p) != kSensitive.end();
}

// ---- Parsers -----------------------------------------------------------------

std::vector<PmEntry> ParsePmList(std::string_view text)
{
    std::vector<PmEntry> out;
    ForEachLine(text, [&](std::string_view line) {
        line = Trim(line);
        if (!line.starts_with("package:"))
            return;
        line.remove_prefix(8);
        PmEntry e;
        // Trailing "  installer=x uid:n" (either may be missing).
        size_t headEnd = line.size();
        if (const auto u = line.rfind(" uid:"); u != std::string_view::npos) {
            e.uid = ParseInt<int>(Trim(line.substr(u + 5))).value_or(-1);
            headEnd = std::min(headEnd, u);
        }
        if (const auto ins = line.rfind(" installer="); ins != std::string_view::npos && ins < headEnd) {
            const auto v = Trim(line.substr(ins + 11, headEnd - ins - 11));
            e.installer = v == "null" ? std::string() : std::string(v);
            headEnd = ins;
        }
        const auto head = Trim(line.substr(0, headEnd));
        // "<path>=<pkg>": the path itself may contain '=' (base64 dir names), so split on the LAST one.
        const auto eq = head.rfind('=');
        if (eq == std::string_view::npos) {
            e.package = std::string(head); // `pm list packages` without -f
        } else {
            e.apkPath = std::string(head.substr(0, eq));
            e.package = std::string(head.substr(eq + 1));
        }
        if (!e.package.empty())
            out.push_back(std::move(e));
    });
    return out;
}

namespace {

size_t Indent(std::string_view line)
{
    size_t n = 0;
    while (n < line.size() && line[n] == ' ')
        ++n;
    return n;
}

std::string_view ValueAfter(std::string_view line, std::string_view key)
{
    const auto p = line.find(key);
    if (p == std::string_view::npos)
        return {};
    auto v = line.substr(p + key.size());
    const auto sp = v.find(' ');
    return sp == std::string_view::npos ? v : v.substr(0, sp);
}

} // namespace

std::map<std::string, PackageDump, std::less<>> ParseDumpsysPackages(std::string_view text)
{
    std::map<std::string, PackageDump, std::less<>> out;
    enum class Sub { None, InstallPerms, RuntimePerms };
    PackageDump* cur = nullptr;
    Sub sub = Sub::None;
    bool inUser0 = false;
    bool inPackages = false;

    ForEachLine(text, [&](std::string_view line) {
        const size_t ind = Indent(line);
        const auto t = Trim(line);
        if (t.empty())
            return;
        if (ind == 0) {
            // Section header: only "Packages:" holds what we want ("Hidden system packages:" repeats blocks).
            inPackages = t == "Packages:";
            cur = nullptr;
            return;
        }
        if (!inPackages)
            return;
        if (ind == 2 && t.starts_with("Package [")) {
            const auto close = t.find(']');
            if (close == std::string_view::npos) {
                cur = nullptr;
                return;
            }
            cur = &out[std::string(t.substr(9, close - 9))];
            sub = Sub::None;
            inUser0 = false;
            return;
        }
        if (!cur)
            return;

        if (ind == 4) {
            sub = Sub::None;
            if (t.starts_with("User ")) {
                inUser0 = t.starts_with("User 0:");
                if (inUser0) {
                    cur->user0Installed = ValueAfter(t, "installed=") != "false";
                    cur->user0Enabled = ParseInt<int>(ValueAfter(t, "enabled=")).value_or(0);
                }
                return;
            }
            inUser0 = false;
            if (t == "install permissions:")
                sub = Sub::InstallPerms;
            else if (t.starts_with("codePath="))
                cur->codePath = std::string(t.substr(9));
            else if (t.starts_with("versionName="))
                cur->versionName = std::string(t.substr(12));
            else if (t.starts_with("versionCode=")) {
                cur->versionCode = ParseInt<int64_t>(ValueAfter(t, "versionCode=")).value_or(0);
                cur->targetSdk = ParseInt<int>(ValueAfter(t, "targetSdk=")).value_or(0);
            } else if (t.starts_with("firstInstallTime="))
                cur->firstInstall = std::string(t.substr(17));
            else if (t.starts_with("lastUpdateTime="))
                cur->lastUpdate = std::string(t.substr(15));
            else if (t.starts_with("installerPackageName="))
                cur->installer = t.substr(21) == "null" ? std::string() : std::string(t.substr(21));
            else if (t.starts_with("splits=[")) {
                auto list = t.substr(8);
                if (!list.empty() && list.back() == ']')
                    list.remove_suffix(1);
                size_t s = 0;
                while (s < list.size()) {
                    auto c = list.find(',', s);
                    const auto item = Trim(list.substr(s, c == std::string_view::npos ? std::string_view::npos : c - s));
                    if (!item.empty())
                        cur->splits.emplace_back(item);
                    if (c == std::string_view::npos)
                        break;
                    s = c + 1;
                }
            }
            return;
        }
        if (ind == 6) {
            if (inUser0 && t == "runtime permissions:") {
                sub = Sub::RuntimePerms;
                return;
            }
            if (sub == Sub::InstallPerms) {
                const auto colon = t.find(':');
                if (colon != std::string_view::npos) {
                    const std::string name(t.substr(0, colon));
                    cur->requestedInstall.push_back(name);
                    if (t.find("granted=true") != std::string_view::npos)
                        cur->grantedInstall.push_back(name);
                }
                return;
            }
            if (inUser0)
                sub = Sub::None; // another user-0 sub-list (gids, disabledComponents...)
            return;
        }
        if (ind == 8 && sub == Sub::RuntimePerms) {
            const auto colon = t.find(':');
            if (colon != std::string_view::npos && t.find("granted=true") != std::string_view::npos)
                cur->grantedRuntime.emplace_back(t.substr(0, colon));
        }
    });
    return out;
}

std::set<std::string, std::less<>> ParseComponentPackages(std::string_view text)
{
    std::set<std::string, std::less<>> out;
    ForEachLine(text, [&](std::string_view line) {
        const auto t = Trim(line);
        if (t.empty() || t.find(' ') != std::string_view::npos || t.find('=') != std::string_view::npos)
            return;
        const auto slash = t.find('/');
        if (slash == std::string_view::npos || slash == 0)
            return;
        out.emplace(t.substr(0, slash));
    });
    return out;
}

std::set<std::string, std::less<>> ParseColonComponents(std::string_view value)
{
    std::set<std::string, std::less<>> out;
    value = Trim(value);
    if (value.empty() || value == "null")
        return out;
    size_t s = 0;
    while (s <= value.size()) {
        const auto c = value.find(':', s);
        const auto item = Trim(value.substr(s, c == std::string_view::npos ? std::string_view::npos : c - s));
        if (const auto slash = item.find('/'); slash != std::string_view::npos && slash > 0)
            out.emplace(item.substr(0, slash));
        if (c == std::string_view::npos)
            break;
        s = c + 1;
    }
    return out;
}

std::set<std::string, std::less<>> ParseDeviceAdmins(std::string_view text)
{
    std::set<std::string, std::less<>> out;
    bool in = false;
    size_t headerIndent = 0;
    ForEachLine(text, [&](std::string_view line) {
        const auto t = Trim(line);
        if (t.empty())
            return;
        const size_t ind = Indent(line);
        if (t.starts_with("Enabled Device Admins")) {
            in = true;
            headerIndent = ind;
            return;
        }
        if (!in)
            return;
        if (ind <= headerIndent) {
            in = false;
            return;
        }
        // "com.pkg/.Receiver:" lines; nested "testOnlyAdmin=false" lines are deeper.
        if (ind == headerIndent + 2 && t.back() == ':') {
            if (const auto slash = t.find('/'); slash != std::string_view::npos && slash > 0)
                out.emplace(t.substr(0, slash));
        }
    });
    return out;
}

std::map<std::string, DiskSizes, std::less<>> ParseDiskStats(std::string_view text)
{
    std::map<std::string, DiskSizes, std::less<>> out;
    auto array = [&](std::string_view key) -> nlohmann::json {
        const auto p = text.find(key);
        if (p == std::string_view::npos)
            return nlohmann::json::array();
        const auto open = text.find('[', p);
        const auto close = text.find(']', open);
        if (open == std::string_view::npos || close == std::string_view::npos)
            return nlohmann::json::array();
        try {
            return nlohmann::json::parse(text.substr(open, close - open + 1));
        } catch (const nlohmann::json::exception&) {
            return nlohmann::json::array();
        }
    };
    const auto names = array("Package Names:");
    const auto app = array("App Sizes:");
    const auto data = array("App Data Sizes:");
    const auto cache = array("Cache Sizes:");
    for (size_t i = 0; i < names.size(); ++i) {
        if (!names[i].is_string())
            continue;
        DiskSizes d;
        auto num = [&](const nlohmann::json& a) -> uint64_t {
            return i < a.size() && a[i].is_number_integer() ? a[i].get<uint64_t>() : 0;
        };
        d.app = num(app);
        d.data = num(data);
        d.cache = num(cache);
        out[names[i].get<std::string>()] = d;
    }
    return out;
}

std::string ParseBadgingLabel(std::string_view text)
{
    std::string label;
    ForEachLine(text, [&](std::string_view line) {
        if (!label.empty())
            return;
        // Prefer the plain default label; "application-label-xx:" are translations.
        if (line.starts_with("application-label:'")) {
            auto v = line.substr(19);
            if (!v.empty() && v.back() == '\'')
                v.remove_suffix(1);
            label = std::string(v);
        }
    });
    if (label.empty()) {
        // Fall back to "application: label='X' icon=..." (older aapt output).
        ForEachLine(text, [&](std::string_view line) {
            if (!label.empty() || !line.starts_with("application: "))
                return;
            const auto p = line.find("label='");
            if (p == std::string_view::npos)
                return;
            const auto e = line.find('\'', p + 7);
            if (e != std::string_view::npos)
                label = std::string(line.substr(p + 7, e - p - 7));
        });
    }
    // aapt escapes quotes as \'
    std::string out;
    for (size_t i = 0; i < label.size(); ++i) {
        if (label[i] == '\\' && i + 1 < label.size() && label[i + 1] == '\'')
            continue;
        out.push_back(label[i]);
    }
    return std::string(Trim(out));
}

void ComputeFlags(AppInfo& app, const RiskContext& ctx)
{
    auto in = [&](const std::set<std::string, std::less<>>* s) { return s && s->contains(app.package); };
    auto granted = [&](std::string_view perm) { return std::ranges::binary_search(app.grantedPermissions, perm); };

    uint32_t f = 0;
    if (in(ctx.accessibility))
        f |= AppFlag_Accessibility;
    if (in(ctx.deviceAdmins))
        f |= AppFlag_DeviceAdmin;
    if (in(ctx.notifListeners))
        f |= AppFlag_NotifListener;
    if (granted("android.permission.READ_SMS") || granted("android.permission.RECEIVE_SMS") || granted("android.permission.SEND_SMS"))
        f |= AppFlag_Sms;
    if (granted("android.permission.READ_CALL_LOG") || granted("android.permission.WRITE_CALL_LOG"))
        f |= AppFlag_CallLog;
    if (!app.system) {
        if (InstallerName(app) != "Preinstalled" && !IsStoreInstaller(app.installer))
            f |= AppFlag_Sideloaded;
        if (ctx.launcher && !in(ctx.launcher))
            f |= AppFlag_NoIcon;
    }
    if (granted("android.permission.SYSTEM_ALERT_WINDOW"))
        f |= AppFlag_Overlay;
    if (granted("android.permission.REQUEST_INSTALL_PACKAGES"))
        f |= AppFlag_InstallPackages;
    // Before Android 10 any location grant also works in the background.
    if (granted("android.permission.ACCESS_BACKGROUND_LOCATION")
        || (ctx.deviceSdk > 0 && ctx.deviceSdk < 29
            && (granted("android.permission.ACCESS_FINE_LOCATION") || granted("android.permission.ACCESS_COARSE_LOCATION"))))
        f |= AppFlag_BgLocation;
    if (in(ctx.boot))
        f |= AppFlag_Boot;
    if (!app.enabled)
        f |= AppFlag_Disabled;

    app.flags = f;
    app.risk = 0;
    for (const auto& fi : AllFlags())
        if (f & fi.flag)
            app.risk += fi.weight;
}

} // namespace ps
