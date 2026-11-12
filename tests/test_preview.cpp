#include "cleanup/Preview.h"

#include <doctest/doctest.h>

#include <ostream>

using namespace ps;

TEST_CASE("PreviewKindFor picks the right renderer")
{
    auto make = [](const char* path, Category c) {
        FileEntry e;
        e.path = path;
        e.nameOffset = static_cast<uint32_t>(std::string_view(path).rfind('/') + 1);
        e.category = c;
        return e;
    };
    CHECK(PreviewKindFor(make("/a/IMG_1.jpg", Category::Images)) == PreviewKind::Image);
    CHECK(PreviewKindFor(make("/a/VID_1.mp4", Category::Videos)) == PreviewKind::Video);
    CHECK(PreviewKindFor(make("/a/song.mp3", Category::Music)) == PreviewKind::Audio);
    CHECK(PreviewKindFor(make("/a/notes.TXT", Category::Documents)) == PreviewKind::Text);
    CHECK(PreviewKindFor(make("/a/data.json", Category::Other)) == PreviewKind::Text);
    CHECK(PreviewKindFor(make("/a/report.pdf", Category::Documents)) == PreviewKind::Other);
    CHECK(PreviewKindFor(make("/a/noext", Category::Other)) == PreviewKind::Other);
    CHECK_FALSE(IsTextName("archive.txt.zip"));
    CHECK(IsTextName("subs.SRT"));
}

TEST_CASE("DecodeTextPreview")
{
    SUBCASE("plain UTF-8 with CRLF and tabs")
    {
        const auto t = DecodeTextPreview("hello\r\n\tworld \xC3\xA9t\xC3\xA9\r\n");
        REQUIRE(t);
        CHECK(*t == "hello\n    world \xC3\xA9t\xC3\xA9\n");
    }
    SUBCASE("UTF-8 BOM is dropped")
    {
        CHECK(DecodeTextPreview("\xEF\xBB\xBFhi") == std::string("hi"));
    }
    SUBCASE("UTF-16 LE and BE")
    {
        CHECK(DecodeTextPreview(std::string("\xFF\xFEh\0i\0", 6)) == std::string("hi"));
        CHECK(DecodeTextPreview(std::string("\xFE\xFF\0h\0i", 6)) == std::string("hi"));
        // U+1F600 as a surrogate pair (LE): 3D D8 00 DE
        CHECK(DecodeTextPreview(std::string("\xFF\xFE\x3D\xD8\x00\xDE", 6)) == std::string("\xF0\x9F\x98\x80"));
    }
    SUBCASE("binary is refused")
    {
        CHECK_FALSE(DecodeTextPreview(std::string("PK\x03\x04\0\0", 6)));
        std::string noisy(200, 'a');
        for (int i = 0; i < 10; ++i)
            noisy[static_cast<size_t>(i) * 20] = '\x01';
        CHECK_FALSE(DecodeTextPreview(noisy));
    }
    SUBCASE("a multi-byte character cut by the size limit is dropped")
    {
        CHECK(DecodeTextPreview("ab\xE2\x82") == std::string("ab"));
    }
    SUBCASE("stray high bytes are read as Latin-1")
    {
        CHECK(DecodeTextPreview("caf\xE9!") == std::string("caf\xC3\xA9!"));
    }
    SUBCASE("line cap")
    {
        CHECK(DecodeTextPreview("1\n2\n3\n4\n", 2) == std::string("1\n2\n"));
    }
    SUBCASE("empty file is empty text")
    {
        CHECK(DecodeTextPreview("") == std::string());
    }
}
