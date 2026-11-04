#include "ipc/ipc_client.h"

#include <chrono>
#include <cstring>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace tcu::ipc {

IpcClient::IpcClient(DaemonId self_id, std::string broker_socket_path)
    : self_id_(self_id), socket_path_(std::move(broker_socket_path)) {}

IpcClient::~IpcClient() {
    disconnect();
}

bool IpcClient::connect(int retry_timeout_ms) {
    if (connected_.load()) {
        return true;
    }

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(retry_timeout_ms);

    while (true) {
        int fd = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
        if (fd < 0) {
            return false;
        }
        int sndbuf = 262144;
        int rcvbuf = 262144;
        ::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
        ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);

        if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
            sock_fd_ = fd;
            break;
        }
        ::close(fd);
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    running_.store(true);
    connected_.store(true);
    rx_thread_       = std::thread(&IpcClient::rxLoop, this);
    dispatch_thread_ = std::thread(&IpcClient::dispatchLoop, this);

    // Send HELLO_REGISTER
    IpcMessage hello;
    hello.header.kind      = static_cast<uint8_t>(IpcMsgKind::HELLO_REGISTER);
    hello.header.sender_id = static_cast<uint16_t>(self_id_);
    sendRaw(hello);

    // Re-subscribe any pre-registered topics
    std::lock_guard<std::mutex> lk(handlers_mtx_);
    for (const auto& [topic, cb] : topic_callbacks_) {
        (void)cb;
        IpcMessage sub;
        sub.header.kind            = static_cast<uint8_t>(IpcMsgKind::SUBSCRIBE_TOPIC);
        sub.header.sender_id       = static_cast<uint16_t>(self_id_);
        sub.header.topic_or_method = static_cast<uint16_t>(topic);
        sendRaw(sub);
    }
    return true;
}

void IpcClient::disconnect() {
    if (!running_.exchange(false)) {
        return;
    }
    connected_.store(false);
    if (sock_fd_ >= 0) {
        ::shutdown(sock_fd_, SHUT_RDWR);
    }
    dispatch_cv_.notify_all();
    {
        std::lock_guard<std::mutex> lk(pending_mtx_);
        for (auto& [cid, pr] : pending_rpcs_) {
            (void)cid;
            std::lock_guard<std::mutex> plk(pr->mtx);
            pr->result.status = IpcStatus::ERR_TARGET_OFFLINE;
            pr->completed = true;
            pr->cv.notify_all();
        }
        pending_rpcs_.clear();
    }
    if (rx_thread_.joinable()) {
        rx_thread_.join();
    }
    if (dispatch_thread_.joinable()) {
        dispatch_thread_.join();
    }
    if (sock_fd_ >= 0) {
        ::close(sock_fd_);
        sock_fd_ = -1;
    }
}

bool IpcClient::sendRaw(const IpcMessage& msg) {
    std::lock_guard<std::mutex> lk(send_mtx_);
    if (sock_fd_ < 0 || !connected_.load()) {
        return false;
    }
    auto raw = msg.serialize();
    ssize_t sent = ::send(sock_fd_, raw.data(), raw.size(), MSG_NOSIGNAL);
    return sent == static_cast<ssize_t>(raw.size());
}

bool IpcClient::subscribe(TopicId topic, TopicCallback cb) {
    {
        std::lock_guard<std::mutex> lk(handlers_mtx_);
        topic_callbacks_[topic] = std::move(cb);
    }
    if (connected_.load()) {
        IpcMessage sub;
        sub.header.kind            = static_cast<uint8_t>(IpcMsgKind::SUBSCRIBE_TOPIC);
        sub.header.sender_id       = static_cast<uint16_t>(self_id_);
        sub.header.topic_or_method = static_cast<uint16_t>(topic);
        return sendRaw(sub);
    }
    return true;
}

bool IpcClient::publish(TopicId topic, const std::string& payload, bool urgent) {
    IpcMessage msg;
    msg.header.kind            = static_cast<uint8_t>(IpcMsgKind::PUBLISH_EVENT);
    msg.header.flags           = urgent ? IPC_FLAG_URGENT : IPC_FLAG_NONE;
    msg.header.sender_id       = static_cast<uint16_t>(self_id_);
    msg.header.topic_or_method = static_cast<uint16_t>(topic);
    msg.payload                = payload;
    return sendRaw(msg);
}

void IpcClient::registerRpc(RpcMethodId method, RpcHandler handler) {
    std::lock_guard<std::mutex> lk(handlers_mtx_);
    rpc_handlers_[method] = std::move(handler);
}

RpcResult IpcClient::callRpc(DaemonId target,
                             RpcMethodId method,
                             const std::string& req_payload,
                             int timeout_ms,
                             bool urgent) {
    if (!connected_.load()) {
        return RpcResult{IpcStatus::ERR_TARGET_OFFLINE, ""};
    }

    const uint32_t corr_id = next_corr_id_.fetch_add(1);
    auto pending = std::make_shared<PendingRpc>();
    {
        std::lock_guard<std::mutex> lk(pending_mtx_);
        pending_rpcs_[corr_id] = pending;
    }

    IpcMessage req;
    req.header.kind            = static_cast<uint8_t>(IpcMsgKind::RPC_REQUEST);
    req.header.flags           = urgent ? IPC_FLAG_URGENT : IPC_FLAG_NONE;
    req.header.sender_id       = static_cast<uint16_t>(self_id_);
    req.header.target_id       = static_cast<uint16_t>(target);
    req.header.topic_or_method = static_cast<uint16_t>(method);
    req.header.corr_id         = corr_id;
    req.payload                = req_payload;

    if (!sendRaw(req)) {
        std::lock_guard<std::mutex> lk(pending_mtx_);
        pending_rpcs_.erase(corr_id);
        return RpcResult{IpcStatus::ERR_TARGET_OFFLINE, ""};
    }

    std::unique_lock<std::mutex> plk(pending->mtx);
    bool ok = pending->cv.wait_for(
        plk, std::chrono::milliseconds(timeout_ms), [&] { return pending->completed; });

    {
        std::lock_guard<std::mutex> lk(pending_mtx_);
        pending_rpcs_.erase(corr_id);
    }

    if (!ok) {
        return RpcResult{IpcStatus::ERR_TIMEOUT, ""};
    }
    return pending->result;
}

void IpcClient::rxLoop() {
    std::vector<uint8_t> buf(kMaxIpcPacketBytes);
    while (running_.load()) {
        pollfd pfd{};
        pfd.fd = sock_fd_;
        pfd.events = POLLIN;
        int pr = ::poll(&pfd, 1, 50);
        if (pr <= 0) {
            continue;
        }
        ssize_t n = ::recv(sock_fd_, buf.data(), buf.size(), 0);
        if (n <= 0) {
            connected_.store(false);
            break;
        }
        IpcMessage msg;
        if (!IpcMessage::deserialize(buf.data(), static_cast<size_t>(n), msg)) {
            continue;
        }

        const auto kind = static_cast<IpcMsgKind>(msg.header.kind);
        if (kind == IpcMsgKind::RPC_RESPONSE) {
            std::shared_ptr<PendingRpc> pending;
            {
                std::lock_guard<std::mutex> lk(pending_mtx_);
                auto it = pending_rpcs_.find(msg.header.corr_id);
                if (it != pending_rpcs_.end()) {
                    pending = it->second;
                }
            }
            if (pending) {
                std::lock_guard<std::mutex> plk(pending->mtx);
                pending->result.status  = static_cast<IpcStatus>(msg.header.status);
                pending->result.payload = std::move(msg.payload);
                pending->completed      = true;
                pending->cv.notify_all();
            }
        } else if (kind == IpcMsgKind::PUBLISH_EVENT || kind == IpcMsgKind::RPC_REQUEST) {
            std::lock_guard<std::mutex> dlk(dispatch_mtx_);
            if ((msg.header.flags & IPC_FLAG_URGENT) != 0U) {
                dispatch_queue_.push_front(std::move(msg));
            } else {
                dispatch_queue_.push_back(std::move(msg));
            }
            dispatch_cv_.notify_one();
        }
    }
}

void IpcClient::dispatchLoop() {
    while (running_.load()) {
        IpcMessage msg;
        {
            std::unique_lock<std::mutex> dlk(dispatch_mtx_);
            dispatch_cv_.wait(dlk, [&] {
                return !running_.load() || !dispatch_queue_.empty();
            });
            if (!running_.load() && dispatch_queue_.empty()) {
                break;
            }
            msg = std::move(dispatch_queue_.front());
            dispatch_queue_.pop_front();
        }

        const auto kind = static_cast<IpcMsgKind>(msg.header.kind);
        if (kind == IpcMsgKind::PUBLISH_EVENT) {
            TopicCallback cb;
            auto topic = static_cast<TopicId>(msg.header.topic_or_method);
            {
                std::lock_guard<std::mutex> hlk(handlers_mtx_);
                auto it = topic_callbacks_.find(topic);
                if (it != topic_callbacks_.end()) {
                    cb = it->second;
                }
            }
            if (cb) {
                cb(topic, msg.payload);
            }
        } else if (kind == IpcMsgKind::RPC_REQUEST) {
            RpcHandler handler;
            auto method = static_cast<RpcMethodId>(msg.header.topic_or_method);
            {
                std::lock_guard<std::mutex> hlk(handlers_mtx_);
                auto it = rpc_handlers_.find(method);
                if (it != rpc_handlers_.end()) {
                    handler = it->second;
                }
            }

            IpcMessage resp;
            resp.header.kind            = static_cast<uint8_t>(IpcMsgKind::RPC_RESPONSE);
            resp.header.topic_or_method = msg.header.topic_or_method;
            resp.header.sender_id       = static_cast<uint16_t>(self_id_);
            resp.header.target_id       = msg.header.sender_id;
            resp.header.corr_id         = msg.header.corr_id;

            if (!handler) {
                resp.header.status = static_cast<uint8_t>(IpcStatus::ERR_UNSUPPORTED);
                resp.payload       = "{\"error\":\"unsupported_method\"}";
            } else {
                RpcResult res = handler(msg.payload);
                resp.header.status = static_cast<uint8_t>(res.status);
                resp.payload       = std::move(res.payload);
            }
            sendRaw(resp);
        }
    }
}

} // namespace tcu::ipc

