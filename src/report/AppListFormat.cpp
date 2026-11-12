#include "report/AppListFormat.h"

#include "util/Strings.h"

#include <format>

namespace ps {

std::string InstallerToken(const AppInfo& app)
{
    const std::string name = InstallerName(app);
    if (name == "Play Store") return "play_store";
    if (name == "Galaxy Store") return "galaxy_store";
    if (name == "Amazon Appstore") return "amazon_appstore";
    if (name == "AppGallery") return "appgallery";
    if (name == "F-Droid") return "fdroid";
    if (name == "Xiaomi GetApps") return "xiaomi_getapps";
    if (name == "OPPO App Market") return "oppo_market";
    if (name == "vivo App Store") return "vivo_store";
    if (name == "Sideloaded") return "sideloaded";
    if (name == "Preinstalled") return "preinstalled";
    return "via:" + app.installer;
}

std::string FormatAppBlock(const AppInfo& app, const AppListOptions& opts)
{
    std::string flags;
    for (const auto& fi : AllFlags()) {
        if (!(app.flags & fi.flag))
            continue;
        if (!flags.empty())
            flags += ", ";
        flags += fi.name;
    }

    std::string perms;
    for (const auto& p : app.grantedPermissions) {
        if (!opts.fullPermissions && !IsSensitivePermission(p))
            continue;
        if (!perms.empty())
            perms += ", ";
        perms += p;
    }

    std::string out;
    out += std::format("package: {}\n", app.package);
    out += std::format("label: {}\n", app.label.empty() ? "unknown" : app.label);
    out += std::format("installer: {}\n", InstallerToken(app));
    out += std::format("version: {}\n", app.versionName.empty() ? "unknown" : app.versionName);
    out += std::format("installed: {}\n", app.firstInstall.size() >= 10 ? app.firstInstall.substr(0, 10) : std::string("unknown"));
    out += std::format("size: {}\n", app.HasSize() ? FormatBytes(app.TotalBytes()) : std::string("unknown"));
    out += std::format("launcher_icon: {}\n", app.launcher ? "yes" : "no");
    out += std::format("flags: {}\n", flags.empty() ? "none" : flags);
    out += std::format("permissions: {}\n", perms.empty() ? "none" : perms);
    return out;
}

std::string FormatAppList(std::span<const AppInfo* const> apps, const AppListOptions& opts)
{
    std::string out;
    for (const AppInfo* a : apps) {
        if (!out.empty())
            out += "\n";
        out += FormatAppBlock(*a, opts);
    }
    return out;
}

} // namespace ps
