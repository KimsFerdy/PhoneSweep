#pragma once
#include "adb/Device.h"

#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ps {

class AdbClient;

struct DeviceSnapshot {
    bool adbFound = false;
    std::string adbPath;
    std::string adbVersion;
    std::vector<DeviceEntry> entries;
    std::map<std::string, DeviceInfo, std::less<>> info; // keyed by serial, only for State::Device
    std::string lastError;                               // adb devices failed, etc.
    uint64_t generation = 0;                             // bumps on every change

    const DeviceEntry* Find(std::string_view serial) const;
    const DeviceInfo* FindInfo(std::string_view serial) const;
};

// Fake scenarios for UI sign-off / screenshots without a phone.
enum class FakeScenario { None, NoAdb, NoDevice, Unauthorized, OneDevice, TwoDevices, ThreeDevices };
FakeScenario ParseFakeScenario(std::string_view name);

// Polls `adb devices -l` on a background thread (~1.5 s) and refreshes device
// info for every authorized phone. The UI reads Snapshot() each frame.
class DeviceMonitor {
public:
    DeviceMonitor(std::shared_ptr<const AdbClient> adb, std::chrono::milliseconds interval = std::chrono::milliseconds(1500));
    explicit DeviceMonitor(FakeScenario fake);
    ~DeviceMonitor();

    DeviceMonitor(const DeviceMonitor&) = delete;
    DeviceMonitor& operator=(const DeviceMonitor&) = delete;

    DeviceSnapshot Snapshot() const;
    uint64_t Generation() const;

    // Re-query info (battery, storage) for a device on the next poll.
    void RequestInfoRefresh(std::string_view serial);

private:
    void Run(std::stop_token st);
    void Publish(DeviceSnapshot snap);

    std::shared_ptr<const AdbClient> m_adb;
    std::chrono::milliseconds m_interval;

    mutable std::mutex m_mutex;
    DeviceSnapshot m_snapshot;
    std::vector<std::string> m_refreshRequests;

    std::jthread m_thread; // last: joins before members above are destroyed
};

} // namespace ps
