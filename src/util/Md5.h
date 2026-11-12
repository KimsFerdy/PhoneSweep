#pragma once
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace ps {

// Incremental MD5 via Windows CNG (BCrypt). Lowercase hex, matching toybox md5sum.
class Md5 {
public:
    Md5();
    ~Md5();
    Md5(const Md5&) = delete;
    Md5& operator=(const Md5&) = delete;

    void Update(std::span<const uint8_t> data);
    void Update(std::string_view data) { Update({ reinterpret_cast<const uint8_t*>(data.data()), data.size() }); }
    std::string HexFinal(); // object is spent afterwards

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

std::string Md5Hex(std::string_view data);
// Streams a local file; nullopt if it can't be read.
std::optional<std::string> Md5File(const std::filesystem::path& file);

} // namespace ps
