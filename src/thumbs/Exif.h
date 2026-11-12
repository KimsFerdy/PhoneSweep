#pragma once
#include <cstddef>
#include <cstdint>
#include <span>

namespace ps {

struct ExifInfo {
    bool found = false;   // an EXIF APP1 segment was parsed
    int orientation = 1;  // EXIF 0x0112, 1..8 (1 = upright)
    size_t thumbOffset = 0; // embedded JPEG thumbnail, offset into the input buffer
    size_t thumbLength = 0; // 0 = none (or not fully inside the buffer)
};

// Parses the EXIF block at the start of a JPEG. Works on a partial file (the
// first ~128 KB is enough on phones). Never reads out of bounds.
ExifInfo ParseJpegExif(std::span<const uint8_t> jpeg);

} // namespace ps
