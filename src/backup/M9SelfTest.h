#pragma once
#include <filesystem>

namespace ps {

// Dev/QA: switches the USB phone to Wi-Fi (adb tcpip), checks it shows up with the
// same hardware serial, pushes + pulls a throwaway file over Wi-Fi, then puts the
// phone back to USB-only. If `backupCopySource` is a backup folder, a COPY of it is
// zipped (verified, copy's folder removed) and a restore plan is read from the zip
// (read-only on the phone). Run with: PhoneSweep.exe --m9-selftest[=<backup folder>]
int RunM9SelfTest(const std::filesystem::path& backupCopySource);

} // namespace ps
