#pragma once

#include "ipc/ipc_protocol.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace tcu::ipc {

struct RpcResult {
    IpcStatus   status{IpcStatus::OK};
    std::string payload;

    bool ok() const noexcept { return status == IpcStatus::OK; }
};

using TopicCallback = std::function<void(TopicId, const std::string&)>;
using RpcHandler    = std::function<RpcResult(const std::string& req_payload)>;

/// Non-blocking multi-threaded Intra-SoC UDS IPC Client.
/// Separates socket RX from callback dispatch so handlers can invoke nested RPCs deadlock-free.
class IpcClient {
public:
    IpcClient(DaemonId self_id, std::string broker_socket_path);
    ~IpcClient();

    IpcClient(const IpcClient&) = delete;
    IpcClient& operator=(const IpcClient&) = delete;

    bool connect(int retry_timeout_ms = 2000);
    void disconnect();
    bool isConnected() const noexcept { return connected_.load(); }

    bool subscribe(TopicId topic, TopicCallback cb);
    bool publish(TopicId topic, const std::string& payload, bool urgent = false);

    void registerRpc(RpcMethodId method, RpcHandler handler);
    RpcResult callRpc(DaemonId target,
                      RpcMethodId method,
                      const std::string& req_payload,
                      int timeout_ms = 1500,
                      bool urgent = false);

private:
    struct PendingRpc {
        std::mutex              mtx;
        std::condition_variable cv;
        bool                    completed{false};
        RpcResult               result;
    };

    void rxLoop();
    void dispatchLoop();
    bool sendRaw(const IpcMessage& msg);

    DaemonId                                self_id_;
    std::string                             socket_path_;
    int                                     sock_fd_{-1};
    std::atomic<bool>                       running_{false};
    std::atomic<bool>                       connected_{false};
    std::atomic<uint32_t>                   next_corr_id_{1};

    std::mutex                              send_mtx_;
    std::thread                             rx_thread_;
    std::thread                             dispatch_thread_;

    std::mutex                              handlers_mtx_;
    std::map<TopicId, TopicCallback>        topic_callbacks_;
    std::map<RpcMethodId, RpcHandler>       rpc_handlers_;

    std::mutex                              dispatch_mtx_;
    std::condition_variable                 dispatch_cv_;
    std::deque<IpcMessage>                  dispatch_queue_;

    std::mutex                              pending_mtx_;
    std::map<uint32_t, std::shared_ptr<PendingRpc>> pending_rpcs_;
};

} // namespace tcu::ipc

