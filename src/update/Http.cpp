#include "update/Http.h"

#include "app/Version.h"
#include "util/Log.h"
#include "util/Utf8.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>

#include <format>
#include <fstream>
#include <memory>
#include <vector>

namespace ps {

namespace {

struct HandleCloser {
    void operator()(HINTERNET h) const
    {
        if (h)
            WinHttpCloseHandle(h);
    }
};
using Handle = std::unique_ptr<void, HandleCloser>;

std::string LastError(const char* what)
{
    return std::format("{} failed (error {})", what, GetLastError());
}

// Opens a GET request and waits for the response headers. On success `req` is ready to read.
HttpResult Open(std::string_view url, Handle& session, Handle& connect, Handle& req)
{
    HttpResult r;
    if (!url.starts_with("https://")) {
        r.error = "Only https:// downloads are allowed";
        return r;
    }
    const std::wstring wurl = Widen(url);
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256]{}, path[2048]{};
    uc.lpszHostName = host;
    uc.dwHostNameLength = static_cast<DWORD>(std::size(host));
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = static_cast<DWORD>(std::size(path));
    wchar_t extra[2048]{};
    uc.lpszExtraInfo = extra;
    uc.dwExtraInfoLength = static_cast<DWORD>(std::size(extra));
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc) || uc.nScheme != INTERNET_SCHEME_HTTPS) {
        r.error = "Bad download address";
        return r;
    }
    const std::wstring ua = Widen(std::format("PhoneSweep/{} (Windows)", kAppVersion));
    session.reset(WinHttpOpen(ua.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) {
        r.error = LastError("WinHttpOpen");
        return r;
    }
    WinHttpSetTimeouts(session.get(), 10'000, 15'000, 20'000, 30'000);
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
    if (!WinHttpSetOption(session.get(), WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols))) {
        protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2; // older Windows 10 builds: no TLS 1.3 flag
        WinHttpSetOption(session.get(), WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
    }
    connect.reset(WinHttpConnect(session.get(), host, uc.nPort, 0));
    if (!connect) {
        r.error = LastError("WinHttpConnect");
        return r;
    }
    const std::wstring object = std::wstring(path) + extra;
    req.reset(WinHttpOpenRequest(connect.get(), L"GET", object.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE));
    if (!req) {
        r.error = LastError("WinHttpOpenRequest");
        return r;
    }
    DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    WinHttpSetOption(req.get(), WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof(redirect));
    const wchar_t* headers = L"Accept: application/vnd.github+json, application/octet-stream\r\nX-GitHub-Api-Version: 2022-11-28\r\n";
    if (!WinHttpSendRequest(req.get(), headers, static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
        || !WinHttpReceiveResponse(req.get(), nullptr)) {
        const DWORD e = GetLastError();
        r.error = e == ERROR_WINHTTP_NAME_NOT_RESOLVED || e == ERROR_WINHTTP_CANNOT_CONNECT
            ? "No connection to GitHub. Check your internet connection."
            : e == ERROR_WINHTTP_TIMEOUT ? "GitHub didn't answer in time." : std::format("Connection failed (error {})", e);
        return r;
    }
    DWORD status = 0, len = sizeof(status);
    WinHttpQueryHeaders(req.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &len,
        WINHTTP_NO_HEADER_INDEX);
    r.status = static_cast<int>(status);
    if (status < 200 || status >= 300) {
        r.error = status == 404 ? "Not found on GitHub (404)"
            : status == 403 || status == 429 ? "GitHub is limiting requests right now. Try again in a while."
                                             : std::format("GitHub answered {}", status);
        return r;
    }
    r.ok = true;
    return r;
}

uint64_t ContentLength(HINTERNET req)
{
    wchar_t buf[32]{};
    DWORD len = sizeof(buf);
    if (!WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, buf, &len, WINHTTP_NO_HEADER_INDEX))
        return 0;
    return _wcstoui64(buf, nullptr, 10);
}

// Reads the body in chunks; sink returns false to stop with an error.
bool ReadBody(HINTERNET req, std::stop_token st, HttpResult& r, const std::function<bool(const char*, size_t)>& sink)
{
    std::vector<char> buf(64 * 1024);
    for (;;) {
        if (st.stop_requested()) {
            r.ok = false;
            r.cancelled = true;
            r.error = "Cancelled";
            return false;
        }
        DWORD got = 0;
        if (!WinHttpReadData(req, buf.data(), static_cast<DWORD>(buf.size()), &got)) {
            r.ok = false;
            r.error = GetLastError() == ERROR_WINHTTP_TIMEOUT ? "The download stalled (timeout)." : LastError("Download");
            return false;
        }
        if (got == 0)
            return true;
        if (!sink(buf.data(), got)) {
            r.ok = false;
            return false;
        }
    }
}

} // namespace

HttpResult HttpGet(std::string_view url, std::stop_token st, size_t maxBytes)
{
    Handle session, connect, req;
    HttpResult r = Open(url, session, connect, req);
    if (!r.ok)
        return r;
    ReadBody(req.get(), st, r, [&](const char* p, size_t n) {
        if (r.body.size() + n > maxBytes) {
            r.error = "Answer too large";
            return false;
        }
        r.body.append(p, n);
        return true;
    });
    return r;
}

HttpResult HttpDownload(std::string_view url, const std::filesystem::path& file, std::stop_token st,
    const std::function<void(uint64_t, uint64_t)>& onProgress)
{
    Handle session, connect, req;
    HttpResult r = Open(url, session, connect, req);
    if (!r.ok)
        return r;
    const uint64_t total = ContentLength(req.get());
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    auto part = file;
    part += L".part";
    uint64_t done = 0;
    {
        std::ofstream out(part, std::ios::binary | std::ios::trunc);
        if (!out) {
            r.ok = false;
            r.error = "Can't write the download to " + Narrow(part.wstring());
            return r;
        }
        ReadBody(req.get(), st, r, [&](const char* p, size_t n) {
            out.write(p, static_cast<std::streamsize>(n));
            if (!out) {
                r.error = "Couldn't write the download (disk full?)";
                return false;
            }
            done += n;
            if (onProgress)
                onProgress(done, total);
            return true;
        });
    }
    if (r.ok && total && done != total) {
        r.ok = false;
        r.error = std::format("Download incomplete ({} of {} bytes)", done, total);
    }
    if (!r.ok) {
        std::filesystem::remove(part, ec);
        return r;
    }
    std::filesystem::rename(part, file, ec);
    if (ec) {
        r.ok = false;
        r.error = "Couldn't finish the download: " + ec.message();
    }
    return r;
}

} // namespace ps
