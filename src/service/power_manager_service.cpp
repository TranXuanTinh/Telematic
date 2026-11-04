#include "service/power_manager_service.h"

namespace tcu::service {

void PowerManagerService::acquireWakeLock(ipc::DaemonId holder, const std::string& reason) {
    std::lock_guard<std::mutex> lk(mtx_);
    wake_locks_[holder].insert(reason.empty() ? "default" : reason);
    if (state_ == PowerState::PRE_SLEEP || state_ == PowerState::SLEEP) {
        state_ = PowerState::ACTIVE_RUN;
    }
}

void PowerManagerService::releaseWakeLock(ipc::DaemonId holder, const std::string& reason) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = wake_locks_.find(holder);
    if (it == wake_locks_.end()) return;
    if (reason.empty()) {
        wake_locks_.erase(it);
    } else {
        it->second.erase(reason);
        if (it->second.empty()) {
            wake_locks_.erase(it);
        }
    }
}

bool PowerManagerService::hasActiveWakeLocks() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return !wake_locks_.empty();
}

uint8_t PowerManagerService::activeWakeLockBitmask() const {
    std::lock_guard<std::mutex> lk(mtx_);
    uint8_t mask = 0U;
    for (const auto& [did, reasons] : wake_locks_) {
        if (reasons.empty()) continue;
        switch (did) {
            case ipc::DaemonId::ECALLD: mask |= 0x01U; break;
            case ipc::DaemonId::SVTD:   mask |= 0x02U; break;
            case ipc::DaemonId::DIAGD:  mask |= 0x04U; break;
            case ipc::DaemonId::OTAD:   mask |= 0x08U; break;
            default:                    mask |= 0x10U; break;
        }
    }
    return mask;
}

void PowerManagerService::setPowerState(PowerState state, WakeupReason reason) {
    std::lock_guard<std::mutex> lk(mtx_);
    state_ = state;
    if (reason != WakeupReason::NONE) {
        wakeup_reason_ = reason;
    }
}

PowerState PowerManagerService::currentPowerState() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return state_;
}

WakeupReason PowerManagerService::lastWakeupReason() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return wakeup_reason_;
}

void PowerManagerService::beginSleepPreparation() {
    std::lock_guard<std::mutex> lk(mtx_);
    sleep_acks_.clear();
    state_ = PowerState::PRE_SLEEP;
}

void PowerManagerService::recordSleepReadyAck(ipc::DaemonId daemon) {
    std::lock_guard<std::mutex> lk(mtx_);
    sleep_acks_.insert(daemon);
}

bool PowerManagerService::allCoreDaemonsSleepReady() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return sleep_acks_.count(ipc::DaemonId::PLATFORMD) > 0 &&
           sleep_acks_.count(ipc::DaemonId::SVTD) > 0;
}

} // namespace tcu::service

