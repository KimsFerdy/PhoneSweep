#include "thumbs/ImageOps.h"

#include <stb_image.h>
#include <stb_image_resize2.h>
#include <stb_image_write.h>

#include <algorithm>
#include <cmath>

namespace ps {

namespace {

char Lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; }

std::string Ext(std::string_view name)
{
    const auto dot = name.rfind('.');
    if (dot == std::string_view::npos)
        return {};
    std::string e(name.substr(dot + 1));
    for (char& c : e)
        c = Lower(c);
    return e;
}

} // namespace

bool IsJpeg(std::string_view name)
{
    const auto e = Ext(name);
    return e == "jpg" || e == "jpeg" || e == "jfif";
}

bool IsDecodableImage(std::string_view name)
{
    const auto e = Ext(name);
    return e == "jpg" || e == "jpeg" || e == "jfif" || e == "png" || e == "gif" || e == "bmp";
}

std::optional<Image> DecodeImage(std::span<const uint8_t> bytes)
{
    if (bytes.empty() || bytes.size() > static_cast<size_t>(INT32_MAX))
        return std::nullopt;
    int w = 0, h = 0, n = 0;
    stbi_uc* px = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &w, &h, &n, 4);
    if (!px)
        return std::nullopt;
    Image img;
    img.w = w;
    img.h = h;
    img.rgba.assign(px, px + static_cast<size_t>(w) * h * 4);
    stbi_image_free(px);
    return img;
}

Image ApplyOrientation(Image img, int o)
{
    if (o <= 1 || o > 8 || !img.Valid())
        return img;
    const int W = img.w, H = img.h;
    const bool swap = o >= 5; // 5..8 transpose dimensions
    Image out;
    out.w = swap ? H : W;
    out.h = swap ? W : H;
    out.rgba.resize(img.rgba.size());
    const auto* src = reinterpret_cast<const uint32_t*>(img.rgba.data());
    auto* dst = reinterpret_cast<uint32_t*>(out.rgba.data());
    for (int dy = 0; dy < out.h; ++dy) {
        for (int dx = 0; dx < out.w; ++dx) {
            int sx = dx, sy = dy;
            switch (o) {
            case 2: sx = W - 1 - dx; sy = dy; break;          // mirror horizontal
            case 3: sx = W - 1 - dx; sy = H - 1 - dy; break;  // rotate 180
            case 4: sx = dx; sy = H - 1 - dy; break;          // mirror vertical
            case 5: sx = dy; sy = dx; break;                  // transpose
            case 6: sx = dy; sy = H - 1 - dx; break;          // rotate 90 CW
            case 7: sx = W - 1 - dy; sy = H - 1 - dx; break;  // transverse
            case 8: sx = W - 1 - dy; sy = dx; break;          // rotate 90 CCW
            default: break;
            }
            dst[static_cast<size_t>(dy) * out.w + dx] = src[static_cast<size_t>(sy) * W + sx];
        }
    }
    return out;
}

Image FitWithin(const Image& img, int maxEdge)
{
    if (!img.Valid() || (img.w <= maxEdge && img.h <= maxEdge))
        return img;
    const double s = static_cast<double>(maxEdge) / std::max(img.w, img.h);
    Image out;
    out.w = std::max(1, static_cast<int>(std::lround(img.w * s)));
    out.h = std::max(1, static_cast<int>(std::lround(img.h * s)));
    out.rgba.resize(static_cast<size_t>(out.w) * out.h * 4);
    stbir_resize_uint8_srgb(img.rgba.data(), img.w, img.h, 0, out.rgba.data(), out.w, out.h, 0, STBIR_RGBA);
    return out;
}

std::string EncodeJpeg(const Image& img, int quality)
{
    if (!img.Valid())
        return {};
    std::string out;
    auto write = [](void* ctx, void* data, int size) { static_cast<std::string*>(ctx)->append(static_cast<const char*>(data), size); };
    if (!stbi_write_jpg_to_func(write, &out, img.w, img.h, 4, img.rgba.data(), quality))
        return {};
    return out;
}

} // namespace ps
