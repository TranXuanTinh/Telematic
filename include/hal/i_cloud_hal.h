#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace tcu::hal {

/// Pure Virtual HAL for mTLS MQTT Cloud Messaging & Resumable HTTPS Range Downloads.
class ICloudHAL {
public:
    using CloudCommandCallback =
        std::function<void(const std::string& topic, const std::string& payload)>;
    using DownloadProgressCallback =
        std::function<bool(uint64_t downloaded_bytes, uint64_t total_bytes)>; // return false to pause/preempt

    virtual ~ICloudHAL() = default;

    virtual bool connect() = 0;
    virtual void disconnect() = 0;
    virtual bool isConnected() const = 0;

    virtual bool publish(const std::string& topic, const std::string& payload, int qos = 1) = 0;
    virtual void setCommandCallback(CloudCommandCallback cb) = 0;

    /// Resumable HTTPS Range (HTTP 206) or file:// asset download to dest_file_path.
    /// Automatically appends from existing file size if partial file is present.
    virtual bool downloadAssetResumable(const std::string& url,
                                        const std::string& dest_file_path,
                                        DownloadProgressCallback progress_cb = nullptr) = 0;
};

} // namespace tcu::hal

