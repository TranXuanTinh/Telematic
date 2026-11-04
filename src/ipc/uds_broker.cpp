#include "ipc/uds_broker.h"
#include "common/logger.h"

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace tcu::ipc {

UdsBroker::UdsBroker(std::string socket_path)
    : socket_path_(std::move(socket_path)) {}

UdsBroker::~UdsBroker() {
    stop();
}

bool UdsBroker::start() {
    if (running_.load()) {
        return true;
    }

    std::error_code ec;
    auto parent = std::filesystem::path(socket_path_).parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
    }
    ::unlink(socket_path_.c_str());

    listen_fd_ = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (listen_fd_ < 0) {
        common::Logger::error("UdsBroker failed to create AF_UNIX SOCK_SEQPACKET socket");
        return false;
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (socket_path_.size() >= sizeof(addr.sun_path)) {
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }
    std::strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);

    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }

    if (::listen(listen_fd_, 32) < 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }

    epoll_fd_ = ::epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd_ < 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }

    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = listen_fd_;
    ::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, listen_fd_, &ev);

    running_.store(true);
    worker_ = std::thread(&UdsBroker::eventLoop, this);
    return true;
}

void UdsBroker::stop() {
    if (!running_.exchange(false)) {
        return;
    }
    if (worker_.joinable()) {
        worker_.join();
    }
    {
        std::lock_guard<std::mutex> lk(mtx_);
        for (auto& [fd, conn] : clients_by_fd_) {
            (void)conn;
            ::close(fd);
        }
        clients_by_fd_.clear();
        fd_by_daemon_.clear();
        topic_subscribers_.clear();
    }
    if (epoll_fd_ >= 0) {
        ::close(epoll_fd_);
        epoll_fd_ = -1;
    }
    if (listen_fd_ >= 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
    ::unlink(socket_path_.c_str());
}

bool UdsBroker::sendPacket(int fd, const IpcMessage& msg) {
    auto raw = msg.serialize();
    ssize_t sent = ::send(fd, raw.data(), raw.size(), MSG_NOSIGNAL);
    return sent == static_cast<ssize_t>(raw.size());
}

void UdsBroker::disconnectClientLocked(int client_fd) {
    if (epoll_fd_ >= 0) {
        ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, client_fd, nullptr);
    }
    auto it = clients_by_fd_.find(client_fd);
    if (it != clients_by_fd_.end()) {
        auto dit = fd_by_daemon_.find(it->second.daemon_id);
        if (dit != fd_by_daemon_.end() && dit->second == client_fd) {
            fd_by_daemon_.erase(dit);
        }
        clients_by_fd_.erase(it);
    }
    for (auto it2 = rpc_return_fd_.begin(); it2 != rpc_return_fd_.end();) {
        if (it2->second == client_fd) {
            it2 = rpc_return_fd_.erase(it2);
        } else {
            ++it2;
        }
    }
    for (auto& [topic, subs] : topic_subscribers_) {
        (void)topic;
        subs.erase(client_fd);
    }
    ::close(client_fd);
}

void UdsBroker::eventLoop() {
    std::vector<uint8_t> rx_buf(kMaxIpcPacketBytes);
    epoll_event events[32];

    while (running_.load()) {
        int n = ::epoll_wait(epoll_fd_, events, 32, 50);
        if (n <= 0) {
            continue;
        }
        for (int i = 0; i < n; ++i) {
            int fd = events[i].data.fd;
            if (fd == listen_fd_) {
                while (true) {
                    int cfd = ::accept4(listen_fd_, nullptr, nullptr,
                                        SOCK_NONBLOCK | SOCK_CLOEXEC);
                    if (cfd < 0) {
                        break;
                    }
                    int sndbuf = 262144;
                    int rcvbuf = 262144;
                    ::setsockopt(cfd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
                    ::setsockopt(cfd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

                    ucred cred{};
                    socklen_t clen = sizeof(cred);
                    uint32_t pid = 0;
                    uint32_t uid = 0;
                    if (::getsockopt(cfd, SOL_SOCKET, SO_PEERCRED, &cred, &clen) == 0) {
                        pid = static_cast<uint32_t>(cred.pid);
                        uid = static_cast<uint32_t>(cred.uid);
                    }

                    epoll_event cev{};
                    cev.events = EPOLLIN | EPOLLRDHUP;
                    cev.data.fd = cfd;
                    ::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, cfd, &cev);

                    std::lock_guard<std::mutex> lk(mtx_);
                    clients_by_fd_[cfd] = ClientConn{cfd, DaemonId::UNKNOWN, pid, uid};
                }
            } else {
                if ((events[i].events & (EPOLLHUP | EPOLLERR | EPOLLRDHUP)) != 0U) {
                    // Drain any final queued packet before disconnecting
                    while (true) {
                        ssize_t r = ::recv(fd, rx_buf.data(), rx_buf.size(), 0);
                        if (r <= 0) break;
                        handleClientPacket(fd, rx_buf.data(), static_cast<size_t>(r));
                    }
                    std::lock_guard<std::mutex> lk(mtx_);
                    disconnectClientLocked(fd);
                    continue;
                }
                while (true) {
                    ssize_t r = ::recv(fd, rx_buf.data(), rx_buf.size(), 0);
                    if (r > 0) {
                        handleClientPacket(fd, rx_buf.data(), static_cast<size_t>(r));
                    } else if (r == 0) {
                        std::lock_guard<std::mutex> lk(mtx_);
                        disconnectClientLocked(fd);
                        break;
                    } else {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            break;
                        }
                        std::lock_guard<std::mutex> lk(mtx_);
                        disconnectClientLocked(fd);
                        break;
                    }
                }
            }
        }
    }
}

void UdsBroker::handleClientPacket(int client_fd, const uint8_t* buf, size_t len) {
    IpcMessage msg;
    if (!IpcMessage::deserialize(buf, len, msg)) {
        return;
    }

    std::lock_guard<std::mutex> lk(mtx_);
    auto cit = clients_by_fd_.find(client_fd);
    if (cit == clients_by_fd_.end()) {
        return;
    }

    const auto kind = static_cast<IpcMsgKind>(msg.header.kind);
    switch (kind) {
        case IpcMsgKind::HELLO_REGISTER: {
            auto did = static_cast<DaemonId>(msg.header.sender_id);
            cit->second.daemon_id = did;
            if (did != DaemonId::UNKNOWN && did != DaemonId::TCU_CTL &&
                did != DaemonId::TEST_NODE) {
                fd_by_daemon_[did] = client_fd;
            }
            break;
        }
        case IpcMsgKind::SUBSCRIBE_TOPIC: {
            topic_subscribers_[msg.header.topic_or_method].insert(client_fd);
            break;
        }
        case IpcMsgKind::PUBLISH_EVENT: {
            auto sit = topic_subscribers_.find(msg.header.topic_or_method);
            if (sit != topic_subscribers_.end()) {
                for (int sub_fd : sit->second) {
                    if (sub_fd != client_fd) {
                        sendPacket(sub_fd, msg);
                    }
                }
            }
            break;
        }
        case IpcMsgKind::RPC_REQUEST: {
            auto target = static_cast<DaemonId>(msg.header.target_id);
            auto tit = fd_by_daemon_.find(target);
            if (tit == fd_by_daemon_.end()) {
                IpcMessage err_resp;
                err_resp.header.kind = static_cast<uint8_t>(IpcMsgKind::RPC_RESPONSE);
                err_resp.header.status = static_cast<uint8_t>(IpcStatus::ERR_TARGET_OFFLINE);
                err_resp.header.topic_or_method = msg.header.topic_or_method;
                err_resp.header.sender_id = msg.header.target_id;
                err_resp.header.target_id = msg.header.sender_id;
                err_resp.header.corr_id = msg.header.corr_id;
                err_resp.payload = "{\"error\":\"target_offline\"}";
                sendPacket(client_fd, err_resp);
            } else {
                if (msg.header.sender_id == 0) {
                    msg.header.sender_id = (cit->second.daemon_id != DaemonId::UNKNOWN)
                        ? static_cast<uint16_t>(cit->second.daemon_id)
                        : static_cast<uint16_t>(DaemonId::TCU_CTL);
                }
                const uint64_t route_key =
                    (static_cast<uint64_t>(msg.header.sender_id) << 32U) |
                    static_cast<uint64_t>(msg.header.corr_id);
                rpc_return_fd_[route_key] = client_fd;
                sendPacket(tit->second, msg);
            }
            break;
        }
        case IpcMsgKind::RPC_RESPONSE: {
            const uint64_t route_key =
                (static_cast<uint64_t>(msg.header.target_id) << 32U) |
                static_cast<uint64_t>(msg.header.corr_id);
            auto rit = rpc_return_fd_.find(route_key);
            if (rit != rpc_return_fd_.end()) {
                int dst_fd = rit->second;
                rpc_return_fd_.erase(rit);
                sendPacket(dst_fd, msg);
            } else {
                auto target = static_cast<DaemonId>(msg.header.target_id);
                auto tit = fd_by_daemon_.find(target);
                if (tit != fd_by_daemon_.end()) {
                    sendPacket(tit->second, msg);
                }
            }
            break;
        }
    }
}

} // namespace tcu::ipc
