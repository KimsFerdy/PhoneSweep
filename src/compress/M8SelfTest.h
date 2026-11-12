#pragma once

namespace ps {

// Dev/QA: pushes throwaway photos and a video into PhoneSweepSelfTest/m8/ on internal
// storage, runs the real compress (all three outcomes) and restore code on exactly
// those files, checks sizes, dates, EXIF and MD5s, then removes everything it made.
// Returns 0 on success. Run with: PhoneSweep.exe --m8-selftest
int RunM8SelfTest();

} // namespace ps
