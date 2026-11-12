#pragma once
#include "scan/AppModel.h"

#include <span>
#include <string>

namespace ps {

struct AppListOptions {
    bool fullPermissions = true; // false: only sensitive permissions
};

// The plain-text applist format meant for pasting into any AI assistant (build spec M6):
//
//   package: com.example.thing
//   label: Thing
//   installer: sideloaded
//   version: 2.3.1
//   installed: 2025-08-14
//   size: 143 MB
//   launcher_icon: no
//   flags: SIDELOADED, NO_ICON, ACCESSIBILITY, SMS
//   permissions: android.permission.READ_SMS, ...
//
// One block per app, blocks separated by a blank line. Stable field order.
std::string FormatAppBlock(const AppInfo& app, const AppListOptions& opts = {});
std::string FormatAppList(std::span<const AppInfo* const> apps, const AppListOptions& opts = {});

// Short installer token for reports: play_store, galaxy_store, sideloaded, preinstalled, via:<pkg>.
std::string InstallerToken(const AppInfo& app);

} // namespace ps
