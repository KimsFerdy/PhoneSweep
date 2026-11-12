// Nuke eligibility: the tests that matter most (build spec).
#include "nuke/NukePlan.h"
#include "scan/AppModel.h"
#include "scan/Categorize.h"
#include "scan/FileStore.h"
#include "transfer/Manifest.h"

#include <doctest/doctest.h>

#include <ostream>

using namespace ps;

namespace {

NukeDecision D(bool keep, bool verified, bool prot, bool excluded, bool inclProt, ProtectorMode m)
{
    return DecideNuke({ keep, verified, prot, excluded, inclProt }, m);
}

} // namespace

TEST_CASE("Every combination of facts x mode (exhaustive truth table)")
{
    for (int mode = 0; mode < 2; ++mode) {
        const auto m = static_cast<ProtectorMode>(mode);
        for (int bits = 0; bits < 32; ++bits) {
            const bool keep = bits & 1, verified = bits & 2, prot = bits & 4, excluded = bits & 8, incl = bits & 16;
            CAPTURE(mode);
            CAPTURE(bits);
            const auto d = D(keep, verified, prot, excluded, incl, m);

            // Invariants that must hold no matter what:
            // a) unticked files are never deleted
            if (excluded)
                CHECK(d == NukeDecision::Excluded);
            // b) KEEP + not saved is never deleted
            if (keep && !verified)
                CHECK(d != NukeDecision::Delete);
            // c) protected files are never deleted in Prompt mode, nor in Free unless ticked
            if (prot && (m == ProtectorMode::Prompt || !incl))
                CHECK(d != NukeDecision::Delete);
            // d) Prompt mode never deletes anything that isn't verified
            if (m == ProtectorMode::Prompt && !verified)
                CHECK(d != NukeDecision::Delete);
            // e) a verified, unprotected, ticked file is always deleted (both modes)
            if (verified && !prot && !excluded)
                CHECK(d == NukeDecision::Delete);
        }
    }
}

TEST_CASE("Prompt mode: only saved + verified files are deleted")
{
    const auto P = ProtectorMode::Prompt;
    CHECK(D(false, true, false, false, false, P) == NukeDecision::Delete);
    CHECK(D(false, false, false, false, false, P) == NukeDecision::NotSaved);
    CHECK(D(true, false, false, false, false, P) == NukeDecision::KeepUnsaved);
    CHECK(D(true, true, false, false, false, P) == NukeDecision::Delete); // KEEP + saved = safe to delete
    CHECK(D(false, true, true, false, false, P) == NukeDecision::Protected);
    CHECK(D(false, true, true, false, true, P) == NukeDecision::Protected); // ticking protected does nothing in Prompt
}

TEST_CASE("Free mode: everything in the plan goes, except KEEP-unsaved and unticked protected")
{
    const auto F = ProtectorMode::Free;
    CHECK(D(false, false, false, false, false, F) == NukeDecision::Delete); // not saved: deleted forever
    CHECK(D(false, true, false, false, false, F) == NukeDecision::Delete);
    CHECK(D(true, false, false, false, false, F) == NukeDecision::KeepUnsaved);
    CHECK(D(false, false, true, false, false, F) == NukeDecision::Protected);
    CHECK(D(false, false, true, false, true, F) == NukeDecision::Delete); // explicitly ticked
    CHECK(D(true, false, true, false, true, F) == NukeDecision::KeepUnsaved); // KEEP still wins
    CHECK(D(false, false, false, true, false, F) == NukeDecision::Excluded);
}

namespace {

FileStore MakeStore()
{
    FileStore s;
    std::vector<FileEntry> b(6);
    const char* paths[] = {
        "/storage/emulated/0/DCIM/Camera/saved.jpg",       // 0 verified
        "/storage/emulated/0/Download/junk.apk",           // 1 not saved
        "/storage/emulated/0/Documents/keep_unsaved.pdf",  // 2 KEEP, not saved
        "/storage/emulated/0/Android/data/x/cache.bin",    // 3 protected
        "/storage/0000-0000/DCIM/Camera/sd_saved.jpg",     // 4 SD card, verified
        "/storage/emulated/0/DCIM/Camera/changed.jpg",     // 5 verified earlier, changed since (mtime differs)
    };
    for (size_t i = 0; i < b.size(); ++i) {
        b[i].path = paths[i];
        b[i].size = 100 * (i + 1);
        b[i].mtime = 1000;
        b[i].volume = i == 4 ? 1 : 0;
        Classify(b[i], i == 4 ? "/storage/0000-0000/" : "/storage/emulated/0/");
    }
    b[2].flags = FileFlag_Keep;
    s.SetVolumes({ { "/storage/emulated/0/", "Internal" }, { "/storage/0000-0000/", "SD card", true } });
    s.Append(std::move(b));
    return s;
}

VerifiedIndex MakeVerified()
{
    VerifiedIndex v;
    auto add = [&](const char* p, uint64_t size, int64_t mtime) {
        ManifestEntry e;
        e.devicePath = p;
        e.size = size;
        e.mtime = mtime;
        v.Add(e, "C:/backup");
    };
    add("/storage/emulated/0/DCIM/Camera/saved.jpg", 100, 1000);
    add("/storage/0000-0000/DCIM/Camera/sd_saved.jpg", 500, 1000);
    add("/storage/emulated/0/DCIM/Camera/changed.jpg", 600, 999); // file was edited after the backup
    return v;
}

} // namespace

TEST_CASE("BuildNukePlan: Prompt mode on a realistic store (SD card treated like internal)")
{
    const FileStore s = MakeStore();
    const VerifiedIndex v = MakeVerified();
    const auto plan = BuildNukePlan(s, v, {}, {}, {}, ProtectorMode::Prompt);
    CHECK(plan.del.files == std::vector<uint32_t>{ 0, 4 });     // internal + SD, both verified
    CHECK(plan.notSaved.files == std::vector<uint32_t>{ 1, 5 }); // changed-since-backup counts as NOT saved
    CHECK(plan.keepUnsaved.files == std::vector<uint32_t>{ 2 });
    CHECK(plan.protectedFiles.files == std::vector<uint32_t>{ 3 });
    CHECK(plan.HasBlockers());
    CHECK(plan.del.bytes == 100 + 500);
}

TEST_CASE("BuildNukePlan: Free mode, unticking and ticking protected")
{
    const FileStore s = MakeStore();
    const VerifiedIndex v = MakeVerified();
    NukeOverrides ov;
    ov.excludedPaths.insert("/storage/emulated/0/Download/junk.apk");
    ov.includedProtectedPaths.insert("/storage/emulated/0/Android/data/x/cache.bin");
    const auto plan = BuildNukePlan(s, v, {}, {}, ov, ProtectorMode::Free);
    CHECK(plan.del.files == std::vector<uint32_t>{ 0, 3, 4, 5 });
    CHECK(plan.excluded.files == std::vector<uint32_t>{ 1 });
    CHECK(plan.keepUnsaved.files == std::vector<uint32_t>{ 2 }); // KEEP still protects in Free mode
    CHECK_FALSE(plan.HasBlockers());                              // ...but doesn't block it
}

TEST_CASE("BuildNukePlan: only user apps flagged and still ticked get uninstalled")
{
    std::vector<AppInfo> apps(3);
    apps[0].package = "com.evil";
    apps[1].package = "com.unticked";
    apps[2].package = "com.android.systemthing";
    apps[2].system = true;
    const std::set<std::string, std::less<>> flagged = { "com.evil", "com.unticked", "com.android.systemthing" };
    NukeOverrides ov;
    ov.excludedApps.insert("com.unticked");
    const auto plan = BuildNukePlan(FileStore{}, VerifiedIndex{}, apps, flagged, ov, ProtectorMode::Prompt);
    CHECK(plan.uninstall == std::vector<std::string>{ "com.evil" }); // system apps never
}

TEST_CASE("Plan fingerprint changes when the plan changes")
{
    const FileStore s = MakeStore();
    const VerifiedIndex v = MakeVerified();
    const auto a = BuildNukePlan(s, v, {}, {}, {}, ProtectorMode::Prompt);
    const auto b = BuildNukePlan(s, v, {}, {}, {}, ProtectorMode::Free);
    CHECK(a.Fingerprint() == BuildNukePlan(s, v, {}, {}, {}, ProtectorMode::Prompt).Fingerprint());
    CHECK(a.Fingerprint() != b.Fingerprint());
}

TEST_CASE("CandidateEmptyDirs: deepest first, never top-level / Android / camera")
{
    const std::vector<std::string> roots = { "/storage/emulated/0/", "/storage/0000-0000/" };
    const auto dirs = CandidateEmptyDirs(
        {
            "/storage/emulated/0/WhatsApp/Media/WhatsApp Images/Sent/a.jpg",
            "/storage/emulated/0/DCIM/Camera/b.jpg",
            "/storage/emulated/0/Download/c.apk",
            "/storage/emulated/0/Android/media/com.whatsapp/x.jpg",
            "/storage/0000-0000/Music/Playlist 2/d.mp3",
        },
        roots);
    CHECK(dirs == std::vector<std::string>{
                      "/storage/emulated/0/WhatsApp/Media/WhatsApp Images/Sent",
                      "/storage/emulated/0/WhatsApp/Media/WhatsApp Images",
                      "/storage/emulated/0/WhatsApp/Media",
                      "/storage/0000-0000/Music/Playlist 2",
                  });
}

TEST_CASE("SqlQuote")
{
    CHECK(SqlQuote("/sdcard/a.jpg") == "'/sdcard/a.jpg'");
    CHECK(SqlQuote("/sdcard/it's.jpg") == "'/sdcard/it''s.jpg'");
}
