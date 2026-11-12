#include "platform/AudioPlayer.h"

#include "util/Log.h"
#include "util/Utf8.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mfplay.h>
#include <propvarutil.h>

#include <atomic>

namespace ps {

namespace {

// Records asynchronous failures (file can't be opened / no codec) and the end of playback.
class Callback final : public IMFPMediaPlayerCallback {
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (riid == __uuidof(IMFPMediaPlayerCallback) || riid == __uuidof(IUnknown)) {
            *ppv = static_cast<IMFPMediaPlayerCallback*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++m_refs; }
    STDMETHODIMP_(ULONG) Release() override
    {
        const ULONG r = --m_refs;
        if (r == 0)
            delete this;
        return r;
    }
    void STDMETHODCALLTYPE OnMediaPlayerEvent(MFP_EVENT_HEADER* e) override
    {
        if (!e)
            return;
        if (FAILED(e->hrEvent)) {
            failed = true;
            LogWarn("Audio preview: Windows can't play this file (0x{:08X})", static_cast<uint32_t>(e->hrEvent));
        }
        if (e->eEventType == MFP_EVENT_TYPE_PLAYBACK_ENDED)
            ended = true;
    }

    std::atomic<bool> failed{ false }, ended{ false };

private:
    std::atomic<ULONG> m_refs{ 1 };
};

} // namespace

struct AudioPlayer::Impl {
    IMFPMediaPlayer* player = nullptr;
    Callback* callback = nullptr;
    bool comInit = false;

    void Release()
    {
        if (player) {
            player->Shutdown();
            player->Release();
            player = nullptr;
        }
        if (callback) {
            callback->Release();
            callback = nullptr;
        }
    }
};

AudioPlayer::AudioPlayer()
    : m(std::make_unique<Impl>())
{
}

AudioPlayer::~AudioPlayer()
{
    Close();
    if (m->comInit)
        CoUninitialize();
}

bool AudioPlayer::Play(const std::filesystem::path& file, std::string key)
{
    Close();
    if (!m->comInit)
        m->comInit = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
    m->callback = new Callback();
    const HRESULT hr = MFPCreateMediaPlayer(file.c_str(), TRUE, 0, m->callback, nullptr, &m->player);
    if (FAILED(hr)) {
        LogWarn("Audio preview: can't start the player for {} (0x{:08X})", Narrow(file.wstring()), static_cast<uint32_t>(hr));
        m->Release();
        return false;
    }
    m_key = std::move(key);
    m_file = file;
    return true;
}

void AudioPlayer::Close()
{
    m->Release();
    m_key.clear();
    m_file.clear();
}

bool AudioPlayer::IsPlaying() const
{
    if (!m->player || m->callback->failed || m->callback->ended)
        return false;
    MFP_MEDIAPLAYER_STATE state = MFP_MEDIAPLAYER_STATE_EMPTY;
    if (FAILED(m->player->GetState(&state)))
        return false;
    // EMPTY while the file is still opening (fStartPlayback=TRUE): count it as playing.
    return state == MFP_MEDIAPLAYER_STATE_PLAYING || state == MFP_MEDIAPLAYER_STATE_EMPTY;
}

namespace {
uint32_t ToMs(const PROPVARIANT& v)
{
    if (v.vt == VT_I8)
        return static_cast<uint32_t>(v.hVal.QuadPart / 10'000); // 100 ns units
    if (v.vt == VT_UI8)
        return static_cast<uint32_t>(v.uhVal.QuadPart / 10'000);
    return 0;
}
} // namespace

uint32_t AudioPlayer::PositionMs() const
{
    if (!m->player)
        return 0;
    PROPVARIANT v;
    PropVariantInit(&v);
    const uint32_t ms = SUCCEEDED(m->player->GetPosition(MFP_POSITIONTYPE_100NS, &v)) ? ToMs(v) : 0;
    PropVariantClear(&v);
    return ms;
}

uint32_t AudioPlayer::LengthMs() const
{
    if (!m->player)
        return 0;
    PROPVARIANT v;
    PropVariantInit(&v);
    const uint32_t ms = SUCCEEDED(m->player->GetDuration(MFP_POSITIONTYPE_100NS, &v)) ? ToMs(v) : 0;
    PropVariantClear(&v);
    return ms;
}

void AudioPlayer::SeekMs(uint32_t ms)
{
    if (!m->player)
        return;
    PROPVARIANT v;
    PropVariantInit(&v);
    v.vt = VT_I8;
    v.hVal.QuadPart = static_cast<LONGLONG>(ms) * 10'000;
    m->player->SetPosition(MFP_POSITIONTYPE_100NS, &v);
    if (m->callback->ended) {
        m->callback->ended = false;
        m->player->Play();
    }
}

bool AudioPlayer::TakeFailure()
{
    if (m->callback && m->callback->failed.exchange(false))
        return true;
    return false;
}

} // namespace ps
