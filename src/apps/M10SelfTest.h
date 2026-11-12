#pragma once
#include <filesystem>

namespace ps {

// Dev/QA: checks live CPU/RAM (top) and battery (batterystats) parsing against the
// real phone, then installs the given throwaway APK (com.phonesweep.selftest) twice
// and removes it with Quick and Full uninstall, checking the APK backup and folders.
// Run with: PhoneSweep.exe --m10-selftest[=<test.apk>]
int RunM10SelfTest(const std::filesystem::path& testApk);

} // namespace ps
