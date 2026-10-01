// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Bind and dial helpers shared by every transport, so they all follow one
// rule: resolve once, bind every resolved address with one listener each,
// and dial resolved addresses in order until one connects.
//
// Bind names come from /etc/hosts only, never DNS, and must map to this
// host's own addresses (see resolve_bind_addresses()). Dial names use the
// system resolver: a peer is authenticated by TLS, not by its address.

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
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace kythira::net_bind {

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

// One address a server listens on.
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

// Both wildcards, IPv4 then IPv6: what "*" binds.
inline auto any_endpoints() -> std::vector<bind_endpoint> {
    auto out = ipv4_any_endpoint();
    bind_endpoint ep;
    auto& a = reinterpret_cast<sockaddr_in6&>(ep.addr);
    a.sin6_family = AF_INET6;
    a.sin6_addr = in6addr_any;
    ep.len = sizeof(sockaddr_in6);
    out.push_back(ep);
    return out;
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

// Resolves the address a server listens on. "*" means both wildcards
// (0.0.0.0 and ::, two sockets), so dual-stack never depends on the
// net.ipv6.bindv6only sysctl. An IPv4 or IPv6 literal is used as given (an IPv6 one may carry a
// %zone). A host name is looked up only in the local hosts file
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
    if (address == "*") return any_endpoints();

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
        if (port == 0) {
            // An ephemeral port: every later address listens on the one the
            // kernel picked for the first, so the server has a single port.
            sockaddr_storage bound{};
            socklen_t bound_len = sizeof(bound);
            ::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &bound_len);
            port = ntohs(bound.ss_family == AF_INET6
                             ? reinterpret_cast<const sockaddr_in6&>(bound).sin6_port
                             : reinterpret_cast<const sockaddr_in&>(bound).sin_port);
        }
        ::listen(fd, 256);
        fds.push_back(fd);
    }
    if (fds.empty()) fail("no usable address family");
    return fds;
}

// One endpoint as a numeric address string, with "%<scope>" on a scoped
// IPv6 one: the host form libraries that bind by string (httplib, gRPC)
// take.
inline auto endpoint_host(const bind_endpoint& ep) -> std::string {
    char buf[INET6_ADDRSTRLEN] = {};
    if (ep.addr.ss_family == AF_INET6) {
        const auto& a = reinterpret_cast<const sockaddr_in6&>(ep.addr);
        ::inet_ntop(AF_INET6, &a.sin6_addr, buf, sizeof(buf));
        std::string out(buf);
        if (a.sin6_scope_id != 0) out += "%" + std::to_string(a.sin6_scope_id);
        return out;
    }
    const auto& a = reinterpret_cast<const sockaddr_in&>(ep.addr);
    ::inet_ntop(AF_INET, &a.sin_addr, buf, sizeof(buf));
    return buf;
}

// False when the kernel has no support for `family` (IPv6 disabled). A
// multi-address bind skips such an address rather than failing.
inline auto family_supported(int family) -> bool {
    int fd = ::socket(family, SOCK_STREAM, 0);
    if (fd < 0) return errno != EAFNOSUPPORT;
    ::close(fd);
    return true;
}

}  // namespace kythira::net_bind
