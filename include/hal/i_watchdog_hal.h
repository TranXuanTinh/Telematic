#pragma once

#include <cstdint>
#include <string>

namespace tcu::hal {

/// Pure Virtual HAL for Linux Hardware Watchdog (/dev/watchdog) & systemd sd_notify.
class IWatchdogHAL {
public:
    virtual ~IWatchdogHAL() = default;

    virtual bool open(const std::string& device_path, int timeout_sec) = 0;
    virtual bool kick() = 0;
    virtual void notifySystemdReady() = 0;
    virtual void closeGracefully() = 0;
    virtual uint64_t kickCount() const = 0;
};

} // namespace tcu::hal

