#pragma once
#include "scan/AppModel.h"

#include <vector>

namespace ps {

// Demo app list for --fake runs: well-known apps plus a few made-up suspicious ones,
// with flags computed by the real risk rules.
std::vector<AppInfo> FakeApps();

} // namespace ps
