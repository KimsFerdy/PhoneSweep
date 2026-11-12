#include "cleanup/Cleanup.h"
#include "scan/Categorize.h"
#include "scan/FileStore.h"
#include "transfer/Manifest.h"

#include <doctest/doctest.h>

#include <ostream>

using namespace ps;

namespace {

FileEntry Make(const char* path, uint64_t size, int64_t mtime = 1000, uint8_t volume = 0)
{
    FileEntry e;
    e.path = path;
    e.size = size;
    e.mtime = mtime;
    e.volume = volume;
    Classify(e, volume ? "/storage/0000-0000/" : "/storage/emulated/0/");
    return e;
}

} // namespace

TEST_CASE("SizeCandidates: same size, >= minSize, never protected")
{
    FileStore s;
    s.Append({ Make("/storage/emulated/0/a.jpg", 5000), Make("/storage/emulated/0/b.jpg", 5000), Make("/storage/emulated/0/c.jpg", 7000),
        Make("/storage/emulated/0/Android/data/x/d.jpg", 7000), Make("/storage/emulated/0/e.jpg", 100), Make("/storage/emulated/0/f.jpg", 100) });
    const auto g = SizeCandidates(s, 1000);
    REQUIRE(g.size() == 1);
    CHECK(g[0] == std::vector<uint32_t>{ 0, 1 });
}

TEST_CASE("ChooseKeeper prefers KEEP, then camera, then non-messaging, then internal, then oldest")
{
    FileStore s;
    s.Append({
        Make("/storage/emulated/0/WhatsApp/Media/WhatsApp Images/Sent/IMG-1.jpg", 10, 500),
        Make("/storage/emulated/0/DCIM/Camera/IMG_1.jpg", 10, 900),
        Make("/storage/emulated/0/Pictures/copy.jpg", 10, 100),
    });
    CHECK(ChooseKeeper({ 0, 1, 2 }, s) == 1); // camera original wins
    CHECK(ChooseKeeper({ 0, 2 }, s) == 1);    // non-messaging beats WhatsApp "Sent"

    FileStore k;
    std::vector<FileEntry> b = { Make("/storage/emulated/0/DCIM/Camera/a.jpg", 10), Make("/storage/emulated/0/Download/a.jpg", 10) };
    b[1].flags = FileFlag_Keep;
    k.Append(std::move(b));
    CHECK(ChooseKeeper({ 0, 1 }, k) == 1); // KEEP beats camera

    FileStore v;
    v.Append({ Make("/storage/0000-0000/Pictures/a.jpg", 10, 100, 1), Make("/storage/emulated/0/Pictures/a.jpg", 10, 900) });
    CHECK(ChooseKeeper({ 0, 1 }, v) == 1); // internal beats SD card

    FileStore o;
    o.Append({ Make("/storage/emulated/0/Pictures/new.jpg", 10, 900), Make("/storage/emulated/0/Pictures/old.jpg", 10, 100) });
    CHECK(ChooseKeeper({ 0, 1 }, o) == 1); // oldest
}

TEST_CASE("DuplicateDeleteList never deletes the keeper, KEEP files, or every copy")
{
    FileStore s;
    std::vector<FileEntry> b = { Make("/storage/emulated/0/a/1.jpg", 10), Make("/storage/emulated/0/b/1.jpg", 10), Make("/storage/emulated/0/c/1.jpg", 10),
        Make("/storage/emulated/0/x/2.mp4", 99), Make("/storage/emulated/0/y/2.mp4", 99) };
    b[2].flags = FileFlag_Keep;
    s.Append(std::move(b));

    DupGroup g1;
    g1.size = 10;
    g1.files = { { "/storage/emulated/0/a/1.jpg", 10 }, { "/storage/emulated/0/b/1.jpg", 10 }, { "/storage/emulated/0/c/1.jpg", 10 } };
    g1.keep = 0;
    DupGroup g2;
    g2.size = 99;
    g2.files = { { "/storage/emulated/0/x/2.mp4", 99 }, { "/storage/emulated/0/y/2.mp4", 99 } };
    g2.keep = 1;
    DupGroup broken; // invalid keeper index: must be skipped entirely
    broken.files = { { "/storage/emulated/0/x/2.mp4", 99 }, { "/storage/emulated/0/y/2.mp4", 99 } };
    broken.keep = 7;

    const std::vector<DupGroup> groups = { g1, g2, broken };
    CHECK(DuplicateDeleteList(groups, { true, true, true }, s) == std::vector<std::string>{ "/storage/emulated/0/b/1.jpg", "/storage/emulated/0/x/2.mp4" });
    CHECK(DuplicateDeleteList(groups, { false, true, false }, s) == std::vector<std::string>{ "/storage/emulated/0/x/2.mp4" });

    // Property: for every group, at least one copy survives.
    for (int mask = 0; mask < 8; ++mask) {
        const std::vector<bool> t = { (mask & 1) != 0, (mask & 2) != 0, (mask & 4) != 0 };
        const auto del = DuplicateDeleteList(groups, t, s);
        for (const auto& g : groups) {
            size_t survivors = 0;
            for (const auto& f : g.files)
                survivors += std::ranges::find(del, f.path) == del.end();
            CHECK(survivors >= 1);
        }
    }
}

TEST_CASE("ClassifyJunk")
{
    const std::set<std::string, std::less<>> installed = { "com.whatsapp", "com.spotify.music" };
    auto kind = [&](const char* p, uint64_t size = 10) { return ClassifyJunk(Make(p, size), installed); };
    CHECK(kind("/storage/emulated/0/DCIM/.thumbnails/123.jpg") == JunkKind::Thumbnails);
    CHECK(kind("/storage/emulated/0/Android/data/com.spotify.music/cache/x.bin") == JunkKind::AppCaches);
    CHECK_FALSE(kind("/storage/emulated/0/Android/data/com.spotify.music/files/offline.bin")); // installed app's real data
    CHECK(kind("/storage/emulated/0/Android/data/com.gone.app/files/x.bin") == JunkKind::Leftovers);
    CHECK(kind("/storage/emulated/0/Android/obb/com.old.game/main.obb") == JunkKind::Leftovers);
    CHECK(kind("/storage/emulated/0/WhatsApp/Media/.Statuses/a.jpg") == JunkKind::WhatsAppStatuses);
    CHECK(kind("/storage/emulated/0/log/app.log") == JunkKind::LogsTemp);
    CHECK(kind("/storage/emulated/0/Xender/.cache/x") == JunkKind::LogsTemp);
    CHECK(kind("/storage/emulated/0/WhatsApp/Databases/msgstore-2026-11-10.1.db.crypt14") == JunkKind::OldChatBackups);
    CHECK_FALSE(kind("/storage/emulated/0/WhatsApp/Databases/msgstore.db.crypt14"));                       // newest: never
    CHECK_FALSE(kind("/storage/emulated/0/WhatsApp/Databases/msgstore-increment-2-2026-11-12.1.db.crypt14")); // incremental chain: never
    CHECK(kind("/storage/emulated/0/Download/app-release.apk") == JunkKind::Installers);
    CHECK(kind("/storage/emulated/0/Documents/empty.txt", 0) == JunkKind::EmptyFiles);
    CHECK_FALSE(kind("/storage/emulated/0/DCIM/.nomedia", 0)); // marker files stay
    CHECK_FALSE(kind("/storage/emulated/0/DCIM/Camera/IMG_1.jpg"));
}

TEST_CASE("BuildJunk honours KEEP and the protector mode")
{
    FileStore s;
    std::vector<FileEntry> b = { Make("/storage/emulated/0/DCIM/.thumbnails/1.jpg", 10), Make("/storage/emulated/0/Download/a.apk", 20),
        Make("/storage/emulated/0/Download/b.apk", 30) };
    b[2].flags = FileFlag_Keep;
    s.Append(std::move(b));
    VerifiedIndex v;
    const std::set<std::string, std::less<>> installed;

    const auto prompt = BuildJunk(s, v, installed, ProtectorMode::Prompt);
    const auto& thumbs = prompt[static_cast<size_t>(JunkKind::Thumbnails)];
    const auto& apks = prompt[static_cast<size_t>(JunkKind::Installers)];
    CHECK(thumbs.files.size() == 1);     // safe junk: allowed in prompt mode
    CHECK(apks.files.empty());           // review junk, not saved: blocked in prompt mode
    CHECK(apks.blocked.size() == 1);     // a.apk (b.apk is KEEP: not junk at all)

    ManifestEntry m;
    m.devicePath = "/storage/emulated/0/Download/a.apk";
    m.size = 20;
    m.mtime = 1000;
    v.Add(m, "C:/backup");
    CHECK(BuildJunk(s, v, installed, ProtectorMode::Prompt)[static_cast<size_t>(JunkKind::Installers)].files.size() == 1); // saved: allowed

    const auto free = BuildJunk(s, VerifiedIndex{}, installed, ProtectorMode::Free);
    CHECK(free[static_cast<size_t>(JunkKind::Installers)].files.size() == 1);
    CHECK(free[static_cast<size_t>(JunkKind::Installers)].blocked.empty());
}
