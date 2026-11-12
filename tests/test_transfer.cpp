#include "transfer/LocalPath.h"
#include "transfer/Manifest.h"
#include "util/Md5.h"
#include "util/Utf8.h"

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <ostream>

using namespace ps;

TEST_CASE("MD5 known vectors (RFC 1321)")
{
    CHECK(Md5Hex("") == "d41d8cd98f00b204e9800998ecf8427e");
    CHECK(Md5Hex("abc") == "900150983cd24fb0d6963f7d28e17f72");
    CHECK(Md5Hex("The quick brown fox jumps over the lazy dog") == "9e107d9d372bb6826bd81d3542a419d6");
    // Incremental == one-shot.
    Md5 m;
    m.Update(std::string_view("The quick brown "));
    m.Update(std::string_view("fox jumps over the lazy dog"));
    CHECK(m.HexFinal() == "9e107d9d372bb6826bd81d3542a419d6");
}

TEST_CASE("Md5File matches Md5Hex")
{
    const auto f = std::filesystem::temp_directory_path() / "ps_md5_test.bin";
    {
        std::ofstream out(f, std::ios::binary);
        out << "abc";
    }
    CHECK(Md5File(f) == std::optional<std::string>("900150983cd24fb0d6963f7d28e17f72"));
    std::filesystem::remove(f);
    CHECK_FALSE(Md5File(f).has_value());
}

TEST_CASE("SanitizeComponent makes NTFS-safe names")
{
    CHECK(SanitizeComponent("IMG_1.jpg") == "IMG_1.jpg");
    CHECK(SanitizeComponent("a:b*c?d\"e<f>g|h") == "a_b_c_d_e_f_g_h");
    CHECK(SanitizeComponent("back\\slash") == "back_slash");
    CHECK(SanitizeComponent("trailing dot.") == "trailing dot_");
    CHECK(SanitizeComponent("trailing space ") == "trailing space_");
    CHECK(SanitizeComponent("CON") == "_CON");
    CHECK(SanitizeComponent("nul.txt") == "_nul.txt");
    CHECK(SanitizeComponent("console.txt") == "console.txt");
    CHECK(SanitizeComponent("") == "_");
    CHECK(SanitizeComponent("..") == "._"); // trailing dot replaced: no longer a parent reference
    CHECK(SanitizeComponent(".") == "_");
    CHECK(SanitizeComponent("tab\there") == "tab_here");
    // Unicode survives, invalid bytes become '_'.
    CHECK(SanitizeComponent("Fally Ipupa – Éloko Oyo 😀.mp3") == "Fally Ipupa – Éloko Oyo 😀.mp3");
    CHECK(SanitizeComponent("d\xC3\xA8m\xCE\x98m\xE2\x8C\x90\xFE.x") == "d\xC3\xA8m\xCE\x98m\xE2\x8C\x90_.x");
    CHECK(SanitizeComponent("\xC0\xAF") == "__"); // overlong encoding rejected
}

TEST_CASE("MapToLocal preserves structure and prefixes the SD card")
{
    CHECK(MapToLocal("", "DCIM/Camera/a.jpg", true) == std::filesystem::path(L"DCIM\\Camera\\a.jpg"));
    CHECK(MapToLocal("SD card", "DCIM/Camera/a.jpg", true) == std::filesystem::path(L"SD card\\DCIM\\Camera\\a.jpg"));
    CHECK(MapToLocal("", "DCIM/Camera/a.jpg", false) == std::filesystem::path(L"a.jpg"));
    CHECK(MapToLocal("", "WhatsApp/Media/x:y.jpg", true) == std::filesystem::path(L"WhatsApp\\Media\\x_y.jpg"));
    // Path traversal can't escape the backup folder.
    const auto evil = MapToLocal("", "../../Windows/evil.dll", true);
    CHECK(evil == std::filesystem::path(L"._\\._\\Windows\\evil.dll"));
    for (const auto& part : evil)
        CHECK(part != std::filesystem::path(L".."));
}

TEST_CASE("NextFreeName")
{
    const auto dir = std::filesystem::temp_directory_path() / "ps_nfn_test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const auto a = dir / "a.jpg";
    CHECK(NextFreeName(a) == a);
    std::ofstream(a).put('x');
    CHECK(NextFreeName(a) == dir / "a (1).jpg");
    std::ofstream(dir / "a (1).jpg").put('x');
    CHECK(NextFreeName(a) == dir / "a (2).jpg");
    std::filesystem::remove_all(dir);
}

TEST_CASE("ExpandFolderPattern")
{
    // 2026-11-11 12:00 UTC; the date is local, so only check shape and device part.
    const std::string s = ExpandFolderPattern("{device}_{date}", "SM-G960F", 1'794'398'400);
    CHECK(s.starts_with("SM-G960F_2026-11-1"));
    CHECK(ExpandFolderPattern("backup:{device}", "A/B", 0).find(':') == std::string::npos);
}

TEST_CASE("Manifest round-trips, including non-UTF-8 device paths")
{
    ManifestEntry a;
    a.devicePath = "/storage/emulated/0/DCIM/Camera/IMG 1.jpg";
    a.size = 123;
    a.mtime = 1700000000;
    a.md5 = "900150983cd24fb0d6963f7d28e17f72";
    a.localRel = "DCIM/Camera/IMG 1.jpg";
    a.verifiedAt = "2026-11-11 10:00:00";
    ManifestEntry b;
    b.devicePath = "/storage/0000-0000/Music Audio/d\xE8m\x98.m4a"; // invalid UTF-8 from a FAT card
    b.size = 9;
    b.mtime = 1;
    b.localRel = "SD card/Music Audio/d_m_.m4a";

    const std::string json = Manifest::ToJson("serial1", "SM-G960F", { a, b });
    std::string serial;
    std::vector<ManifestEntry> back;
    REQUIRE(Manifest::FromJson(json, &serial, &back));
    CHECK(serial == "serial1");
    REQUIRE(back.size() == 2);
    CHECK(back[0].devicePath == a.devicePath);
    CHECK(back[0].md5 == a.md5);
    CHECK(back[0].Method() == "size+md5");
    CHECK(back[1].devicePath == b.devicePath); // exact raw bytes
    CHECK(back[1].Method() == "size");
    CHECK_FALSE(Manifest::FromJson("{\"files\":[]}", nullptr, nullptr)); // not our format
}

TEST_CASE("VerifiedIndex: verified means same size AND mtime")
{
    const auto root = std::filesystem::temp_directory_path() / "ps_vi_test";
    std::filesystem::remove_all(root);
    {
        Manifest m(root / "SM-G960F_2026-11-11" / "saved_manifest.json", "dev1", "SM-G960F");
        ManifestEntry e;
        e.devicePath = "/sdcard/a.jpg";
        e.size = 10;
        e.mtime = 100;
        e.localRel = "a.jpg";
        m.Upsert(e);
        REQUIRE(m.Save());
        Manifest other(root / "other_phone" / "saved_manifest.json", "dev2", "X");
        ManifestEntry o = e;
        o.devicePath = "/sdcard/b.jpg";
        other.Upsert(o);
        REQUIRE(other.Save());
    }
    VerifiedIndex vi;
    CHECK(vi.LoadForDevice(root, "dev1") == 1);
    CHECK(vi.IsVerified("/sdcard/a.jpg", 10, 100));
    CHECK_FALSE(vi.IsVerified("/sdcard/a.jpg", 11, 100)); // file grew since the backup
    CHECK_FALSE(vi.IsVerified("/sdcard/a.jpg", 10, 101)); // modified since the backup
    CHECK_FALSE(vi.IsVerified("/sdcard/b.jpg", 10, 100)); // another phone's manifest
    CHECK(vi.LocalFile("/sdcard/a.jpg") == root / "SM-G960F_2026-11-11" / "a.jpg");
    std::filesystem::remove_all(root);
}
