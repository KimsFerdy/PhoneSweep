#include "scan/ScanParse.h"

#include <doctest/doctest.h>

#include <ostream> // doctest needs a complete ostream to print string_view on failure

using namespace ps;

TEST_CASE("ParseStatLine: stat -c format")
{
    const auto r = ParseStatLine("239494\t1776018376\t/storage/emulated/0/WhatsApp/Media/x=.webp");
    REQUIRE(r);
    CHECK(r->size == 239494);
    CHECK(r->mtime == 1776018376);
    CHECK(r->path == "/storage/emulated/0/WhatsApp/Media/x=.webp");
}

TEST_CASE("ParseStatLine: find -printf with fractional mtime")
{
    const auto r = ParseStatLine("130\t1729902815.1234567890\t/storage/emulated/0/.mn_1561123885");
    REQUIRE(r);
    CHECK(r->mtime == 1729902815);
    CHECK(r->path == "/storage/emulated/0/.mn_1561123885");
}

TEST_CASE("ParseStatLine: paths with tabs, spaces, unicode, invalid UTF-8")
{
    auto r = ParseStatLine("5\t1\t/sdcard/a\tb c.txt");
    REQUIRE(r);
    CHECK(r->path == "/sdcard/a\tb c.txt");

    r = ParseStatLine("5\t1\t/sdcard/Musique/Fally Ipupa – Éloko Oyo 😀.mp3");
    REQUIRE(r);
    CHECK(r->path == "/sdcard/Musique/Fally Ipupa – Éloko Oyo 😀.mp3");

    // Raw non-UTF-8 bytes (seen on a FAT SD card) pass through untouched.
    r = ParseStatLine("5\t1\t/storage/0000-0000/x/\x80\xFF\xFE.bin");
    REQUIRE(r);
    CHECK(r->path == "/storage/0000-0000/x/\x80\xFF\xFE.bin");
}

TEST_CASE("ParseStatLine: allocated size (sparse files)")
{
    // stat -c '%s\t%b\t%B\t%Y\t%n': Samsung .thumbdata claims 5.8 GB, occupies 1.6 MB.
    auto r = ParseStatLine("5873002252\t3128\t512\t1794423526\t/storage/emulated/0/DCIM/.thumbnails/.thumbdata4--1967290299");
    REQUIRE(r);
    CHECK(r->size == 5873002252ull);
    CHECK(r->diskBytes == 3128ull * 512);
    CHECK(r->mtime == 1794423526);
    CHECK(r->path == "/storage/emulated/0/DCIM/.thumbnails/.thumbdata4--1967290299");

    // find -printf '%s\t%k\t%T@\t%p'
    r = ParseStatLine("82333\t84\t1789122271.5\t/sdcard/a b.jpg");
    REQUIRE(r);
    CHECK(r->diskBytes == 84 * 1024);
    CHECK(r->mtime == 1789122271);

    // Old two-field format still works; disk size unknown.
    r = ParseStatLine("10\t100\t/sdcard/x");
    REQUIRE(r);
    CHECK(r->diskBytes == 0);

    CHECK_FALSE(ParseStatLine("10\tx\t512\t100\t/sdcard/x"));
}

TEST_CASE("ParseStatLine rejects garbage")
{
    CHECK_FALSE(ParseStatLine(""));
    CHECK_FALSE(ParseStatLine("find: '/x': Permission denied"));
    CHECK_FALSE(ParseStatLine("abc\t1\t/x"));
    CHECK_FALSE(ParseStatLine("1\tabc\t/x"));
    CHECK_FALSE(ParseStatLine("1\t2\trelative/path"));
    CHECK_FALSE(ParseStatLine("1\t2\t"));
    CHECK_FALSE(ParseStatLine("1\t2.x\t/x"));
    CHECK_FALSE(ParseStatLine("-5\t2\t/x"));
}

TEST_CASE("StatStreamParser glues file names containing newlines")
{
    std::vector<StatRecord> out;
    StatStreamParser p([&](StatRecord&& r) { out.push_back(std::move(r)); });
    p.Feed("10\t100\t/sdcard/normal.jpg");
    p.Feed("20\t200\t/sdcard/weird");
    p.Feed("name.txt");
    p.Feed("30\t300\t/sdcard/after.jpg");
    p.Finish();
    REQUIRE(out.size() == 3);
    CHECK(out[1].path == "/sdcard/weird\nname.txt");
    CHECK(out[1].size == 20);
    CHECK(out[2].path == "/sdcard/after.jpg");
    CHECK(p.Glued() == 1);
    CHECK(p.Orphans() == 0);
}

TEST_CASE("StatStreamParser counts orphan lines")
{
    std::vector<StatRecord> out;
    StatStreamParser p([&](StatRecord&& r) { out.push_back(std::move(r)); });
    p.Feed("garbage before anything");
    p.Feed("");
    p.Feed("1\t1\t/a");
    p.Finish();
    CHECK(out.size() == 1);
    CHECK(p.Orphans() == 1);
}

TEST_CASE("ParseMediaStoreRow")
{
    auto r = ParseMediaStoreRow("Row: 99 _data=/storage/0000-0000/DCIM/Camera/20260428_210537.jpg, mime_type=image/jpeg");
    REQUIRE(r);
    CHECK(r->path == "/storage/0000-0000/DCIM/Camera/20260428_210537.jpg");
    CHECK(r->mime == "image/jpeg");

    // Commas inside the path must not confuse the parser.
    r = ParseMediaStoreRow("Row: 1 _data=/sdcard/Download/a, b, mime_type=c.pdf, mime_type=application/pdf");
    REQUIRE(r);
    CHECK(r->path == "/sdcard/Download/a, b, mime_type=c.pdf");
    CHECK(r->mime == "application/pdf");

    CHECK_FALSE(ParseMediaStoreRow("Row: 0 _data=/data, mime_type=NULL"));
    CHECK_FALSE(ParseMediaStoreRow("No result found."));
    CHECK_FALSE(ParseMediaStoreRow(""));
}

TEST_CASE("ParseStorageVolumes keeps only SD-style ids")
{
    const auto v = ParseStorageVolumes("0000-0000\nemulated\nenc_emulated\nself\n1A2B-3c4d\nZZZZ-0000\n12345-678\n");
    REQUIRE(v.size() == 2);
    CHECK(v[0] == "0000-0000");
    CHECK(v[1] == "1A2B-3c4d");
}
