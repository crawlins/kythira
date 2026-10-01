// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <raft/exceptions.hpp>
#include <raft/executor_default.hpp>
#include <raft/future_default.hpp>
#include <raft/json_serializer.hpp>
#include <raft/network.hpp>
#include <raft/types.hpp>

#include <arpa/inet.h>
#include <cctype>
#include <cerrno>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
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
// One connect attempt to an already-resolved address, bounded by `timeout`.
inline auto connect_one(const addrinfo& ai, std::chrono::milliseconds timeout) -> int {
    int fd = ::socket(ai.ai_family, ai.ai_socktype, ai.ai_protocol);
    if (fd < 0) {
        return -1;
    }

    int flags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    int rc = ::connect(fd, ai.ai_addr, ai.ai_addrlen);

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
    return fd;
}

// Tries every address `host` resolves to, in getaddrinfo() order, until one
// connects. Trying only the first broke a peer named "localhost": it
// usually resolves to ::1 first, and a server listening on 127.0.0.1 alone
// was then unreachable. `timeout` bounds the whole call; each attempt gets
// an equal share of what is left, so one address that silently drops SYNs
// cannot use up the budget before the others are tried.
inline auto connect_to(const std::string& host, std::uint16_t port,
                       std::chrono::milliseconds timeout) -> int {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* res{};
    if (::getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0) {
        return -1;
    }

    long candidates = 0;
    for (addrinfo* ai = res; ai != nullptr; ai = ai->ai_next) ++candidates;

    auto deadline = std::chrono::steady_clock::now() + timeout;
    int fd = -1;
    for (addrinfo* ai = res; ai != nullptr && fd < 0; ai = ai->ai_next, --candidates) {
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (remaining.count() <= 0) {
            break;
        }
        fd = connect_one(*ai, std::max(remaining / candidates, std::chrono::milliseconds(1)));
    }
    ::freeaddrinfo(res);
    if (fd < 0) {
        return -1;
    }

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

// One address an RPC server listens on.
struct bind_endpoint {
    sockaddr_storage addr{};
    socklen_t len{0};
};

inline auto endpoint_is_loopback(const bind_endpoint& ep) -> bool {
    if (ep.addr.ss_family == AF_INET) {
        const auto& a = reinterpret_cast<const sockaddr_in&>(ep.addr);
        return (ntohl(a.sin_addr.s_addr) >> 24) == 127;
    }
    if (ep.addr.ss_family == AF_INET6) {
        const auto& a = reinterpret_cast<const sockaddr_in6&>(ep.addr);
        return IN6_IS_ADDR_LOOPBACK(&a.sin6_addr);
    }
    return false;
}

// The wildcard IPv4 address: what both RPC servers bound before they took a
// bind address at all.
inline auto ipv4_any_endpoint() -> std::vector<bind_endpoint> {
    bind_endpoint ep;
    auto& a = reinterpret_cast<sockaddr_in&>(ep.addr);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    ep.len = sizeof(sockaddr_in);
    return {ep};
}

// Every IPv4/IPv6 address assigned to an interface on this host.
inline auto local_interface_addresses() -> std::vector<bind_endpoint> {
    std::vector<bind_endpoint> out;
    ifaddrs* ifs = nullptr;
    if (::getifaddrs(&ifs) != 0) return out;
    for (ifaddrs* i = ifs; i != nullptr; i = i->ifa_next) {
        if (i->ifa_addr == nullptr) continue;
        bind_endpoint ep;
        if (i->ifa_addr->sa_family == AF_INET) {
            ep.len = sizeof(sockaddr_in);
        } else if (i->ifa_addr->sa_family == AF_INET6) {
            ep.len = sizeof(sockaddr_in6);
        } else {
            continue;
        }
        std::memcpy(&ep.addr, i->ifa_addr, ep.len);
        out.push_back(ep);
    }
    ::freeifaddrs(ifs);
    return out;
}

// Checks that every endpoint a host name resolved to is an address of this
// host: loopback, or assigned to one of `locals`. A link-local IPv6 match
// with no zone takes the interface's scope id, which bind() needs. Throws std::invalid_argument
// naming the first address that belongs to some other host.
inline auto require_local_endpoints(std::vector<bind_endpoint>& eps,
                                    const std::vector<bind_endpoint>& locals,
                                    const std::string& address, const char* who) -> void {
    for (auto& ep : eps) {
        if (endpoint_is_loopback(ep)) continue;
        bool found = false;
        for (const auto& l : locals) {
            if (l.addr.ss_family != ep.addr.ss_family) continue;
            if (ep.addr.ss_family == AF_INET) {
                found = reinterpret_cast<const sockaddr_in&>(l.addr).sin_addr.s_addr ==
                        reinterpret_cast<const sockaddr_in&>(ep.addr).sin_addr.s_addr;
            } else {
                const auto& la = reinterpret_cast<const sockaddr_in6&>(l.addr);
                auto& ea = reinterpret_cast<sockaddr_in6&>(ep.addr);
                found = std::memcmp(&la.sin6_addr, &ea.sin6_addr, sizeof(in6_addr)) == 0;
                if (found && ea.sin6_scope_id == 0) ea.sin6_scope_id = la.sin6_scope_id;
            }
            if (found) break;
        }
        if (!found) {
            char buf[INET6_ADDRSTRLEN] = {};
            const void* raw = ep.addr.ss_family == AF_INET
                                  ? static_cast<const void*>(
                                        &reinterpret_cast<const sockaddr_in&>(ep.addr).sin_addr)
                                  : static_cast<const void*>(
                                        &reinterpret_cast<const sockaddr_in6&>(ep.addr).sin6_addr);
            ::inet_ntop(ep.addr.ss_family, raw, buf, sizeof(buf));
            throw std::invalid_argument(std::string(who) + ": bind address '" + address +
                                        "' resolves to " + buf +
                                        ", which is not an address of this host");
        }
    }
}

// Parses one address from a hosts file: an IPv4 or IPv6 literal, where an
// IPv6 one may carry a %zone (interface name or index).
inline auto parse_hosts_address(const std::string& text) -> std::optional<bind_endpoint> {
    bind_endpoint ep;
    auto& v4 = reinterpret_cast<sockaddr_in&>(ep.addr);
    if (::inet_pton(AF_INET, text.c_str(), &v4.sin_addr) == 1) {
        v4.sin_family = AF_INET;
        ep.len = sizeof(sockaddr_in);
        return ep;
    }
    ep = bind_endpoint{};
    auto& v6 = reinterpret_cast<sockaddr_in6&>(ep.addr);
    auto pct = text.find('%');
    if (::inet_pton(AF_INET6, text.substr(0, pct).c_str(), &v6.sin6_addr) != 1) {
        return std::nullopt;
    }
    if (pct != std::string::npos) {
        auto zone = text.substr(pct + 1);
        unsigned idx = ::if_nametoindex(zone.c_str());
        if (idx == 0 && !zone.empty() && std::all_of(zone.begin(), zone.end(), [](unsigned char c) {
                return std::isdigit(c);
            })) {
            idx = static_cast<unsigned>(std::stoul(zone));
        }
        if (idx == 0) return std::nullopt;
        v6.sin6_scope_id = idx;
    }
    v6.sin6_family = AF_INET6;
    ep.len = sizeof(sockaddr_in6);
    return ep;
}

// Every address `name` is listed under in the hosts file at `path`, in file
// order, without duplicates. Names match case-insensitively, a trailing dot
// is ignored, and anything after '#' is a comment. A missing or unreadable
// file yields no addresses.
inline auto hosts_file_addresses(std::string name, const std::string& path)
    -> std::vector<bind_endpoint> {
    auto lower = [](std::string v) {
        std::transform(v.begin(), v.end(), v.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (!v.empty() && v.back() == '.') v.pop_back();
        return v;
    };
    name = lower(std::move(name));
    std::vector<bind_endpoint> out;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        line = line.substr(0, line.find('#'));
        std::istringstream fields(line);
        std::string addr;
        if (!(fields >> addr)) continue;
        bool listed = false;
        for (std::string alias; fields >> alias;) {
            listed = listed || lower(alias) == name;
        }
        if (!listed) continue;
        auto ep = parse_hosts_address(addr);
        if (!ep) continue;
        bool duplicate = false;
        for (const auto& o : out) {
            duplicate =
                duplicate || (o.len == ep->len && std::memcmp(&o.addr, &ep->addr, ep->len) == 0);
        }
        if (!duplicate) out.push_back(*ep);
    }
    return out;
}

// Resolves the address an RPC server listens on. An IPv4 or IPv6 literal is
// used as given (an IPv6 one may carry a %zone). A host name is looked up only in the local hosts
// file
// (`hosts_path`, normally /etc/hosts), never in DNS, so whoever controls a
// resolver cannot choose what the listener binds. The server binds every
// address the name is listed under (one listener each, so "localhost"
// usually covers both 127.0.0.1 and ::1). "localhost" falls back to
// 127.0.0.1 and ::1 when the file does not list it (RFC 6761). Every
// address must be loopback or assigned to an interface on this host; an
// entry pointing anywhere else is refused.
inline auto resolve_bind_addresses(const std::string& address, const char* who,
                                   const std::string& hosts_path = "/etc/hosts")
    -> std::vector<bind_endpoint> {
    if (address.empty()) {
        throw std::invalid_argument(std::string(who) + ": empty bind address");
    }
    if (auto literal = parse_hosts_address(address)) return {*literal};

    auto out = hosts_file_addresses(address, hosts_path);
    if (out.empty()) {
        std::string lowered(address);
        std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (lowered == "localhost" || lowered == "localhost.") {
            out.push_back(*parse_hosts_address("127.0.0.1"));
            out.push_back(*parse_hosts_address("::1"));
        }
    }
    if (out.empty()) {
        throw std::invalid_argument(std::string(who) + ": bind address '" + address +
                                    "' is not an IP address and is not listed in " + hosts_path +
                                    " (DNS is not consulted for bind addresses)");
    }
    require_local_endpoints(out, local_interface_addresses(), address, who);
    return out;
}

// True when `address` is accepted by resolve_bind_addresses() and every
// address it binds is loopback.
inline auto is_loopback_bind_address(const std::string& address) -> bool {
    try {
        auto eps = resolve_bind_addresses(address, "is_loopback_bind_address");
        for (const auto& e : eps) {
            if (!endpoint_is_loopback(e)) return false;
        }
        return true;
    } catch (const std::invalid_argument&) {
        return false;
    }
}

// Opens one listening socket per endpoint on `port`. IPv6 sockets are
// IPV6_V6ONLY so "::" means IPv6 only and never also claims IPv4. When there
// are several endpoints (a host name), one whose address family the kernel
// does not support (IPv6 disabled while /etc/hosts still lists ::1) is
// skipped, as long as at least one listener opens. Any other failure closes
// the sockets already opened and throws std::runtime_error.
inline auto open_listeners(const std::vector<bind_endpoint>& endpoints, std::uint16_t port,
                           const char* who) -> std::vector<int> {
    std::vector<int> fds;
    auto fail = [&](const std::string& what) {
        for (int fd : fds) ::close(fd);
        throw std::runtime_error(std::string(who) + ": " + what + " on port " +
                                 std::to_string(port));
    };
    for (const auto& ep : endpoints) {
        int fd = ::socket(ep.addr.ss_family, SOCK_STREAM, 0);
        if (fd < 0 && errno == EAFNOSUPPORT && endpoints.size() > 1) continue;
        if (fd < 0) fail("socket()");
        int opt = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        bind_endpoint at = ep;
        if (at.addr.ss_family == AF_INET6) {
            ::setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &opt, sizeof(opt));
            reinterpret_cast<sockaddr_in6&>(at.addr).sin6_port = htons(port);
        } else {
            reinterpret_cast<sockaddr_in&>(at.addr).sin_port = htons(port);
        }
        if (::bind(fd, reinterpret_cast<const sockaddr*>(&at.addr), at.len) < 0) {
            ::close(fd);
            fail("bind()");
        }
        ::listen(fd, 256);
        fds.push_back(fd);
    }
    if (fds.empty()) fail("no usable address family");
    return fds;
}

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
// Deliberately a private, directly-constructed kythira::executor_default
// rather than a backend's global CPU executor (e.g. folly::
// getGlobalCPUExecutor()): Folly's global CPU executor is a
// registration-gated singleton that aborts ("requested before
// registrationComplete()") unless folly::init() ran first, which only
// chaos_node's main.cpp (and other real binaries) does — every plain
// Boost.Test unit test that exercises tcp_rpc_client directly (e.g.
// tcp_rpc_unit_test) does not, and would crash on the first RPC.
// kythira::executor_default is backend-selected via KYTHIRA_DEFAULT_FUTURE_
// BACKEND (KYTHIRA_FUTURE_BACKEND_STDEXEC/BOOST, else Folly), so this class
// no longer hard-requires Folly regardless of which backend is selected.

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
        : _executor(std::make_shared<kythira::executor_default>(k_rpc_thread_pool_size)) {}

    void add_peer(std::uint64_t id, std::string host, std::uint16_t port) {
        _peers.add_peer(id, std::move(host), port);
    }

    auto send_request_vote(std::uint64_t target, const request_vote_request<>& req,
                           std::chrono::milliseconds timeout)
        -> future_default<request_vote_response<>> {
        return call<request_vote_response<>>(target, _ser.serialize(req), timeout,
                                             [this](const std::vector<std::byte>& d) {
                                                 return _ser.deserialize_request_vote_response(d);
                                             });
    }

    // Satisfies kythira::network_client_with_pre_vote (`.kiro/specs/raft-pre-vote/`).
    auto send_request_pre_vote(std::uint64_t target, const request_pre_vote_request<>& req,
                               std::chrono::milliseconds timeout)
        -> future_default<request_pre_vote_response<>> {
        return call<request_pre_vote_response<>>(
            target, _ser.serialize(req), timeout, [this](const std::vector<std::byte>& d) {
                return _ser.deserialize_request_pre_vote_response(d);
            });
    }

    // Satisfies kythira::network_client_with_timeout_now (leadership transfer,
    // Ongaro's dissertation §3.10).
    auto send_timeout_now(std::uint64_t target, const timeout_now_request<>& req,
                          std::chrono::milliseconds timeout)
        -> future_default<timeout_now_response<>> {
        return call<timeout_now_response<>>(target, _ser.serialize(req), timeout,
                                            [this](const std::vector<std::byte>& d) {
                                                return _ser.deserialize_timeout_now_response(d);
                                            });
    }

    auto send_append_entries(std::uint64_t target, const append_entries_request<>& req,
                             std::chrono::milliseconds timeout)
        -> future_default<append_entries_response<>> {
        return call<append_entries_response<>>(
            target, _ser.serialize(req), timeout, [this](const std::vector<std::byte>& d) {
                return _ser.deserialize_append_entries_response(d);
            });
    }

    auto send_install_snapshot(std::uint64_t target, const install_snapshot_request<>& req,
                               std::chrono::milliseconds timeout)
        -> future_default<install_snapshot_response<>> {
        return call<install_snapshot_response<>>(
            target, _ser.serialize(req), timeout, [this](const std::vector<std::byte>& d) {
                return _ser.deserialize_install_snapshot_response(d);
            });
    }

private:
    template<typename Resp, typename Deser>
    auto call(std::uint64_t target, const std::vector<std::byte>& payload,
              std::chrono::milliseconds timeout, Deser deser) -> future_default<Resp> {
        auto peer = _peers.lookup(target);
        if (!peer) {
            return future_factory_default::makeExceptionalFuture<Resp>(std::make_exception_ptr(
                network_exception("tcp_rpc_client: unknown peer " + std::to_string(target))));
        }

        // Dispatched onto the global CPU executor rather than run inline —
        // see the class comment above for why: this is what lets a caller
        // broadcasting to multiple peers in a loop move on to the next peer
        // immediately instead of blocking on this one's full
        // connect()-through-recv() sequence.
        promise_default<Resp> promise;
        auto future = promise.getFuture();

        // Captured by value: `payload` is a reference to the caller's
        // temporary (e.g. _ser.serialize(req)), which does not outlive this
        // function; copying now, before dispatch, is required for the
        // background task to see valid data. `host`/`port` are similarly
        // copied out of `peer` (a pointer into _peers' storage) up front.
        std::string host = peer->first;
        std::uint16_t port = peer->second;

        _executor->submit(
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
    std::shared_ptr<kythira::executor_default> _executor;
};

// ── tcp_rpc_server ────────────────────────────────────────────────────────────
//
// Satisfies kythira::network_server.
// Accepts connections in a background thread; dispatches to registered handlers.

class tcp_rpc_server {
public:
    using rv_fn = std::function<request_vote_response<>(const request_vote_request<>&)>;
    using pv_fn = std::function<request_pre_vote_response<>(const request_pre_vote_request<>&)>;
    using tn_fn = std::function<timeout_now_response<>(const timeout_now_request<>&)>;
    using ae_fn = std::function<append_entries_response<>(const append_entries_request<>&)>;
    using is_fn = std::function<install_snapshot_response<>(const install_snapshot_request<>&)>;
    using serializer_t = json_rpc_serializer<std::vector<std::byte>>;

    explicit tcp_rpc_server(std::uint16_t port) : _port(port) {}

    // Listens on `bind_address` only instead of every IPv4 interface: an IPv4
    // or IPv6 literal, or a host name whose addresses all belong to this host
    // (see tcp_detail::resolve_bind_addresses). Throws std::invalid_argument
    // otherwise.
    tcp_rpc_server(std::uint16_t port, const std::string& bind_address)
        : _port(port), _binds(tcp_detail::resolve_bind_addresses(bind_address, "tcp_rpc_server")) {}

    ~tcp_rpc_server() { stop(); }

    tcp_rpc_server(const tcp_rpc_server&) = delete;
    tcp_rpc_server& operator=(const tcp_rpc_server&) = delete;
    tcp_rpc_server& operator=(tcp_rpc_server&&) = delete;

    // Move-only before start() is called (not safe to move a running server).
    tcp_rpc_server(tcp_rpc_server&& other) noexcept
        : _port(other._port),
          _binds(std::move(other._binds)),
          _listen_fds(std::move(other._listen_fds)),
          _running(other._running.load()),
          _accept_threads(std::move(other._accept_threads)),
          _rv(std::move(other._rv)),
          _pv(std::move(other._pv)),
          _ae(std::move(other._ae)),
          _is(std::move(other._is)),
          _ser(std::move(other._ser)) {
        other._running = false;
    }

    void register_request_vote_handler(rv_fn h) { _rv = std::move(h); }
    // Satisfies kythira::network_server_with_pre_vote (`.kiro/specs/raft-pre-vote/`).
    void register_request_pre_vote_handler(pv_fn h) { _pv = std::move(h); }
    // Satisfies kythira::network_server_with_timeout_now (leadership transfer).
    void register_timeout_now_handler(tn_fn h) { _tn = std::move(h); }
    void register_append_entries_handler(ae_fn h) { _ae = std::move(h); }
    void register_install_snapshot_handler(is_fn h) { _is = std::move(h); }

    void start() {
        if (_running.exchange(true)) {
            return;
        }
        std::lock_guard lock(_listen_mu);
        try {
            _listen_fds = tcp_detail::open_listeners(_binds, _port, "tcp_rpc_server");
        } catch (...) {
            _running = false;
            throw;
        }
        for (int fd : _listen_fds) {
            _accept_threads.emplace_back([this, fd] { accept_loop(fd); });
        }
    }

    void stop() {
        if (!_running.exchange(false)) {
            return;
        }
        std::lock_guard lock(_listen_mu);
        for (int fd : _listen_fds) {
            ::shutdown(fd, SHUT_RDWR);
            ::close(fd);
        }
        _listen_fds.clear();
        for (auto& t : _accept_threads) {
            if (t.joinable()) t.join();
        }
        _accept_threads.clear();
    }

    [[nodiscard]] bool is_running() const noexcept { return _running.load(); }

private:
    void accept_loop(int listen_fd) {
        while (_running) {
            int client = ::accept(listen_fd, nullptr, nullptr);
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
            } else if (type == "request_pre_vote_request" && _pv) {
                resp = _ser.serialize(_pv(_ser.deserialize_request_pre_vote_request(bytes)));
            } else if (type == "timeout_now_request" && _tn) {
                resp = _ser.serialize(_tn(_ser.deserialize_timeout_now_request(bytes)));
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
    std::vector<tcp_detail::bind_endpoint> _binds{tcp_detail::ipv4_any_endpoint()};
    std::mutex _listen_mu;  // guards _listen_fds/_accept_threads across start()/stop()
    std::vector<int> _listen_fds;
    std::atomic<bool> _running{false};
    std::vector<std::thread> _accept_threads;

    rv_fn _rv;
    pv_fn _pv;
    tn_fn _tn;
    ae_fn _ae;
    is_fn _is;
    serializer_t _ser;
};

// ── Concept assertions ────────────────────────────────────────────────────────

static_assert(kythira::network_client<tcp_rpc_client>,
              "tcp_rpc_client must satisfy network_client");
static_assert(kythira::network_server<tcp_rpc_server>,
              "tcp_rpc_server must satisfy network_server");
static_assert(kythira::network_client_with_pre_vote<tcp_rpc_client>,
              "tcp_rpc_client must satisfy network_client_with_pre_vote");
static_assert(kythira::network_server_with_pre_vote<tcp_rpc_server>,
              "tcp_rpc_server must satisfy network_server_with_pre_vote");

// The optional leadership-transfer extension (Ongaro's dissertation §3.10),
// without which the multi-Raft placement driver's `transfer_leader` operator
// and `scatter` are unavailable on this transport.
static_assert(kythira::network_client_with_timeout_now<tcp_rpc_client>,
              "tcp_rpc_client must satisfy network_client_with_timeout_now");
static_assert(kythira::network_server_with_timeout_now<tcp_rpc_server>,
              "tcp_rpc_server must satisfy network_server_with_timeout_now");

}  // namespace kythira
