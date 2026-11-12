#include "scan/Categorize.h"
#include "scan/FileStore.h"

#include <doctest/doctest.h>

#include <ostream> // doctest needs a complete ostream to print string_view on failure

using namespace ps;

TEST_CASE("MIME wins over extension")
{
    CHECK(Categorize("photo.bin", "image/jpeg") == Category::Images);
    CHECK(Categorize("clip.dat", "video/mp4") == Category::Videos);
    CHECK(Categorize("note.xyz", "audio/ogg") == Category::Music);
    CHECK(Categorize("thing", "application/vnd.android.package-archive") == Category::Archives);
    CHECK(Categorize("report", "application/pdf") == Category::Documents);
    CHECK(Categorize("sheet", "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet") == Category::Documents);
}

TEST_CASE("Generic or missing MIME falls back to the extension")
{
    CHECK(Categorize("IMG_1.JPG", "") == Category::Images);
    CHECK(Categorize("IMG_1.jpg", "NULL") == Category::Images);
    CHECK(Categorize("x.apk", "application/octet-stream") == Category::Archives);
    CHECK(Categorize("PTT-2026-WA0007.opus", "") == Category::Music);
    CHECK(Categorize("Facture_Électricité.pdf", "") == Category::Documents);
    CHECK(Categorize("kmSpiko.rar", "") == Category::Archives);
}

TEST_CASE("Extension edge cases")
{
    CHECK(CategoryFromName("noextension") == Category::Other);
    CHECK(CategoryFromName(".nomedia") == Category::Other);   // dotfile, not an extension
    CHECK(CategoryFromName("trailingdot.") == Category::Other);
    CHECK(CategoryFromName("archive.tar.gz") == Category::Archives);
    CHECK(CategoryFromName("Mbóka – épisode 1.MKV") == Category::Videos);
    CHECK(CategoryFromName("data.bin") == Category::Other);
}

TEST_CASE("Source from path")
{
    CHECK(SourceFromPath("DCIM/Camera/IMG_1.jpg") == Source::Camera);
    CHECK(SourceFromPath("DCIM/Screenshots/a.png") == Source::Screenshots);
    CHECK(SourceFromPath("Pictures/Screenshots/a.png") == Source::Screenshots);
    CHECK(SourceFromPath("WhatsApp/Media/WhatsApp Images/IMG-1.jpg") == Source::WhatsApp);
    CHECK(SourceFromPath("Android/media/com.whatsapp/WhatsApp/Media/x.jpg") == Source::WhatsApp);
    CHECK(SourceFromPath("TMWhatsApp/Media/x.jpg") == Source::WhatsApp); // mods count as WhatsApp
    CHECK(SourceFromPath("WhatsApp Business/Media/x.jpg") == Source::WhatsAppBusiness);
    CHECK(SourceFromPath("Android/media/com.whatsapp.w4b/WhatsApp Business/x.jpg") == Source::WhatsAppBusiness);
    CHECK(SourceFromPath("Telegram/Telegram Video/v.mp4") == Source::Telegram);
    CHECK(SourceFromPath("Download/Telegram/v.mp4") == Source::Telegram);
    CHECK(SourceFromPath("Download/Bluetooth/f.jpg") == Source::Bluetooth);
    CHECK(SourceFromPath("Download/file.pdf") == Source::Downloads);
    CHECK(SourceFromPath("Voice Recorder/rec.m4a") == Source::Recordings);
    CHECK(SourceFromPath("Snapchat/x.mp4") == Source::Snapchat);
    CHECK(SourceFromPath("Music/song.mp3") == Source::Other);
    CHECK(SourceFromPath("whatsapp.txt") == Source::Other); // a root file, not a folder
}

TEST_CASE("Protection: Android/data, obb and dot-folders")
{
    CHECK(ProtectionFromPath("Android/data/com.app/files/a.bin") == Protection::AndroidData);
    CHECK(ProtectionFromPath("Android/obb/com.game/main.obb") == Protection::AndroidObb);
    CHECK(ProtectionFromPath(".thumbnails/123.jpg") == Protection::Hidden);
    CHECK(ProtectionFromPath("Xender/.cache/.nomedia") == Protection::Hidden);
    CHECK(ProtectionFromPath("DCIM/.nomedia") == Protection::None); // dot FILE in a normal folder
    CHECK(ProtectionFromPath(".mn_1561123885") == Protection::None);  // dot file at the root
    CHECK(ProtectionFromPath("Android/media/com.whatsapp/x.jpg") == Protection::None); // WhatsApp media is NOT protected
    CHECK(ProtectionFromPath("DCIM/Camera/IMG.jpg") == Protection::None);
}

TEST_CASE("Classify fills offsets and views")
{
    FileEntry e;
    e.path = "/storage/emulated/0/DCIM/Camera/IMG 1 (copy).jpg";
    Classify(e, "/storage/emulated/0/");
    CHECK(e.RelPath() == "DCIM/Camera/IMG 1 (copy).jpg");
    CHECK(e.Name() == "IMG 1 (copy).jpg");
    CHECK(e.RelFolder() == "DCIM/Camera");
    CHECK(e.category == Category::Images);
    CHECK(e.source == Source::Camera);

    FileEntry root;
    root.path = "/storage/emulated/0/ui.xml";
    Classify(root, "/storage/emulated/0/");
    CHECK(root.Name() == "ui.xml");
    CHECK(root.RelFolder().empty());
}

TEST_CASE("FileStore totals count space on disk (sparse files don't inflate them)")
{
    FileStore store;
    std::vector<FileEntry> batch(2);
    batch[0].path = "/storage/emulated/0/DCIM/.thumbnails/.thumbdata4-1";
    batch[0].size = 5'873'002'252ull;
    batch[0].diskBytes = 1'601'536; // sparse
    batch[1].path = "/storage/emulated/0/DCIM/Camera/a.jpg";
    batch[1].size = 1000; // disk size unknown -> falls back to size
    for (auto& e : batch)
        Classify(e, "/storage/emulated/0/");
    store.Append(std::move(batch));
    CHECK(store.Protected().bytes == 1'601'536);
    CHECK(store.All().bytes == 1000);
    CHECK(store.Files()[0].Disk() == 1'601'536);
    CHECK(store.Files()[1].Disk() == 1000);
}

TEST_CASE("FileStore totals exclude protected files and track selection/keep")
{
    FileStore store;
    std::vector<FileEntry> batch(3);
    batch[0].path = "/storage/emulated/0/DCIM/Camera/a.jpg";
    batch[0].size = 100;
    batch[1].path = "/storage/emulated/0/Android/data/x/b.bin";
    batch[1].size = 1000;
    batch[2].path = "/storage/emulated/0/Download/c.apk";
    batch[2].size = 10;
    for (auto& e : batch)
        Classify(e, "/storage/emulated/0/");
    store.Append(std::move(batch));

    CHECK(store.All().count == 2);
    CHECK(store.All().bytes == 110);
    CHECK(store.Protected().count == 1);
    CHECK(store.ByCategory(Category::Images).bytes == 100);
    CHECK(store.ByCategory(Category::Archives).count == 1);

    store.SetSelected(0, true);
    store.SetSelected(2, true);
    store.SetSelected(2, true); // idempotent
    CHECK(store.Selection().count == 2);
    CHECK(store.Selection().bytes == 110);
    store.SetSelected(0, false);
    CHECK(store.Selection().bytes == 10);

    store.SetKeep(0, true);
    CHECK(store.Kept().count == 1);
    CHECK((store.Files()[0].flags & FileFlag_Keep) != 0);

    // MediaStore says the .apk is actually a zip MIME -> still Archives; jpg -> video MIME moves it.
    std::vector<MediaRow> rows = { { "/storage/emulated/0/DCIM/Camera/a.jpg", "video/mp4" } };
    CHECK(store.ApplyMime(rows) == 1);
    CHECK(store.ByCategory(Category::Videos).bytes == 100);
    CHECK(store.ByCategory(Category::Images).count == 0);
}
