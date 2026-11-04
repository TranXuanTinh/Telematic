#pragma once

#include "common/types.h"
#include "ipc/ipc_protocol.h"

#include <map>
#include <mutex>
#include <set>
#include <string>

namespace tcu::service {

/// Manages WakeLocks across TCU Daemons and arbitrates PowerState transitions.
class PowerManagerService {
public:
    void acquireWakeLock(ipc::DaemonId holder, const std::string& reason);
    void releaseWakeLock(ipc::DaemonId holder, const std::string& reason = "");
    bool hasActiveWakeLocks() const;
    uint8_t activeWakeLockBitmask() const;

    void setPowerState(PowerState state, WakeupReason reason = WakeupReason::NONE);
    PowerState currentPowerState() const;
    WakeupReason lastWakeupReason() const;

    void beginSleepPreparation();
    void recordSleepReadyAck(ipc::DaemonId daemon);
    bool allCoreDaemonsSleepReady() const;

private:
    mutable std::mutex                                mtx_;
    PowerState                                        state_{PowerState::ACTIVE_RUN};
    WakeupReason                                      wakeup_reason_{WakeupReason::KL15_IGN_ON};
    std::map<ipc::DaemonId, std::set<std::string>>    wake_locks_;
    std::set<ipc::DaemonId>                           sleep_acks_;
};

} // namespace tcu::service

