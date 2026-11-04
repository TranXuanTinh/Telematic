#pragma once

#include "ipc/ipc_protocol.h"

#include <atomic>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>

namespace tcu::ipc {

/// Sequenced Unix Domain Socket (AF_UNIX SOCK_SEQPACKET) IPC Router.
/// Mediates Pub/Sub topics and point-to-point RPCs across TCU Linux daemons.
class UdsBroker {
public:
    explicit UdsBroker(std::string socket_path);
    ~UdsBroker();

    UdsBroker(const UdsBroker&) = delete;
    UdsBroker& operator=(const UdsBroker&) = delete;

    bool start();
    void stop();
    bool isRunning() const noexcept { return running_.load(); }
    const std::string& socketPath() const noexcept { return socket_path_; }

private:
    struct ClientConn {
        int       fd{-1};
        DaemonId  daemon_id{DaemonId::UNKNOWN};
        uint32_t  peer_pid{0};
        uint32_t  peer_uid{0};
    };

    void eventLoop();
    void handleClientPacket(int client_fd, const uint8_t* buf, size_t len);
    void disconnectClientLocked(int client_fd);
    static bool sendPacket(int fd, const IpcMessage& msg);

    std::string                         socket_path_;
    int                                 listen_fd_{-1};
    int                                 epoll_fd_{-1};
    std::atomic<bool>                   running_{false};
    std::thread                         worker_;

    std::mutex                          mtx_;
    std::map<int, ClientConn>           clients_by_fd_;
    std::map<DaemonId, int>             fd_by_daemon_;
    std::map<uint64_t, int>             rpc_return_fd_;
    std::map<uint16_t, std::set<int>>   topic_subscribers_;
};

} // namespace tcu::ipc
