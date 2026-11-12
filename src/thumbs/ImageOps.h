#pragma once
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ps {

struct Image {
    int w = 0, h = 0;
    std::vector<uint8_t> rgba; // w*h*4, row-major, top-down
    bool Valid() const { return w > 0 && h > 0 && rgba.size() == static_cast<size_t>(w) * h * 4; }
};

// JPEG / PNG / GIF (first frame) / BMP via stb_image. nullopt if undecodable.
std::optional<Image> DecodeImage(std::span<const uint8_t> bytes);
// True for extensions DecodeImage can handle.
bool IsDecodableImage(std::string_view fileName);
bool IsJpeg(std::string_view fileName);

// Applies an EXIF orientation (1..8) so the image displays upright.
Image ApplyOrientation(Image img, int orientation);
// Downscales so the longest edge is <= maxEdge (never upscales). sRGB-correct.
Image FitWithin(const Image& img, int maxEdge);
// Encodes to JPEG (alpha dropped). Empty on failure.
std::string EncodeJpeg(const Image& img, int quality = 85);

} // namespace ps
