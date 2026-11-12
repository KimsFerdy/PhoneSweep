#include "compress/Compress.h"
#include "restore/Restore.h"
#include "scan/Categorize.h"
#include "scan/FileStore.h"
#include "thumbs/Exif.h"
#include "thumbs/ImageOps.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <ostream>

using namespace ps;

TEST_CASE("ParseMediaInfoRows (videos and images, commas in paths)")
{
    const auto m = ParseMediaInfoRows("Row: 0 _data=/storage/0000-0000/DCIM/Camera/a.mp4, duration=146970, width=3840, height=2160, _size=519508173\n"
                                      "Row: 1 _data=/sdcard/x, y.jpg, width=4032, height=1960, _size=1565349\n"
                                      "Row: 2 _data=NULL, width=1, height=1, _size=1\n");
    REQUIRE(m.size() == 2);
    const auto& v = m.at("/storage/0000-0000/DCIM/Camera/a.mp4");
    CHECK(v.durationMs == 146970);
    CHECK(v.width == 3840);
    CHECK(v.height == 2160);
    CHECK(m.at("/sdcard/x, y.jpg").width == 4032);
}

TEST_CASE("Video estimates: compress big H.264, skip already-efficient HEVC")
{
    // 4K camera clip: 519 MB / 147 s = 28 Mbps -> worth it, ~1080p at 4.5 Mbps.
    const auto big = EstimateVideo(519'508'173, { 146970, 3840, 2160 });
    REQUIRE(big);
    CHECK(*big < 519'508'173 / 4);
    // A Samsung 720p HEVC clip at ~5 Mbps: re-encoding to H.264 would GROW it (measured 87.5 -> 109.5 MB).
    CHECK_FALSE(EstimateVideo(91'773'218, { 139840, 1280, 720 }));
    CHECK_FALSE(EstimateVideo(5'000'000, { 60000, 1920, 1080 })); // small files aren't worth the round trip
    CHECK_FALSE(EstimateVideo(50'000'000, { 1000, 1920, 1080 }));  // < 3 s
    CHECK(TargetVideoKbps(3840, 2160) == 4500);
    CHECK(TargetVideoKbps(1280, 720) == 2500);
    CHECK(TargetVideoKbps(640, 480) == 1200);
}

TEST_CASE("Photo estimates")
{
    CHECK(EstimatePhoto(10'900'000, { 0, 4032, 3024 })); // ~7 bpp camera JPEG -> ~3.4 MB
    CHECK_FALSE(EstimatePhoto(1'000'000, { 0, 4032, 3024 })); // already small
    CHECK_FALSE(EstimatePhoto(3'000'000, { 0, 4032, 3024 })); // ~2 bpp: nothing to gain
}

TEST_CASE("FindCompressCandidates skips protected, KEEP and unknown files")
{
    FileStore s;
    std::vector<FileEntry> b(4);
    b[0].path = "/storage/emulated/0/DCIM/Camera/big.mp4";
    b[0].size = 519'508'173;
    b[1].path = "/storage/emulated/0/DCIM/Camera/keep.mp4";
    b[1].size = 519'508'173;
    b[1].flags = FileFlag_Keep;
    b[2].path = "/storage/emulated/0/DCIM/Camera/photo.jpg";
    b[2].size = 10'900'000;
    b[3].path = "/storage/emulated/0/DCIM/Camera/nomedia.mp4"; // not in MediaStore
    b[3].size = 519'508'173;
    for (auto& e : b)
        Classify(e, "/storage/emulated/0/");
    s.Append(std::move(b));
    std::map<std::string, MediaInfo, std::less<>> media = {
        { "/storage/emulated/0/DCIM/Camera/big.mp4", { 146970, 3840, 2160 } },
        { "/storage/emulated/0/DCIM/Camera/keep.mp4", { 146970, 3840, 2160 } },
        { "/storage/emulated/0/DCIM/Camera/photo.jpg", { 0, 4032, 3024 } },
    };
    const auto c = FindCompressCandidates(s, VerifiedIndex{}, media);
    REQUIRE(c.size() == 2);
    CHECK(c[0].path.ends_with("big.mp4")); // biggest saving first
    CHECK(c[0].kind == MediaKind::Video);
    CHECK(c[1].kind == MediaKind::Photo);
    CHECK_FALSE(c[0].saved);
}

TEST_CASE("VideoArgs: downscale 4K, keep metadata, pick encoder")
{
    auto has = [](const std::vector<std::string>& a, std::string_view s) { return std::ranges::find(a, s) != a.end(); };
    const auto x264 = VideoArgs("in.mp4", "out.mp4", { 1000, 3840, 2160 }, false);
    CHECK(has(x264, "libx264"));
    CHECK(has(x264, "26")); // CRF
    CHECK(has(x264, "-vf"));
    CHECK(has(x264, "-map_metadata"));
    CHECK(has(x264, "aac"));
    const auto qsv = VideoArgs("in.mp4", "out.mp4", { 1000, 1280, 720 }, true);
    CHECK(has(qsv, "h264_qsv"));
    CHECK_FALSE(has(qsv, "-vf")); // 720p needs no scaling
}

TEST_CASE("CompressJpeg keeps the EXIF block (orientation survives)")
{
    // Build a "camera" JPEG: noisy pixels (compresses badly) + EXIF with orientation 6.
    Image img;
    img.w = 256;
    img.h = 192;
    img.rgba.resize(static_cast<size_t>(img.w) * img.h * 4);
    uint32_t seed = 1;
    for (auto& b : img.rgba) {
        seed = seed * 1103515245 + 12345;
        b = static_cast<uint8_t>(seed >> 16);
    }
    const std::string plain = EncodeJpeg(img, 100);
    // Minimal EXIF APP1: II*\0, IFD0 with orientation=6, no IFD1.
    const unsigned char tiff[] = { 'I', 'I', 42, 0, 8, 0, 0, 0, 1, 0, 0x12, 0x01, 3, 0, 1, 0, 0, 0, 6, 0, 0, 0, 0, 0, 0, 0 };
    std::string app1 = "\xFF\xE1";
    const size_t len = 2 + 6 + sizeof(tiff);
    app1.push_back(static_cast<char>(len >> 8));
    app1.push_back(static_cast<char>(len & 0xFF));
    app1.append("Exif\0\0", 6);
    app1.append(reinterpret_cast<const char*>(tiff), sizeof(tiff));
    const std::string original = plain.substr(0, 2) + app1 + plain.substr(2);
    REQUIRE(ParseJpegExif({ reinterpret_cast<const uint8_t*>(original.data()), original.size() }).orientation == 6);

    const auto out = CompressJpeg(original, 60);
    REQUIRE(out);
    CHECK(out->size() < original.size());
    const auto exif = ParseJpegExif({ reinterpret_cast<const uint8_t*>(out->data()), out->size() });
    CHECK(exif.found);
    CHECK(exif.orientation == 6);
    CHECK(DecodeImage({ reinterpret_cast<const uint8_t*>(out->data()), out->size() }).has_value());

    // No EXIF in the original -> output is just the re-encode.
    CHECK(SpliceExif(plain, "\xFF\xD8xyz") == std::string("\xFF\xD8xyz"));
}

TEST_CASE("CompressedPhonePath")
{
    CHECK(CompressedPhonePath("/sdcard/a.mp4", MediaKind::Video) == "/sdcard/a.mp4");
    CHECK(CompressedPhonePath("/sdcard/a.MOV", MediaKind::Video) == "/sdcard/a.mp4");
    CHECK(CompressedPhonePath("/sdcard/a.b/c.3gp", MediaKind::Video) == "/sdcard/a.b/c.mp4");
    CHECK(CompressedPhonePath("/sdcard/p.JPG", MediaKind::Photo) == "/sdcard/p.JPG");
}

TEST_CASE("MapRestoreTarget remaps SD card paths")
{
    const std::vector<std::string> withSd = { "/storage/emulated/0/", "/storage/1A2B-3C4D/" };
    const std::vector<std::string> noSd = { "/storage/emulated/0/" };
    CHECK(MapRestoreTarget("/storage/emulated/0/DCIM/a.jpg", withSd) == "/storage/emulated/0/DCIM/a.jpg");
    CHECK(MapRestoreTarget("/storage/0000-0000/DCIM/a.jpg", withSd) == "/storage/1A2B-3C4D/DCIM/a.jpg");   // new phone's card
    CHECK(MapRestoreTarget("/storage/0000-0000/DCIM/a.jpg", noSd) == "/storage/emulated/0/Restored from SD card/DCIM/a.jpg");
}
