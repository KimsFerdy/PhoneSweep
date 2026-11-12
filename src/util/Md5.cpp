#include "util/Md5.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>

#include <fstream>
#include <stdexcept>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

namespace ps {

struct Md5::Impl {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    bool finished = false;
};

Md5::Md5()
    : m_impl(std::make_unique<Impl>())
{
    if (BCryptOpenAlgorithmProvider(&m_impl->alg, BCRYPT_MD5_ALGORITHM, nullptr, 0) != 0
        || BCryptCreateHash(m_impl->alg, &m_impl->hash, nullptr, 0, nullptr, 0, 0) != 0)
        throw std::runtime_error("BCrypt MD5 unavailable");
}

Md5::~Md5()
{
    if (m_impl->hash)
        BCryptDestroyHash(m_impl->hash);
    if (m_impl->alg)
        BCryptCloseAlgorithmProvider(m_impl->alg, 0);
}

void Md5::Update(std::span<const uint8_t> data)
{
    // BCryptHashData takes ULONG lengths: feed in chunks.
    while (!data.empty()) {
        const ULONG n = static_cast<ULONG>(std::min<size_t>(data.size(), 1u << 30));
        BCryptHashData(m_impl->hash, const_cast<PUCHAR>(data.data()), n, 0);
        data = data.subspan(n);
    }
}

std::string Md5::HexFinal()
{
    uint8_t digest[16] = {};
    BCryptFinishHash(m_impl->hash, digest, sizeof(digest), 0);
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out(32, '0');
    for (int i = 0; i < 16; ++i) {
        out[i * 2] = kHex[digest[i] >> 4];
        out[i * 2 + 1] = kHex[digest[i] & 0xF];
    }
    return out;
}

std::string Md5Hex(std::string_view data)
{
    Md5 m;
    m.Update(data);
    return m.HexFinal();
}

std::optional<std::string> Md5File(const std::filesystem::path& file)
{
    std::ifstream in(file, std::ios::binary);
    if (!in)
        return std::nullopt;
    Md5 m;
    std::vector<char> buf(1 << 20);
    while (in) {
        in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        const auto n = in.gcount();
        if (n > 0)
            m.Update(std::string_view(buf.data(), static_cast<size_t>(n)));
    }
    if (in.bad())
        return std::nullopt;
    return m.HexFinal();
}

} // namespace ps
