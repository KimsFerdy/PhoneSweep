#include "transfer/LocalPath.h"

#include "util/Utf8.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>

#include <array>
#include <chrono>
#include <format>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

namespace ps {

namespace {

// Length of a valid UTF-8 sequence starting at s[i], or 0 if invalid.
size_t Utf8SeqLen(std::string_view s, size_t i)
{
    const auto c = static_cast<unsigned char>(s[i]);
    size_t n = 0;
    uint32_t cp = 0;
    if (c < 0x80)
        return 1;
    if ((c & 0xE0) == 0xC0) {
        n = 2;
        cp = c & 0x1F;
    } else if ((c & 0xF0) == 0xE0) {
        n = 3;
        cp = c & 0x0F;
    } else if ((c & 0xF8) == 0xF0) {
        n = 4;
        cp = c & 0x07;
    } else {
        return 0;
    }
    if (i + n > s.size())
        return 0;
    for (size_t k = 1; k < n; ++k) {
        const auto cc = static_cast<unsigned char>(s[i + k]);
        if ((cc & 0xC0) != 0x80)
            return 0;
        cp = (cp << 6) | (cc & 0x3F);
    }
    // Reject overlongs, surrogates and > U+10FFFF.
    if ((n == 2 && cp < 0x80) || (n == 3 && cp < 0x800) || (n == 4 && cp < 0x10000) || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
        return 0;
    return n;
}

bool IsReserved(std::string_view base)
{
    static constexpr std::array kNames = { "CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8",
        "COM9", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9" };
    for (const char* n : kNames) {
        const std::string_view nv(n);
        if (base.size() != nv.size())
            continue;
        bool eq = true;
        for (size_t i = 0; i < base.size() && eq; ++i)
            eq = (base[i] >= 'a' && base[i] <= 'z' ? base[i] - 32 : base[i]) == nv[i];
        if (eq)
            return true;
    }
    return false;
}

} // namespace

std::string SanitizeComponent(std::string_view name)
{
    std::string out;
    out.reserve(name.size());
    for (size_t i = 0; i < name.size();) {
        const size_t n = Utf8SeqLen(name, i);
        if (n == 0) {
            out.push_back('_'); // invalid byte (e.g. FAT-mangled names)
            ++i;
            continue;
        }
        if (n == 1) {
            const char c = name[i];
            if (static_cast<unsigned char>(c) < 0x20 || c == '<' || c == '>' || c == ':' || c == '"' || c == '|' || c == '?' || c == '*'
                || c == '\\' || c == '/')
                out.push_back('_');
            else
                out.push_back(c);
        } else {
            out.append(name.substr(i, n));
        }
        i += n;
    }
    // Windows strips trailing dots/spaces, which would silently rename files.
    while (!out.empty() && (out.back() == '.' || out.back() == ' '))
        out.back() = '_';
    if (out.empty())
        out = "_";
    if (out == "." || out == "..")
        out = "_";
    const auto dot = out.find('.');
    if (IsReserved(std::string_view(out).substr(0, dot)))
        out.insert(0, "_");
    return out;
}

std::filesystem::path MapToLocal(std::string_view volumePrefix, std::string_view relPath, bool preserveStructure)
{
    std::filesystem::path out;
    if (!volumePrefix.empty())
        out /= Widen(SanitizeComponent(volumePrefix));
    if (!preserveStructure) {
        const auto slash = relPath.rfind('/');
        return out / Widen(SanitizeComponent(slash == std::string_view::npos ? relPath : relPath.substr(slash + 1)));
    }
    size_t s = 0;
    while (s < relPath.size()) {
        const auto e = relPath.find('/', s);
        const auto comp = relPath.substr(s, e == std::string_view::npos ? std::string_view::npos : e - s);
        if (!comp.empty())
            out /= Widen(SanitizeComponent(comp));
        if (e == std::string_view::npos)
            break;
        s = e + 1;
    }
    return out;
}

const char* ToString(ConflictPolicy p)
{
    switch (p) {
    case ConflictPolicy::SkipIfIdentical: return "skip if identical";
    case ConflictPolicy::Rename: return "rename";
    case ConflictPolicy::Overwrite: return "overwrite";
    }
    return "?";
}

std::filesystem::path NextFreeName(const std::filesystem::path& p)
{
    std::error_code ec;
    if (!std::filesystem::exists(LongPath(p), ec))
        return p;
    const auto stem = p.stem().wstring();
    const auto ext = p.extension().wstring();
    for (int i = 1; i < 100000; ++i) {
        auto cand = p.parent_path() / (stem + L" (" + std::to_wstring(i) + L")" + ext);
        if (!std::filesystem::exists(LongPath(cand), ec))
            return cand;
    }
    return p;
}

std::filesystem::path LongPath(const std::filesystem::path& p)
{
    const std::wstring& s = p.native();
    if (s.size() < 240 || !p.is_absolute() || s.starts_with(L"\\\\?\\"))
        return p;
    if (s.starts_with(L"\\\\")) // UNC
        return std::filesystem::path(L"\\\\?\\UNC\\" + s.substr(2));
    return std::filesystem::path(L"\\\\?\\" + s);
}

std::string ExpandFolderPattern(std::string_view pattern, std::string_view device, int64_t unixTime)
{
    const auto local = std::chrono::current_zone()->to_local(std::chrono::sys_seconds(std::chrono::seconds(unixTime)));
    const std::string date = std::format("{:%Y-%m-%d}", local);
    const std::string time = std::format("{:%H%M}", local);
    std::string out;
    for (size_t i = 0; i < pattern.size();) {
        auto take = [&](std::string_view key, std::string_view value) {
            if (pattern.substr(i).starts_with(key)) {
                out += value;
                i += key.size();
                return true;
            }
            return false;
        };
        if (take("{device}", device) || take("{model}", device) || take("{date}", date) || take("{time}", time))
            continue;
        out.push_back(pattern[i++]);
    }
    return SanitizeComponent(out);
}

std::filesystem::path DocumentsFolder()
{
    PWSTR p = nullptr;
    std::filesystem::path out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &p)) && p)
        out = p;
    CoTaskMemFree(p);
    if (out.empty()) {
        wchar_t buf[MAX_PATH];
        if (GetEnvironmentVariableW(L"USERPROFILE", buf, MAX_PATH))
            out = std::filesystem::path(buf) / L"Documents";
    }
    return out;
}

bool SetFileMtime(const std::filesystem::path& p, int64_t unixSeconds)
{
    HANDLE h = CreateFileW(LongPath(p).c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    // FILETIME: 100-ns ticks since 1601-01-01.
    const ULONGLONG ticks = (static_cast<ULONGLONG>(unixSeconds) + 11644473600ull) * 10000000ull;
    FILETIME ft{ static_cast<DWORD>(ticks & 0xFFFFFFFF), static_cast<DWORD>(ticks >> 32) };
    const BOOL ok = SetFileTime(h, nullptr, nullptr, &ft);
    CloseHandle(h);
    return ok != 0;
}

uint64_t FreeSpace(const std::filesystem::path& dir)
{
    // Walk up to an existing ancestor (the backup folder may not exist yet).
    std::filesystem::path d = dir;
    std::error_code ec;
    while (!d.empty() && !std::filesystem::exists(d, ec) && d.has_parent_path() && d.parent_path() != d)
        d = d.parent_path();
    ULARGE_INTEGER freeBytes{};
    if (GetDiskFreeSpaceExW(d.c_str(), &freeBytes, nullptr, nullptr))
        return freeBytes.QuadPart;
    return 0;
}

} // namespace ps
