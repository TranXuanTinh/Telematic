#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace tcu::hal {

struct DualBankSlotStatus {
    std::string active_slot{"rootfs.0"};     // "rootfs.0" (Slot A) or "rootfs.1" (Slot B)
    std::string inactive_slot{"rootfs.1"};
    std::string boot_primary{"rootfs.0"};
    bool        active_marked_good{true};
    bool        pending_post_boot_verify{false};
    uint32_t    rollback_count{0};
    std::string installed_version{"2.3.0"};
};

/// Pure Virtual HAL for Dual-Bank A/B RootFS Atomic Bootloader & RAUC Integration.
class IOtaBootHAL {
public:
    using InstallProgressCallback = std::function<void(int percent)>;

    virtual ~IOtaBootHAL() = default;

    virtual DualBankSlotStatus getSlotStatus() const = 0;

    /// Stream verified RAUC bundle into inactive slot and stage next boot order.
    virtual bool installBundleToInactiveSlot(const std::string& bundle_path,
                                             const std::string& target_version,
                                             InstallProgressCallback progress_cb = nullptr) = 0;

    /// Commit newly booted slot after post-boot BIST passes ("rauc status mark-good").
    virtual bool markCurrentSlotGood() = 0;

    /// Mark current slot bad and revert boot_primary back to healthy slot.
    virtual bool rollbackToPreviousSlot() = 0;

    /// Simulate or trigger controlled reboot into staged boot_primary slot.
    virtual bool triggerRebootIntoStagedSlot() = 0;
};

} // namespace tcu::hal

