#include "adb/DeviceMonitor.h"

#include "adb/AdbClient.h"
#include "util/Log.h"
#include "util/Utf8.h"

#include <algorithm>
#include <condition_variable>

namespace ps {

const DeviceEntry* DeviceSnapshot::Find(std::string_view serial) const
{
    auto it = std::ranges::find(entries, serial, &DeviceEntry::serial);
    return it != entries.end() ? &*it : nullptr;
}

const DeviceInfo* DeviceSnapshot::FindInfo(std::string_view serial) const
{
    auto it = info.find(serial);
    return it != info.end() ? &it->second : nullptr;
}

FakeScenario ParseFakeScenario(std::string_view name)
{
    if (name == "noadb") return FakeScenario::NoAdb;
    if (name == "none") return FakeScenario::NoDevice;
    if (name == "unauthorized") return FakeScenario::Unauthorized;
    if (name == "one") return FakeScenario::OneDevice;
    if (name == "two") return FakeScenario::TwoDevices;
    if (name == "three") return FakeScenario::ThreeDevices;
    return FakeScenario::None;
}

namespace {

DeviceInfo FakeA54()
{
    DeviceInfo d;
    d.serial = "R5CW30A1B2C3";
    d.displayName = "Samsung Galaxy A54 5G";
    d.model = "SM-A546E";
    d.brand = "Samsung";
    d.android = "14";
    d.sdk = 34;
    d.oneUi = "6.1";
    d.securityPatch = "2026-09-01";
    d.batteryLevel = 64;
    d.charging = true;
    d.batteryTempC = 31.2;
    d.ramMarketingGB = 8;
    d.storage = StorageStats{ 128'000'000'000ull, 121'200'000'000ull, 6'800'000'000ull };
    return d;
}

DeviceInfo FakeGalaxyS9()
{
    DeviceInfo d;
    d.serial = "R38K40FAKE01";
    d.displayName = "Samsung SM-G960F";
    d.model = "SM-G960F";
    d.brand = "Samsung";
    d.android = "9";
    d.sdk = 28;
    d.oneUi = "1.0";
    d.securityPatch = "2021-11-01";
    d.batteryLevel = 100;
    d.batteryTempC = 32.1;
    d.ramMarketingGB = 6;
    d.storage = StorageStats{ 56'787'955'712ull, 54'330'122'240ull, 2'318'372'864ull };
    return d;
}

DeviceInfo FakePixel()
{
    DeviceInfo d;
    d.serial = "39121FDJH00XYZ";
    d.displayName = "Google Pixel 8";
    d.model = "Pixel 8";
    d.brand = "Google";
    d.android = "16";
    d.sdk = 36;
    d.securityPatch = "2026-10-05";
    d.batteryLevel = 23;
    d.batteryTempC = 28.4;
    d.ramMarketingGB = 8;
    d.storage = StorageStats{ 128'000'000'000ull, 71'000'000'000ull, 57'000'000'000ull };
    return d;
}

void AddFake(DeviceSnapshot& s, DeviceInfo d)
{
    DeviceEntry e;
    e.serial = d.serial;
    e.state = DeviceEntry::State::Device;
    e.rawState = "device";
    e.model = d.model;
    s.entries.push_back(e);
    s.info.emplace(d.serial, std::move(d));
}

} // namespace

DeviceMonitor::DeviceMonitor(std::shared_ptr<const AdbClient> adb, std::chrono::milliseconds interval)
    : m_adb(std::move(adb))
    , m_interval(interval)
{
    m_snapshot.adbFound = m_adb != nullptr;
    if (m_adb)
        m_snapshot.adbPath = Narrow(m_adb->ExePath().wstring());
    m_thread = std::jthread([this](std::stop_token st) { Run(st); });
}

DeviceMonitor::DeviceMonitor(FakeScenario fake)
{
    DeviceSnapshot s;
    s.adbFound = fake != FakeScenario::NoAdb;
    s.adbVersion = s.adbFound ? "37.0.1" : "";
    s.adbPath = s.adbFound ? "C:\\platform-tools\\adb.exe" : "";
    switch (fake) {
    case FakeScenario::Unauthorized: {
        DeviceEntry e;
        e.serial = "R5CW30A1B2C3";
        e.state = DeviceEntry::State::Unauthorized;
        e.rawState = "unauthorized";
        s.entries.push_back(e);
        break;
    }
    case FakeScenario::OneDevice: AddFake(s, FakeA54()); break;
    case FakeScenario::ThreeDevices: AddFake(s, FakePixel()); [[fallthrough]];
    case FakeScenario::TwoDevices:
        AddFake(s, FakeA54());
        AddFake(s, FakeGalaxyS9());
        break;
    default: break;
    }
    s.generation = 1;
    m_snapshot = std::move(s);
}

DeviceMonitor::~DeviceMonitor() = default;

DeviceSnapshot DeviceMonitor::Snapshot() const
{
    std::lock_guard lock(m_mutex);
    return m_snapshot;
}

uint64_t DeviceMonitor::Generation() const
{
    std::lock_guard lock(m_mutex);
    return m_snapshot.generation;
}

void DeviceMonitor::RequestInfoRefresh(std::string_view serial)
{
    std::lock_guard lock(m_mutex);
    m_refreshRequests.emplace_back(serial);
}

void DeviceMonitor::Publish(DeviceSnapshot snap)
{
    std::lock_guard lock(m_mutex);
    snap.generation = m_snapshot.generation + 1;
    m_snapshot = std::move(snap);
}

void DeviceMonitor::Run(std::stop_token st)
{
    DeviceSnapshot current;
    current.adbFound = true;
    current.adbPath = Narrow(m_adb->ExePath().wstring());
    current.adbVersion = m_adb->Version();
    LogInfo("adb found: {} (version {})", current.adbPath, current.adbVersion.empty() ? "?" : current.adbVersion);

    // Start the server explicitly so the first `devices` call doesn't pay for it
    // (and its "daemon started" chatter stays out of parsed output).
    m_adb->Run({ "start-server" }, { .timeout = 20s, .cancel = st });
    Publish(current);

    std::mutex waitMutex;
    std::condition_variable_any cv;

    while (!st.stop_requested()) {
        const AdbResult r = m_adb->Run({ "devices", "-l" }, { .timeout = 10s, .cancel = st });
        if (st.stop_requested())
            break;

        DeviceSnapshot next = current;
        if (r.ok()) {
            next.lastError.clear();
            next.entries = ParseDevicesList(r.out);
        } else {
            next.lastError = r.err.empty() ? std::string("adb devices failed: ") + ToString(r.status) : r.err;
            next.entries.clear();
        }

        std::vector<std::string> refresh;
        {
            std::lock_guard lock(m_mutex);
            refresh.swap(m_refreshRequests);
        }

        // Drop info for devices that left or lost authorization; fetch for new ones.
        std::erase_if(next.info, [&](const auto& kv) {
            const auto* e = next.Find(kv.first);
            return !e || e->state != DeviceEntry::State::Device;
        });
        for (const auto& e : next.entries) {
            if (e.state != DeviceEntry::State::Device)
                continue;
            const bool wanted = std::ranges::find(refresh, e.serial) != refresh.end();
            if (next.info.contains(e.serial) && !wanted)
                continue;
            if (auto info = QueryDeviceInfo(*m_adb, e.serial)) {
                LogInfo("Device ready: {} ({}, Android {})", info->displayName, MaskSerial(e.serial), info->android);
                next.info[e.serial] = std::move(*info);
            }
        }

        const bool changed = next.entries != current.entries || next.lastError != current.lastError
            || next.info.size() != current.info.size() || !refresh.empty();
        if (changed) {
            for (const auto& e : next.entries)
                if (!current.Find(e.serial) || current.Find(e.serial)->state != e.state)
                    LogInfo("Device {}: {}", MaskSerial(e.serial), e.rawState);
            current = next;
            Publish(current);
        }

        std::unique_lock lock(waitMutex);
        cv.wait_for(lock, st, m_interval, [] { return false; });
    }
}

} // namespace ps
