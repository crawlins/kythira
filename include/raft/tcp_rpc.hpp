#pragma once

#include <raft/exceptions.hpp>
#include <raft/future.hpp>
#include <raft/json_serializer.hpp>
#include <raft/network.hpp>
#include <raft/types.hpp>

#include <folly/executors/CPUThreadPoolExecutor.h>

#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace kythira {

// ── Wire framing helpers ─────────────────────────────────────────────────────
// Protocol: [4-byte big-endian payload length][UTF-8 JSON payload]

namespace tcp_detail {

inline auto write_all(int fd, const void* buf, std::size_t n) -> bool {
    const auto* p = static_cast<const char*>(buf);
    while (n > 0) {
        ssize_t w = ::write(fd, p, n);
        if (w <= 0) {
            return false;
        }
        p += w;
        n -= static_cast<std::size_t>(w);
    }
    return true;
}

inline auto read_all(int fd, void* buf, std::size_t n) -> bool {
    auto* p = static_cast<char*>(buf);
    while (n > 0) {
        ssize_t r = ::read(fd, p, n);
        if (r <= 0) {
            return false;
        }
        p += r;
        n -= static_cast<std::size_t>(r);
    }
    return true;
}

inline auto frame_send(int fd, std::string_view payload) -> bool {
    auto len = htonl(static_cast<std::uint32_t>(payload.size()));
    return write_all(fd, &len, 4) && write_all(fd, payload.data(), payload.size());
}

inline auto frame_recv(int fd) -> std::optional<std::string> {
    std::uint32_t net_len{};
    if (!read_all(fd, &net_len, 4)) {
        return std::nullopt;
    }
    std::uint32_t len = ntohl(net_len);
    if (len == 0 || len > 64u * 1024u * 1024u) {
        return std::nullopt;
    }
    std::string buf(len, '\0');
    if (!read_all(fd, buf.data(), len)) {
        return std::nullopt;
    }
    return buf;
}

// Open a TCP connection whose connect() phase is actually bounded by
// `timeout`, then leave the socket blocking (with SO_SNDTIMEO/SO_RCVTIMEO
// set) for the send()/recv() calls that follow.
//
// SO_SNDTIMEO/SO_RCVTIMEO do NOT bound the blocking connect() syscall
// itself on Linux — they only apply to send/recv on an already-connected
// socket. A plain blocking connect() to a host that's stopped responding
// entirely (e.g. a docker kill'd peer, once its address is no longer
// answering ARP/has no route) can block for however long the kernel's own
// TCP SYN retry timeout takes — often several seconds, far longer than
// this project's ~100ms RPC timeouts — regardless of what those socket
// options are set to. That, combined with node<Types>::replicate_to_followers()
// retrying every peer (including one that's permanently gone) on every
// heartbeat tick, can otherwise pile up many long-blocked connect() calls
// over time. Using a non-blocking connect + select()-with-timeout for just
// the connect phase makes this function's own `timeout` parameter an
// actual, enforced upper bound instead of a documented-but-unenforced one.
inline auto connect_to(const std::string& host, std::uint16_t port,
                       std::chrono::milliseconds timeout) -> int {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* res{};
    if (::getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0) {
        return -1;
    }

    int fd = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        ::freeaddrinfo(res);
        return -1;
    }

    int flags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    int rc = ::connect(fd, res->ai_addr, res->ai_addrlen);
    ::freeaddrinfo(res);

    if (rc != 0 && errno != EINPROGRESS) {
        ::close(fd);
        return -1;
    }

    if (rc != 0) {  // EINPROGRESS: wait for the socket to become writable, bounded by `timeout`.
        pollfd pfd{.fd = fd, .events = POLLOUT, .revents = 0};
        int poll_rc = ::poll(&pfd, 1, static_cast<int>(timeout.count()));
        if (poll_rc <= 0) {  // timed out or poll() itself failed
            ::close(fd);
            return -1;
        }
        int so_error = 0;
        socklen_t so_error_len = sizeof(so_error);
        if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &so_error_len) != 0 ||
            so_error != 0) {
            ::close(fd);
            return -1;
        }
    }

    ::fcntl(fd, F_SETFL, flags);  // restore blocking mode for send()/recv()

    timeval tv{};
    tv.tv_sec = static_cast<long>(timeout.count() / 1000);
    tv.tv_usec = static_cast<suseconds_t>((timeout.count() % 1000) * 1000);
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    return fd;
}

// Extract "type" field value from a JSON string without full parsing.
inline auto extract_type_field(const std::string& json) -> std::string {
    auto pos = json.find("\"type\"");
    if (pos == std::string::npos) {
        return {};
    }
    auto colon = json.find(':', pos + 6);
    if (colon == std::string::npos) {
        return {};
    }
    auto q1 = json.find('"', colon + 1);
    if (q1 == std::string::npos) {
        return {};
    }
    auto q2 = json.find('"', q1 + 1);
    if (q2 == std::string::npos) {
        return {};
    }
    return json.substr(q1 + 1, q2 - q1 - 1);
}

inline auto str_to_bytes(const std::string& s) -> std::vector<std::byte> {
    std::vector<std::byte> v(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        v[i] = static_cast<std::byte>(static_cast<unsigned char>(s[i]));
    }
    return v;
}

inline auto bytes_to_str(const std::vector<std::byte>& b) -> std::string {
    std::string s(b.size(), '\0');
    for (std::size_t i = 0; i < b.size(); ++i) {
        s[i] = static_cast<char>(b[i]);
    }
    return s;
}

template<typename NodeId> class peer_registry {
public:
    peer_registry() = default;
    peer_registry(peer_registry&& other) noexcept : _peers(std::move(other._peers)) {}
    peer_registry& operator=(peer_registry&&) = delete;
    peer_registry(const peer_registry&) = delete;
    peer_registry& operator=(const peer_registry&) = delete;

    void add_peer(NodeId id, std::string host, std::uint16_t port) {
        std::lock_guard lock(_mu);
        _peers[id] = {std::move(host), port};
    }
    auto lookup(NodeId id) const -> std::optional<std::pair<std::string, std::uint16_t>> {
        std::lock_guard lock(_mu);
        auto it = _peers.find(id);
        if (it == _peers.end()) {
            return std::nullopt;
        }
        return it->second;
    }

private:
    mutable std::mutex _mu;
    std::unordered_map<NodeId, std::pair<std::string, std::uint16_t>> _peers;
};

}  // namespace tcp_detail

// ── tcp_rpc_client ────────────────────────────────────────────────────────────
//
// Satisfies kythira::network_client.
// Makes one blocking TCP connection per RPC call, dispatched onto a small
// private thread pool so the calling thread gets a genuinely pending
// kythira::Future back immediately — callers that broadcast to multiple
// peers in a loop (node<Types>::start_election(),
// node<Types>::replicate_to_followers()) get real concurrent dispatch
// instead of stalling on peer N+1 until peer N's connect()-through-recv()
// (and its own retry_policy's up-to-3 attempts) fully finishes. This
// matters most for a peer that has gone away entirely (e.g. a killed
// process/container): SO_SNDTIMEO/SO_RCVTIMEO (set in connect_to()) do not
// bound the blocking connect() syscall itself on Linux, so reaching a dead
// peer can take far longer than the configured RPC timeout — with the old
// synchronous-inline call(), that alone was enough to blow through an
// election's whole timeout budget before a live peer was ever contacted.
//
// Deliberately a private, directly-constructed folly::CPUThreadPoolExecutor
// rather than folly::getGlobalCPUExecutor(): the global CPU executor is a
// registration-gated Folly singleton that aborts ("requested before
// registrationComplete()") unless folly::init() ran first, which only
// chaos_node's main.cpp (and other real binaries) does — every plain
// Boost.Test unit test that exercises tcp_rpc_client directly (e.g.
// tcp_rpc_unit_test) does not, and would crash on the first RPC.

class tcp_rpc_client {
public:
    using serializer_t = json_rpc_serializer<std::vector<std::byte>>;

    // RPC dispatch is network-I/O-bound, not CPU-bound, so a small fixed
    // pool is deliberately plenty even for clusters larger than this
    // project's typical 3-7 node scenario tests — it only needs to cover
    // "number of peers contacted in one broadcast round" worth of
    // concurrently in-flight blocking connect()/send()/recv() sequences.
    static constexpr std::size_t k_rpc_thread_pool_size = 8;

    tcp_rpc_client()
        : _executor(std::make_shared<folly::CPUThreadPoolExecutor>(k_rpc_thread_pool_size)) {}

    void add_peer(std::uint64_t id, std::string host, std::uint16_t port) {
        _peers.add_peer(id, std::move(host), port);
    }

    auto send_request_vote(std::uint64_t target, const request_vote_request<>& req,
                           std::chrono::milliseconds timeout) -> Future<request_vote_response<>> {
        return call<request_vote_response<>>(target, _ser.serialize(req), timeout,
                                             [this](const std::vector<std::byte>& d) {
                                                 return _ser.deserialize_request_vote_response(d);
                                             });
    }

    auto send_append_entries(std::uint64_t target, const append_entries_request<>& req,
                             std::chrono::milliseconds timeout)
        -> Future<append_entries_response<>> {
        return call<append_entries_response<>>(
            target, _ser.serialize(req), timeout, [this](const std::vector<std::byte>& d) {
                return _ser.deserialize_append_entries_response(d);
            });
    }

    auto send_install_snapshot(std::uint64_t target, const install_snapshot_request<>& req,
                               std::chrono::milliseconds timeout)
        -> Future<install_snapshot_response<>> {
        return call<install_snapshot_response<>>(
            target, _ser.serialize(req), timeout, [this](const std::vector<std::byte>& d) {
                return _ser.deserialize_install_snapshot_response(d);
            });
    }

private:
    template<typename Resp, typename Deser>
    auto call(std::uint64_t target, const std::vector<std::byte>& payload,
              std::chrono::milliseconds timeout, Deser deser) -> Future<Resp> {
        auto peer = _peers.lookup(target);
        if (!peer) {
            return FutureFactory::makeExceptionalFuture<Resp>(std::make_exception_ptr(
                network_exception("tcp_rpc_client: unknown peer " + std::to_string(target))));
        }

        // Dispatched onto the global CPU executor rather than run inline —
        // see the class comment above for why: this is what lets a caller
        // broadcasting to multiple peers in a loop move on to the next peer
        // immediately instead of blocking on this one's full
        // connect()-through-recv() sequence.
        Promise<Resp> promise;
        auto future = promise.getFuture();

        // Captured by value: `payload` is a reference to the caller's
        // temporary (e.g. _ser.serialize(req)), which does not outlive this
        // function; copying now, before dispatch, is required for the
        // background task to see valid data. `host`/`port` are similarly
        // copied out of `peer` (a pointer into _peers' storage) up front.
        std::string host = peer->first;
        std::uint16_t port = peer->second;

        _executor->add(
            [promise = std::move(promise), host, port, payload, timeout, deser]() mutable {
                int fd = tcp_detail::connect_to(host, port, timeout);
                if (fd < 0) {
                    promise.setException(std::make_exception_ptr(network_exception(
                        "tcp_rpc_client: connect failed to " + host + ":" + std::to_string(port))));
                    return;
                }

                struct Guard {
                    int fd;
                    ~Guard() {
                        if (fd >= 0) {
                            ::close(fd);
                        }
                    }
                } g{fd};

                if (!tcp_detail::frame_send(fd, tcp_detail::bytes_to_str(payload))) {
                    promise.setException(
                        std::make_exception_ptr(network_exception("tcp_rpc_client: send failed")));
                    return;
                }

                auto resp = tcp_detail::frame_recv(fd);
                if (!resp) {
                    promise.setException(
                        std::make_exception_ptr(network_exception("tcp_rpc_client: recv failed")));
                    return;
                }

                try {
                    promise.setValue(deser(tcp_detail::str_to_bytes(*resp)));
                } catch (...) {
                    promise.setException(std::current_exception());
                }
            });

        return future;
    }

    tcp_detail::peer_registry<std::uint64_t> _peers;
    serializer_t _ser;
    std::shared_ptr<folly::CPUThreadPoolExecutor> _executor;
};

// ── tcp_rpc_server ────────────────────────────────────────────────────────────
//
// Satisfies kythira::network_server.
// Accepts connections in a background thread; dispatches to registered handlers.

class tcp_rpc_server {
public:
    using rv_fn = std::function<request_vote_response<>(const request_vote_request<>&)>;
    using ae_fn = std::function<append_entries_response<>(const append_entries_request<>&)>;
    using is_fn = std::function<install_snapshot_response<>(const install_snapshot_request<>&)>;
    using serializer_t = json_rpc_serializer<std::vector<std::byte>>;

    explicit tcp_rpc_server(std::uint16_t port) : _port(port) {}

    ~tcp_rpc_server() { stop(); }

    tcp_rpc_server(const tcp_rpc_server&) = delete;
    tcp_rpc_server& operator=(const tcp_rpc_server&) = delete;
    tcp_rpc_server& operator=(tcp_rpc_server&&) = delete;

    // Move-only before start() is called (not safe to move a running server).
    tcp_rpc_server(tcp_rpc_server&& other) noexcept
        : _port(other._port),
          _listen_fd(other._listen_fd.load()),
          _running(other._running.load()),
          _accept_thread(std::move(other._accept_thread)),
          _rv(std::move(other._rv)),
          _ae(std::move(other._ae)),
          _is(std::move(other._is)),
          _ser(std::move(other._ser)) {
        other._listen_fd = -1;
        other._running = false;
    }

    void register_request_vote_handler(rv_fn h) { _rv = std::move(h); }
    void register_append_entries_handler(ae_fn h) { _ae = std::move(h); }
    void register_install_snapshot_handler(is_fn h) { _is = std::move(h); }

    void start() {
        if (_running.exchange(true)) {
            return;
        }

        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            _running = false;
            throw std::runtime_error("tcp_rpc_server: socket()");
        }

        int opt = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(_port);

        if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            ::close(fd);
            _running = false;
            throw std::runtime_error("tcp_rpc_server: bind() on port " + std::to_string(_port));
        }
        ::listen(fd, 256);
        _listen_fd = fd;
        _accept_thread = std::thread([this] { accept_loop(); });
    }

    void stop() {
        if (!_running.exchange(false)) {
            return;
        }
        int fd = _listen_fd.exchange(-1);
        if (fd >= 0) {
            ::shutdown(fd, SHUT_RDWR);
            ::close(fd);
        }
        if (_accept_thread.joinable()) {
            _accept_thread.join();
        }
    }

    [[nodiscard]] bool is_running() const noexcept { return _running.load(); }

private:
    void accept_loop() {
        while (_running) {
            int client = ::accept(_listen_fd.load(), nullptr, nullptr);
            if (client < 0) {
                break;
            }
            std::thread([this, client] {
                handle(client);
                ::close(client);
            }).detach();
        }
    }

    void handle(int fd) {
        auto data = tcp_detail::frame_recv(fd);
        if (!data) {
            return;
        }

        std::string type = tcp_detail::extract_type_field(*data);
        auto bytes = tcp_detail::str_to_bytes(*data);

        try {
            std::vector<std::byte> resp;
            if (type == "request_vote_request" && _rv) {
                resp = _ser.serialize(_rv(_ser.deserialize_request_vote_request(bytes)));
            } else if (type == "append_entries_request" && _ae) {
                resp = _ser.serialize(_ae(_ser.deserialize_append_entries_request(bytes)));
            } else if (type == "install_snapshot_request" && _is) {
                resp = _ser.serialize(_is(_ser.deserialize_install_snapshot_request(bytes)));
            } else {
                return;
            }
            tcp_detail::frame_send(fd, tcp_detail::bytes_to_str(resp));
        } catch (...) {
        }
    }

    std::uint16_t _port;
    std::atomic<int> _listen_fd{-1};
    std::atomic<bool> _running{false};
    std::thread _accept_thread;

    rv_fn _rv;
    ae_fn _ae;
    is_fn _is;
    serializer_t _ser;
};

// ── Concept assertions ────────────────────────────────────────────────────────

static_assert(kythira::network_client<tcp_rpc_client>,
              "tcp_rpc_client must satisfy network_client");
static_assert(kythira::network_server<tcp_rpc_server>,
              "tcp_rpc_server must satisfy network_server");

}  // namespace kythira
