#pragma once
#include "scan/FileModel.h"

#include <cstddef>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>

namespace ps {

class AdbClient;

// v1.1: side-by-side preview of duplicate copies.
enum class PreviewKind : uint8_t {
    Image, // thumbnail (preview size)
    Video, // first-frame thumbnail + play on the PC
    Audio, // in-app player
    Text,  // first 64 KB rendered as text
    Other, // sniffed: shown as text if it turns out to be text, else an icon
};

PreviewKind PreviewKindFor(const FileEntry& e);

// .txt, .log, .md, .json, .csv, .xml, .srt ... (case-insensitive, by file name).
bool IsTextName(std::string_view fileName);

// Turns the first bytes of a file into displayable UTF-8: BOMs handled (UTF-8,
// UTF-16 LE/BE), CRLF -> LF, tabs -> 4 spaces, a cut multi-byte character at the
// end dropped, at most maxLines lines. Returns nullopt when the bytes look binary
// (NULs or many control characters). Invalid UTF-8 is read as Latin-1.
std::optional<std::string> DecodeTextPreview(std::string_view bytes, size_t maxLines = 500);

inline constexpr size_t kTextPreviewBytes = 64 * 1024;

// `head -c 64K` over adb exec-out. Empty optional when the phone couldn't read it.
std::optional<std::string> FetchFileHead(const AdbClient& adb, const std::string& serial, const std::string& path, size_t bytes,
    std::stop_token st);

} // namespace ps
