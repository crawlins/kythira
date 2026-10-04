// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Connection handling shared by the three raw-socket servers:
// tcp_rpc_server, tls_tcp_rpc_server and the tcp_gossip listener. See
// .kiro/specs/tcp-rpc-server-hardening/.
//
// Each of those servers used to `break` out of its accept loop on any
// accept() error and start one detached std::thread per connection, with
// no cap, no deadline, and nothing that waited for the thread in stop().
// An unauthenticated peer could cut a node off for good: about a thousand
// idle connections exhaust the descriptor limit, accept() fails with
// EMFILE and the listener never accepts again; with a higher limit,
// std::thread's constructor throws and the process terminates. A handler
// still running when its server was destroyed read freed members.
//
// connection_tracker bounds how many connections are live and from where,
// enforces a deadline on every phase except the handler by shutting the
// socket down from a reaper thread, and lets stop() wait until the last
// connection thread is done with the server. run_accept_loop() is the
// accept loop all three servers share.

#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>

namespace kythira {

/// Connection limits shared by tcp_rpc_server, tls_tcp_rpc_server and the
/// tcp_gossip listener. See .kiro/specs/tcp-rpc-server-hardening/.
struct tcp_server_limits {
    /// accept() until the request frame is fully read; for TLS this
    /// includes the handshake and the trust-policy check. Bounds the whole
    /// phase, however often the peer sends a byte.
    std::chrono::milliseconds request_timeout{std::chrono::seconds{30}};
    /// Handler return until the reply frame is fully written.
    std::chrono::milliseconds reply_timeout{std::chrono::seconds{30}};
    /// Connections served at once; further ones are closed on arrival.
    std::size_t max_connections{256};
    /// Connections served at once from one source address (an IPv4-mapped
    /// IPv6 address counts as its IPv4 address).
    std::size_t max_connections_per_source{32};
};

struct tcp_server_connection_stats {
    std::size_t active_connections{0};
    std::uint64_t refused_global_limit{0};
    std::uint64_t refused_per_source_limit{0};
    std::uint64_t timed_out{0};
    std::uint64_t accept_errors{0};
};

/// Throws std::invalid_argument naming the first zero field.
inline void validate(const tcp_server_limits& limits, const char* who) {
    auto fail = [who](const char* field) {
        throw std::invalid_argument(std::string(who) + ": tcp_server_limits::" + field +
                                    " must be greater than zero");
    };
    if (limits.request_timeout.count() <= 0) fail("request_timeout");
    if (limits.reply_timeout.count() <= 0) fail("reply_timeout");
    if (limits.max_connections == 0) fail("max_connections");
    if (limits.max_connections_per_source == 0) fail("max_connections_per_source");
}

namespace tcp_detail {

// The numeric peer address of an accepted socket, without the port. An
// IPv4-mapped IPv6 address is folded to its IPv4 form, so a dual-stack
// listener counts one IPv4 client the same way an IPv4 listener does.
inline auto connection_source_key(const sockaddr_storage& ss) -> std::string {
    char buf[INET6_ADDRSTRLEN]{};
    if (ss.ss_family == AF_INET) {
        const auto& in = reinterpret_cast<const sockaddr_in&>(ss);
        ::inet_ntop(AF_INET, &in.sin_addr, buf, sizeof(buf));
        return buf;
    }
    if (ss.ss_family == AF_INET6) {
        const auto& in6 = reinterpret_cast<const sockaddr_in6&>(ss);
        if (IN6_IS_ADDR_V4MAPPED(&in6.sin6_addr)) {
            in_addr v4{};
            std::memcpy(&v4, &in6.sin6_addr.s6_addr[12], sizeof(v4));
            ::inet_ntop(AF_INET, &v4, buf, sizeof(buf));
            return buf;
        }
        ::inet_ntop(AF_INET6, &in6.sin6_addr, buf, sizeof(buf));
        return buf;
    }
    return "unknown";
}

enum class accept_error_class {
    retry,    // concerns only the connection being accepted: accept again now
    backoff,  // the process is short of a resource: wait, then accept again
    fatal,    // the listening socket itself is unusable
};

inline auto classify_accept_error(int err) -> accept_error_class {
    switch (err) {
        case EBADF:
        case EINVAL:
        case ENOTSOCK:
            return accept_error_class::fatal;
        case EMFILE:
        case ENFILE:
        case ENOBUFS:
        case ENOMEM:
            return accept_error_class::backoff;
        case EINTR:
        case EAGAIN:
#if EWOULDBLOCK != EAGAIN
        case EWOULDBLOCK:
#endif
        case ECONNABORTED:
        case EPROTO:
        case EPERM:
        // Linux passes pending network errors on the new socket through
        // accept(); accept(2) says to treat them like EAGAIN.
        case ENETDOWN:
        case ENOPROTOOPT:
        case EHOSTDOWN:
#ifdef ENONET
        case ENONET:
#endif
        case EHOSTUNREACH:
        case EOPNOTSUPP:
        case ENETUNREACH:
            return accept_error_class::retry;
        default:
            // Unknown: keep the listener, but do not spin on it.
            return accept_error_class::backoff;
    }
}

class connection_tracker : public std::enable_shared_from_this<connection_tracker> {
public:
    using clock = std::chrono::steady_clock;

    enum class phase {
        request,
        handler,
        reply
    };

    // One admitted connection. Move-only; destroying it releases the
    // connection's slot and closes its socket. It touches only the tracker,
    // which it keeps alive, never the server: so once shutdown_and_drain()
    // has seen the last ticket go, no connection thread can reach the
    // server again.
    class ticket {
    public:
        ticket(ticket&& other) noexcept
            : _tracker(std::move(other._tracker)), _id(other._id), _fd(other._fd) {
            other._fd = -1;
        }
        auto operator=(ticket&&) -> ticket& = delete;
        ticket(const ticket&) = delete;
        auto operator=(const ticket&) -> ticket& = delete;
        ~ticket() {
            if (_tracker) {
                _tracker->release(_id);
            }
        }

        // Starts `p`, with its deadline from the limits (none for the
        // handler phase, which the server cannot safely interrupt).
        void enter(phase p) {
            if (_tracker) {
                _tracker->enter(_id, p);
            }
        }

        [[nodiscard]] auto fd() const -> int { return _fd; }

    private:
        friend class connection_tracker;
        ticket(std::shared_ptr<connection_tracker> t, std::uint64_t id, int fd)
            : _tracker(std::move(t)), _id(id), _fd(fd) {}

        std::shared_ptr<connection_tracker> _tracker;
        std::uint64_t _id;
        int _fd;
    };

    // Use create(); the tracker must be owned by a shared_ptr.
    static auto create(tcp_server_limits limits, const char* who)
        -> std::shared_ptr<connection_tracker> {
        validate(limits, who);
        return std::shared_ptr<connection_tracker>(new connection_tracker(limits));
    }

    connection_tracker(const connection_tracker&) = delete;
    auto operator=(const connection_tracker&) -> connection_tracker& = delete;

    ~connection_tracker() {
        {
            std::lock_guard lock(_mu);
            _reaper_stop = true;
        }
        _reaper_cv.notify_all();
        if (_reaper.joinable()) {
            if (_reaper.get_id() == std::this_thread::get_id()) {
                _reaper.detach();
            } else {
                _reaper.join();
            }
        }
    }

    // Admits `fd`, accepted from `source`, in its request phase, or returns
    // nullopt (the caller closes fd) when the tracker is draining or either
    // limit is reached.
    auto admit(int fd, const sockaddr_storage& source) -> std::optional<ticket> {
        auto key = connection_source_key(source);
        std::lock_guard lock(_mu);
        if (_draining) {
            return std::nullopt;
        }
        if (_conns.size() >= _limits.max_connections) {
            ++_stats.refused_global_limit;
            return std::nullopt;
        }
        auto& from_source = _per_source[key];
        if (from_source >= _limits.max_connections_per_source) {
            ++_stats.refused_per_source_limit;
            return std::nullopt;
        }
        ++from_source;
        auto id = _next_id++;
        _conns.emplace(id, entry{fd, std::move(key), clock::now() + _limits.request_timeout});
        _reaper_cv.notify_all();
        return ticket(shared_from_this(), id, fd);
    }

    // Starts the reaper, or restarts it after shutdown_and_drain() so a
    // stopped server can be started again.
    void start_reaper() {
        std::lock_guard lock(_mu);
        _draining = false;
        if (_reaper.joinable()) {
            return;
        }
        _reaper_stop = false;
        _reaper = std::thread([this] { reap(); });
    }

    // Refuses new connections, shuts down every tracked socket so blocked
    // reads, writes and TLS handshakes return at once, stops the reaper,
    // and waits until every ticket has been destroyed. A connection in its
    // handler phase is waited for, not interrupted.
    void shutdown_and_drain() {
        std::thread reaper;
        {
            std::unique_lock lock(_mu);
            _draining = true;
            for (auto& [id, e] : _conns) {
                ::shutdown(e.fd, SHUT_RDWR);
            }
            _drain_cv.wait(lock, [this] { return _conns.empty(); });
            _reaper_stop = true;
            reaper = std::move(_reaper);
        }
        _reaper_cv.notify_all();
        if (reaper.joinable()) {
            reaper.join();
        }
    }

    void note_accept_error() {
        std::lock_guard lock(_mu);
        ++_stats.accept_errors;
    }

    [[nodiscard]] auto stats() const -> tcp_server_connection_stats {
        std::lock_guard lock(_mu);
        auto s = _stats;
        s.active_connections = _conns.size();
        return s;
    }

    [[nodiscard]] auto limits() const -> const tcp_server_limits& { return _limits; }

private:
    struct entry {
        int fd;
        std::string source;
        std::optional<clock::time_point> deadline;
        bool expired{false};
    };

    explicit connection_tracker(tcp_server_limits limits) : _limits(limits) {}

    void enter(std::uint64_t id, phase p) {
        std::lock_guard lock(_mu);
        auto it = _conns.find(id);
        if (it == _conns.end()) {
            return;
        }
        switch (p) {
            case phase::request:
                it->second.deadline = clock::now() + _limits.request_timeout;
                break;
            case phase::handler:
                it->second.deadline.reset();
                break;
            case phase::reply:
                it->second.deadline = clock::now() + _limits.reply_timeout;
                break;
        }
        _reaper_cv.notify_all();
    }

    // The fd is closed here, under the mutex, after its entry is erased:
    // the reaper and shutdown_and_drain() only shutdown() fds they find in
    // _conns under the same mutex, so neither can reach a closed or reused
    // descriptor.
    void release(std::uint64_t id) {
        std::lock_guard lock(_mu);
        auto it = _conns.find(id);
        if (it == _conns.end()) {
            return;
        }
        if (auto s = _per_source.find(it->second.source);
            s != _per_source.end() && --s->second == 0) {
            _per_source.erase(s);
        }
        ::close(it->second.fd);
        _conns.erase(it);
        _drain_cv.notify_all();
    }

    void reap() {
        std::unique_lock lock(_mu);
        while (!_reaper_stop) {
            auto now = clock::now();
            std::optional<clock::time_point> earliest;
            for (auto& [id, e] : _conns) {
                if (e.expired || !e.deadline) {
                    continue;
                }
                if (*e.deadline <= now) {
                    e.expired = true;
                    ++_stats.timed_out;
                    ::shutdown(e.fd, SHUT_RDWR);
                } else if (!earliest || *e.deadline < *earliest) {
                    earliest = e.deadline;
                }
            }
            if (earliest) {
                _reaper_cv.wait_until(lock, *earliest);
            } else {
                _reaper_cv.wait(lock);
            }
        }
    }

    const tcp_server_limits _limits;
    mutable std::mutex _mu;
    std::condition_variable _reaper_cv;
    std::condition_variable _drain_cv;
    std::unordered_map<std::uint64_t, entry> _conns;
    std::unordered_map<std::string, std::size_t> _per_source;
    std::uint64_t _next_id{0};
    bool _draining{false};
    bool _reaper_stop{false};
    std::thread _reaper;
    tcp_server_connection_stats _stats;
};

// Wakes an accept loop out of its back-off wait when the server stops.
class accept_backoff {
public:
    template<typename Running>
    void wait_for(std::chrono::milliseconds delay, const Running& running) {
        std::unique_lock lock(_mu);
        _cv.wait_for(lock, delay, [&] { return !running.load(); });
    }

    void wake() {
        {
            std::lock_guard lock(_mu);
        }
        _cv.notify_all();
    }

private:
    std::mutex _mu;
    std::condition_variable _cv;
};

// Accepts on `listen_fd` while `running` holds, admitting each connection
// to `tracker` and handing its ticket to `spawn`, which starts the thread
// that serves it. Exits only once `running` is cleared or on an error that
// means the listening socket itself is unusable; resource errors such as
// EMFILE back off from 10 ms, doubling to 1 s, by which time finished
// connections have usually freed descriptors. If `spawn` throws (a thread
// could not be started), the ticket it was given releases the slot and
// closes the socket, and the loop carries on.
template<typename Running, typename Spawn>
void run_accept_loop(int listen_fd, const Running& running, connection_tracker& tracker,
                     accept_backoff& backoff, Spawn&& spawn) {
    constexpr std::chrono::milliseconds k_min_delay{10};
    constexpr std::chrono::milliseconds k_max_delay{1000};
    auto delay = k_min_delay;
    while (running.load()) {
        sockaddr_storage src{};
        socklen_t len = sizeof(src);
#ifdef SOCK_CLOEXEC
        int fd = ::accept4(listen_fd, reinterpret_cast<sockaddr*>(&src), &len, SOCK_CLOEXEC);
#else
        int fd = ::accept(listen_fd, reinterpret_cast<sockaddr*>(&src), &len);
        if (fd >= 0) {
            ::fcntl(fd, F_SETFD, FD_CLOEXEC);
        }
#endif
        if (fd < 0) {
            int err = errno;
            auto cls = classify_accept_error(err);
            if (!running.load() || cls == accept_error_class::fatal) {
                break;
            }
            tracker.note_accept_error();
            if (cls == accept_error_class::backoff) {
                backoff.wait_for(delay, running);
                delay = std::min(delay * 2, k_max_delay);
            }
            continue;
        }
        delay = k_min_delay;
#if defined(SO_NOSIGPIPE) && !defined(MSG_NOSIGNAL)
        int one = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
        auto t = tracker.admit(fd, src);
        if (!t) {
            ::close(fd);
            continue;
        }
        try {
            spawn(std::move(*t));
        } catch (...) {
            // Whichever ticket object still owns the slot (the one `spawn`
            // received, or `t` if it was never moved from) releases it and
            // closes the socket.
        }
    }
}

}  // namespace tcp_detail
}  // namespace kythira
