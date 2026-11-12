#include "thumbs/Exif.h"

#include <optional>

namespace ps {

namespace {

// Bounds-checked reader over the TIFF block inside APP1.
class Tiff {
public:
    Tiff(std::span<const uint8_t> d, bool le) : m_d(d), m_le(le) {}

    std::optional<uint16_t> U16(size_t off) const
    {
        if (off + 2 > m_d.size())
            return std::nullopt;
        return m_le ? static_cast<uint16_t>(m_d[off] | (m_d[off + 1] << 8))
                    : static_cast<uint16_t>((m_d[off] << 8) | m_d[off + 1]);
    }
    std::optional<uint32_t> U32(size_t off) const
    {
        if (off + 4 > m_d.size())
            return std::nullopt;
        const uint32_t b0 = m_d[off], b1 = m_d[off + 1], b2 = m_d[off + 2], b3 = m_d[off + 3];
        return m_le ? (b0 | (b1 << 8) | (b2 << 16) | (b3 << 24)) : ((b0 << 24) | (b1 << 16) | (b2 << 8) | b3);
    }
    size_t Size() const { return m_d.size(); }

private:
    std::span<const uint8_t> m_d;
    bool m_le;
};

struct IfdResult {
    std::optional<int> orientation;
    std::optional<uint32_t> thumbOff, thumbLen;
    uint32_t next = 0;
};

IfdResult ReadIfd(const Tiff& t, uint32_t off)
{
    IfdResult r;
    const auto count = t.U16(off);
    if (!count || *count > 512)
        return r;
    for (uint32_t i = 0; i < *count; ++i) {
        const size_t e = off + 2 + static_cast<size_t>(i) * 12;
        const auto tag = t.U16(e);
        const auto type = t.U16(e + 2);
        if (!tag || !type)
            return r;
        // SHORT (3) values sit in the first 2 bytes of the value field; LONG (4) in all 4.
        auto value = [&]() -> std::optional<uint32_t> {
            if (*type == 3)
                return t.U16(e + 8);
            if (*type == 4)
                return t.U32(e + 8);
            return std::nullopt;
        };
        switch (*tag) {
        case 0x0112:
            if (auto v = value(); v && *v >= 1 && *v <= 8)
                r.orientation = static_cast<int>(*v);
            break;
        case 0x0201: r.thumbOff = value(); break;
        case 0x0202: r.thumbLen = value(); break;
        default: break;
        }
    }
    r.next = t.U32(off + 2 + static_cast<size_t>(*count) * 12).value_or(0);
    return r;
}

} // namespace

ExifInfo ParseJpegExif(std::span<const uint8_t> d)
{
    ExifInfo info;
    if (d.size() < 4 || d[0] != 0xFF || d[1] != 0xD8)
        return info;

    size_t p = 2;
    while (p + 4 <= d.size()) {
        if (d[p] != 0xFF)
            return info; // lost sync: not a well-formed header
        const uint8_t marker = d[p + 1];
        if (marker == 0xD8 || (marker >= 0xD0 && marker <= 0xD7) || marker == 0x01 || marker == 0xFF) {
            ++p; // standalone markers / fill bytes
            continue;
        }
        if (marker == 0xDA || marker == 0xD9)
            return info; // start of scan / end: no EXIF before the image data
        const size_t len = (static_cast<size_t>(d[p + 2]) << 8) | d[p + 3];
        if (len < 2)
            return info;
        const size_t segStart = p + 4;
        const size_t segEnd = p + 2 + len;

        if (marker == 0xE1 && segStart + 6 <= d.size() && d[segStart] == 'E' && d[segStart + 1] == 'x' && d[segStart + 2] == 'i'
            && d[segStart + 3] == 'f' && d[segStart + 4] == 0 && d[segStart + 5] == 0) {
            const size_t tiffStart = segStart + 6;
            // The segment may extend past what we downloaded; parse what we have.
            const size_t tiffEnd = std::min(segEnd, d.size());
            if (tiffStart + 8 > tiffEnd)
                return info;
            const auto tiffBytes = d.subspan(tiffStart, tiffEnd - tiffStart);
            bool le;
            if (tiffBytes[0] == 'I' && tiffBytes[1] == 'I')
                le = true;
            else if (tiffBytes[0] == 'M' && tiffBytes[1] == 'M')
                le = false;
            else
                return info;
            const Tiff t(tiffBytes, le);
            if (t.U16(2).value_or(0) != 42)
                return info;
            info.found = true;

            const uint32_t ifd0 = t.U32(4).value_or(0);
            if (ifd0 < 8 || ifd0 >= t.Size())
                return info;
            const IfdResult r0 = ReadIfd(t, ifd0);
            if (r0.orientation)
                info.orientation = *r0.orientation;
            if (r0.next >= 8 && r0.next < t.Size()) {
                const IfdResult r1 = ReadIfd(t, r0.next);
                if (r1.thumbOff && r1.thumbLen && *r1.thumbLen > 0) {
                    const size_t abs = tiffStart + *r1.thumbOff;
                    if (abs + *r1.thumbLen <= d.size() && abs + 1 < d.size() && d[abs] == 0xFF && d[abs + 1] == 0xD8) {
                        info.thumbOffset = abs;
                        info.thumbLength = *r1.thumbLen;
                    }
                }
            }
            return info;
        }
        p = segEnd;
    }
    return info;
}

} // namespace ps
