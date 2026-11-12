#pragma once

namespace ps {

// Dev/QA: creates throwaway files in PhoneSweepSelfTest/ on internal storage and
// the SD card (awkward names included), runs the real RunNuke on exactly those
// paths, and checks they're gone and their folders cleaned. Touches nothing
// else. Returns 0 on success. Run with: PhoneSweep.exe --nuke-selftest
int RunNukeSelfTest();

} // namespace ps
