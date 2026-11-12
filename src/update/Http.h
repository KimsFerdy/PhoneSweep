#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <stop_token>
#include <string>
#include <string_view>

namespace ps {

struct HttpResult {
    bool ok = false;     // 2xx and the body arrived completely
    int status = 0;      // HTTP status, 0 if no response
    std::string body;    // HttpGet only
    std::string error;   // human-readable reason when !ok
    bool cancelled = false;
};

// Minimal HTTPS client on WinHTTP (system proxy settings, TLS from Windows).
// Only https:// URLs are accepted. Redirects are followed (never to plain http).
HttpResult HttpGet(std::string_view url, std::stop_token st, size_t maxBytes = 4u << 20);

// Streams the body to `file` (written as file + ".part", renamed when complete).
HttpResult HttpDownload(std::string_view url, const std::filesystem::path& file, std::stop_token st,
    const std::function<void(uint64_t done, uint64_t total)>& onProgress = {});

} // namespace ps
