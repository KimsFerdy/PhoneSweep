#pragma once
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace ps {

// Tiny audio player on Windows Media Foundation (MFPlay): mp3, m4a/aac, wav, wma,
// flac, and ogg/opus where Windows has the codec (Web Media Extensions).
// UI thread only (events arrive through the window's message loop). One file at a time.
class AudioPlayer {
public:
    AudioPlayer();
    ~AudioPlayer();
    AudioPlayer(const AudioPlayer&) = delete;
    AudioPlayer& operator=(const AudioPlayer&) = delete;

    // Starts opening + playing (asynchronous). False if it couldn't even start.
    bool Play(const std::filesystem::path& file, std::string key);
    void Close();

    bool IsPlaying() const;
    const std::string& Key() const { return m_key; } // which item is loaded ("" = none)
    const std::filesystem::path& File() const { return m_file; }
    uint32_t PositionMs() const;
    uint32_t LengthMs() const;
    void SeekMs(uint32_t ms);
    // True once when Windows couldn't open/decode the file after Play() returned.
    bool TakeFailure();

private:
    struct Impl;
    std::unique_ptr<Impl> m;
    std::string m_key;
    std::filesystem::path m_file;
};

} // namespace ps
