#include "thumbs/Exif.h"
#include "thumbs/ImageOps.h"
#include "thumbs/ThumbFetch.h"

#include <doctest/doctest.h>

#include <ostream>
#include <string>
#include <vector>

using namespace ps;

namespace {

Image Gradient(int w, int h)
{
    Image img;
    img.w = w;
    img.h = h;
    img.rgba.resize(static_cast<size_t>(w) * h * 4);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint8_t* p = &img.rgba[(static_cast<size_t>(y) * w + x) * 4];
            p[0] = static_cast<uint8_t>(x * 255 / std::max(1, w - 1));
            p[1] = static_cast<uint8_t>(y * 255 / std::max(1, h - 1));
            p[2] = 128;
            p[3] = 255;
        }
    return img;
}

// Image whose pixel value (red channel) encodes its source index, for transform tests.
Image Ids(int w, int h)
{
    Image img;
    img.w = w;
    img.h = h;
    img.rgba.assign(static_cast<size_t>(w) * h * 4, 0);
    for (int i = 0; i < w * h; ++i)
        img.rgba[static_cast<size_t>(i) * 4] = static_cast<uint8_t>(i);
    return img;
}

int At(const Image& img, int x, int y) { return img.rgba[(static_cast<size_t>(y) * img.w + x) * 4]; }

struct Writer {
    std::vector<uint8_t> b;
    bool le = true;
    void u8(uint8_t v) { b.push_back(v); }
    void u16(uint16_t v)
    {
        if (le) {
            u8(v & 0xFF);
            u8(v >> 8);
        } else {
            u8(v >> 8);
            u8(v & 0xFF);
        }
    }
    void u32(uint32_t v)
    {
        if (le) {
            u16(v & 0xFFFF);
            u16(v >> 16);
        } else {
            u16(v >> 16);
            u16(v & 0xFFFF);
        }
    }
};

// Minimal JPEG: SOI + APP1(Exif: IFD0{orientation}, IFD1{thumb offset/len}) + embedded thumb + SOS stub.
std::vector<uint8_t> MakeExifJpeg(int orientation, const std::string& thumb, bool littleEndian)
{
    Writer t; // TIFF block
    t.le = littleEndian;
    t.u8(littleEndian ? 'I' : 'M');
    t.u8(littleEndian ? 'I' : 'M');
    t.u16(42);
    t.u32(8); // IFD0 at 8
    // IFD0: 1 entry (orientation, SHORT)
    t.u16(1);
    t.u16(0x0112);
    t.u16(3);
    t.u32(1);
    t.u16(static_cast<uint16_t>(orientation));
    t.u16(0);
    const uint32_t ifd1 = 8 + 2 + 12 + 4;
    t.u32(ifd1);
    // IFD1: 2 entries (thumb offset, length; LONG)
    const uint32_t thumbOff = ifd1 + 2 + 2 * 12 + 4;
    t.u16(2);
    t.u16(0x0201);
    t.u16(4);
    t.u32(1);
    t.u32(thumbOff);
    t.u16(0x0202);
    t.u16(4);
    t.u32(1);
    t.u32(static_cast<uint32_t>(thumb.size()));
    t.u32(0); // no next IFD
    for (char c : thumb)
        t.u8(static_cast<uint8_t>(c));

    std::vector<uint8_t> out = { 0xFF, 0xD8, 0xFF, 0xE1 };
    const size_t segLen = 2 + 6 + t.b.size();
    out.push_back(static_cast<uint8_t>(segLen >> 8));
    out.push_back(static_cast<uint8_t>(segLen & 0xFF));
    for (char c : std::string("Exif\0\0", 6))
        out.push_back(static_cast<uint8_t>(c));
    out.insert(out.end(), t.b.begin(), t.b.end());
    out.insert(out.end(), { 0xFF, 0xDA, 0x00, 0x02 }); // start of scan: parser stops here
    return out;
}

} // namespace

TEST_CASE("EXIF: orientation and embedded thumbnail (little endian)")
{
    const std::string thumb = EncodeJpeg(Gradient(200, 150));
    REQUIRE(!thumb.empty());
    const auto jpeg = MakeExifJpeg(6, thumb, true);
    const ExifInfo info = ParseJpegExif(jpeg);
    CHECK(info.found);
    CHECK(info.orientation == 6);
    REQUIRE(info.thumbLength == thumb.size());
    // The located bytes decode back to the embedded image.
    const auto img = DecodeImage(std::span(jpeg).subspan(info.thumbOffset, info.thumbLength));
    REQUIRE(img);
    CHECK(img->w == 200);
    CHECK(img->h == 150);
}

TEST_CASE("EXIF: big-endian (Motorola) TIFF")
{
    const std::string thumb = EncodeJpeg(Gradient(64, 48));
    const auto jpeg = MakeExifJpeg(8, thumb, false);
    const ExifInfo info = ParseJpegExif(jpeg);
    CHECK(info.found);
    CHECK(info.orientation == 8);
    CHECK(info.thumbLength == thumb.size());
}

TEST_CASE("EXIF: truncated downloads never read out of bounds")
{
    const std::string thumb = EncodeJpeg(Gradient(32, 32));
    const auto jpeg = MakeExifJpeg(3, thumb, true);
    for (size_t n = 0; n <= jpeg.size(); ++n) {
        const ExifInfo info = ParseJpegExif(std::span(jpeg.data(), n));
        if (info.thumbLength) // only reported when the whole thumbnail is inside the buffer
            CHECK(info.thumbOffset + info.thumbLength <= n);
    }
}

TEST_CASE("EXIF: non-JPEG and JPEG without EXIF")
{
    const std::vector<uint8_t> png = { 0x89, 'P', 'N', 'G', 0, 0, 0, 0 };
    CHECK_FALSE(ParseJpegExif(png).found);
    const std::string plain = EncodeJpeg(Gradient(8, 8)); // stb writes JFIF, no EXIF
    const ExifInfo info = ParseJpegExif(std::span(reinterpret_cast<const uint8_t*>(plain.data()), plain.size()));
    CHECK_FALSE(info.found);
    CHECK(info.orientation == 1);
}

TEST_CASE("ApplyOrientation maps pixels like a phone gallery")
{
    // Source 3x2:   0 1 2
    //               3 4 5
    const Image src = Ids(3, 2);

    const Image r6 = ApplyOrientation(src, 6); // rotate 90 CW -> 2x3:  3 0 / 4 1 / 5 2
    REQUIRE(r6.w == 2);
    REQUIRE(r6.h == 3);
    CHECK(At(r6, 0, 0) == 3);
    CHECK(At(r6, 1, 0) == 0);
    CHECK(At(r6, 0, 2) == 5);
    CHECK(At(r6, 1, 2) == 2);

    const Image r8 = ApplyOrientation(src, 8); // rotate 90 CCW -> 2 5 / 1 4 / 0 3
    CHECK(At(r8, 0, 0) == 2);
    CHECK(At(r8, 1, 0) == 5);
    CHECK(At(r8, 0, 2) == 0);

    const Image r3 = ApplyOrientation(src, 3); // 180 -> 5 4 3 / 2 1 0
    CHECK(At(r3, 0, 0) == 5);
    CHECK(At(r3, 2, 1) == 0);

    const Image r2 = ApplyOrientation(src, 2); // mirror -> 2 1 0 / 5 4 3
    CHECK(At(r2, 0, 0) == 2);
    CHECK(At(r2, 0, 1) == 5);

    const Image r5 = ApplyOrientation(src, 5); // transpose -> 0 3 / 1 4 / 2 5
    CHECK(At(r5, 1, 0) == 3);
    CHECK(At(r5, 0, 2) == 2);

    const Image r1 = ApplyOrientation(src, 1);
    CHECK(r1.rgba == src.rgba);
}

TEST_CASE("FitWithin keeps aspect and never upscales")
{
    const Image big = FitWithin(Gradient(4032, 3024), 256);
    CHECK(big.w == 256);
    CHECK(big.h == 192);
    const Image small = FitWithin(Gradient(100, 50), 256);
    CHECK(small.w == 100);
    CHECK(small.h == 50);
}

TEST_CASE("Decodable formats by extension")
{
    CHECK(IsDecodableImage("IMG_1.JPG"));
    CHECK(IsDecodableImage("a.png"));
    CHECK_FALSE(IsDecodableImage("sticker.webp"));
    CHECK_FALSE(IsDecodableImage("photo.heic"));
    CHECK(IsJpeg("x.jpeg"));
    CHECK_FALSE(IsJpeg("x.png"));
}

TEST_CASE("ThumbKey changes with any identity field")
{
    const auto k = ThumbKey("dev", "/sdcard/a.jpg", 10, 100, ThumbVariant::Grid);
    CHECK(k == ThumbKey("dev", "/sdcard/a.jpg", 10, 100, ThumbVariant::Grid));
    CHECK(k != ThumbKey("dev2", "/sdcard/a.jpg", 10, 100, ThumbVariant::Grid));
    CHECK(k != ThumbKey("dev", "/sdcard/b.jpg", 10, 100, ThumbVariant::Grid));
    CHECK(k != ThumbKey("dev", "/sdcard/a.jpg", 11, 100, ThumbVariant::Grid));
    CHECK(k != ThumbKey("dev", "/sdcard/a.jpg", 10, 101, ThumbVariant::Grid));
    CHECK(k != ThumbKey("dev", "/sdcard/a.jpg", 10, 100, ThumbVariant::Preview));
}
