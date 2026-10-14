// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Name resolution that gives up after a deadline.
//
// getaddrinfo() takes no timeout. How long it blocks is decided by
// resolv.conf and by whatever DNS server sits behind it, and for a name that
// does not exist that can be a long time. Rootless Podman is the case that
// matters here: aardvark-dns answers for running containers only and sends
// every other name upstream, so a peer whose container is stopped, killed or
// not started yet takes resolv.conf's full `timeout:2 attempts:3` per query
// to fail. Measured in rootless Podman 4.9 with netavark: 24 s for one
// getaddrinfo() on a stopped container's name, against 12 ms under Docker,
// whose embedded DNS answers NXDOMAIN for single-label names itself.
//
// Every transport resolves the peer it dials, per connection or on first
// contact. Blocking a Raft RPC for 24 s on a peer that is gone is already
// far past any RPC timeout, and the caller is usually holding something that
// other RPCs need: a slot in tcp_rpc_client's pool, or boost_beast_client's
// client-wide mutex. Bounding the wait by the RPC timeout is not enough on
// its own: tcp_rpc_client resolves on every RPC, its default AppendEntries
// timeout is 5 s, and with heartbeats every 100 ms to two dead peers all
// eight pool threads ended up parked on lookups, so live peers went without
// AppendEntries for 40 s and commits stalled (the dual-kill scenario).
//
// So a caller only ever waits for a name it has never resolved. Each answer
// is remembered, failures included, and handed out at once. When it is
// older than `max_age` the next caller still gets it immediately, and a
// lookup to replace it starts in the background. The lookup runs to
// completion on a detached thread, shared with every caller asking for the
// same name meanwhile, so a vanished peer costs one blocked thread rather
// than one per RPC, and stops being dialled as soon as its name stops
// resolving.
//
// One more aardvark-dns habit shapes what "failed" means. While it forwards
// a dead name upstream it answers nothing else on that network either:
// measured in the same setup, lookups of running containers' names failed
// (EAI_AGAIN after resolv.conf's 12 s) for the ~30 s it took aardvark to
// give up on two killed peers. A transient failure therefore never evicts
// an address that resolved before; only a definite "no such name" does.
// A peer that is really gone is still dropped the moment its name stops
// resolving, and a peer that moved is found as soon as a lookup gets
// through.
//
// Remembering an address cuts the other way for a peer that moved. Podman
// gives a restarted container a new address, so until a lookup gets
// through every dial of the remembered one sits in a connect timeout, and
// during an aardvark stall no lookup gets through for a while. The dialler
// knows better than the resolver: once every address a name resolved to
// has refused or timed out, connect_to() calls mark_unreachable() and the
// entry becomes suspect. A suspect entry is not handed out: callers wait
// (briefly, see below) for the lookup that replaces it, a transient
// failure now drops the address instead of keeping it, and the first
// lookup that succeeds brings the new one. glibc retries a query every
// `timeout` seconds for `attempts` rounds, so the one lookup in flight
// picks the name up as soon as it is back.
//
// Waiting for a cold name is capped at `default_cold_wait` regardless of
// the RPC timeout: the lookup keeps running and the next RPC picks up its
// answer, while the thread goes back to serving peers that do answer. Two
// learners joining during an aardvark stall otherwise parked four pool
// threads for the full 5 s AppendEntries timeout each.
//
// Finally, DNS is not the only way to learn where a peer is: a peer that
// sends us an RPC has told us its address. That matters because of one
// more aardvark-dns habit, measured with Podman 4.9's aardvark-dns 1.4:
// once it has forwarded a dead name upstream, and the upstream does not
// answer (the sandbox) or answers slowly (the CI runner), it answers that
// client nothing at all for 40 s or more, the new address of the
// restarted peer included, while a container started meanwhile resolves
// everything at once. So the leader cannot find a restarted follower by
// name in time, but the follower finds the leader and sends it pre-votes.
// learn() records the address an RPC came from as a hint for the name the
// client dials that peer by. A hint is used only when DNS has nothing
// usable (the name is suspect, or its last lookup failed) and only when it
// is newer than the last report that the name was unreachable, so a
// resolved address always wins and a stale hint cannot be dialled twice.

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace kythira::net_resolve {

// One address getaddrinfo() returned, copied out of its addrinfo list so it
// can outlive the lookup and be shared between waiters.
struct resolved_address {
    sockaddr_storage addr{};
    socklen_t len{0};
    int family{0};
    int socktype{0};
    int protocol{0};
};

namespace detail {

struct lookup {
    std::mutex mu;
    std::condition_variable cv;
    bool done{false};
    std::optional<std::vector<resolved_address>> result;
};

}  // namespace detail

// What one lookup found: the addresses, or why there are none. A transient
// failure (the server did not answer, or answered SERVFAIL) says nothing
// about whether the name exists; a definite one says it does not.
struct lookup_outcome {
    std::optional<std::vector<resolved_address>> addresses;
    bool transient{false};
};

namespace detail {

struct known_answer {
    std::optional<std::vector<resolved_address>> result;  // nullopt: it failed
    std::chrono::steady_clock::time_point at;
    // The dialler reported every address in `result` unreachable: do not
    // hand them out again, wait for the lookup that replaces them instead.
    bool suspect{false};
    // When the dialler last reported the name unreachable.
    std::chrono::steady_clock::time_point unreachable_at{};
    // Where an RPC from this peer last came from (learn()), and when. Used
    // only while DNS has no usable answer and only if newer than
    // `unreachable_at`.
    std::optional<std::vector<resolved_address>> hint;
    std::chrono::steady_clock::time_point hint_at{};
};

// Lookups in flight and the last answer for each key (name, service and
// socket type). Leaked on purpose: a detached lookup thread can outlive
// static destruction at exit.
struct registry {
    std::mutex mu;
    std::unordered_map<std::string, std::shared_ptr<lookup>> inflight;
    std::unordered_map<std::string, known_answer> known;
};

inline auto the_registry() -> registry& {
    static auto* r = new registry;  // NOLINT(cppcoreguidelines-owning-memory)
    return *r;
}

inline auto key_for(const std::string& host, const std::string& service, int socktype)
    -> std::string {
    return host + '\0' + service + '\0' + std::to_string(socktype);
}

inline auto run_getaddrinfo(const std::string& host, const std::string& service, int socktype,
                            int flags) -> lookup_outcome {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = socktype;
    hints.ai_flags = flags;
    addrinfo* res = nullptr;
    int rc = ::getaddrinfo(host.c_str(), service.c_str(), &hints, &res);
    if (rc != 0) {
        bool definite = rc == EAI_NONAME || rc == EAI_SERVICE
#ifdef EAI_NODATA
                        || rc == EAI_NODATA
#endif
#ifdef EAI_ADDRFAMILY
                        || rc == EAI_ADDRFAMILY
#endif
            ;
        return {std::nullopt, !definite};
    }
    std::vector<resolved_address> out;
    for (addrinfo* ai = res; ai != nullptr; ai = ai->ai_next) {
        if (ai->ai_addrlen > sizeof(sockaddr_storage)) {
            continue;
        }
        resolved_address a;
        std::memcpy(&a.addr, ai->ai_addr, ai->ai_addrlen);
        a.len = static_cast<socklen_t>(ai->ai_addrlen);
        a.family = ai->ai_family;
        a.socktype = ai->ai_socktype;
        a.protocol = ai->ai_protocol;
        out.push_back(a);
    }
    ::freeaddrinfo(res);
    if (out.empty()) {
        return {std::nullopt, false};
    }
    return {std::move(out), false};
}

}  // namespace detail

// How long an answer is handed out before a lookup to replace it starts.
// One second keeps a peer that died (or came back) from being dialled at
// its old address, or refused, for more than about a second plus however
// long the replacing lookup takes; a running peer costs one background
// lookup a second.
inline constexpr std::chrono::milliseconds default_max_age{1000};

// The most a caller waits for a name with no answer yet, whatever its RPC
// timeout. The lookup carries on and a later call collects its answer.
inline constexpr std::chrono::milliseconds default_cold_wait{1000};

// The dialler could reach none of the addresses `key` last resolved to.
// Its entry becomes suspect: resolve_using() stops handing the addresses
// out and waits for a fresh lookup instead, and a transient failure of
// that lookup drops them rather than keeping them. A key with no entry
// (an address literal, or a name never resolved) is left alone.
// `dialled_at` is when the failed dial began: a hint that arrived after it
// was never tried and stays usable, one from before it is retired.
inline auto mark_unreachable(const std::string& key,
                             std::chrono::steady_clock::time_point dialled_at) -> void {
    auto& reg = detail::the_registry();
    std::lock_guard lock(reg.mu);
    if (auto it = reg.known.find(key); it != reg.known.end()) {
        it->second.unreachable_at = std::max(it->second.unreachable_at, dialled_at);
        if (it->second.result) {
            it->second.suspect = true;
        }
    }
}

inline auto mark_unreachable(const std::string& key) -> void {
    mark_unreachable(key, std::chrono::steady_clock::now());
}

// An RPC from the peer `key` names arrived from `from`: remember it as the
// address to dial when DNS cannot say where the peer is (see the header
// comment). A key with no entry gets one holding only the hint, so a peer
// whose name has never resolved can still be reached once it speaks.
inline auto learn(const std::string& key, std::vector<resolved_address> from) -> void {
    auto& reg = detail::the_registry();
    std::lock_guard lock(reg.mu);
    auto& known = reg.known[key];
    if (known.result == std::nullopt && known.at == std::chrono::steady_clock::time_point{}) {
        // Fresh entry: an old `at` makes the first resolve() start a lookup.
        known.at = std::chrono::steady_clock::time_point::min();
    }
    known.hint = std::move(from);
    known.hint_at = std::chrono::steady_clock::now();
}

// `from` as an address to dial on `port`: the sender's host with the port
// it listens on, since an RPC comes from an ephemeral port. For learn().
inline auto address_from(const sockaddr_storage& from, socklen_t len, std::uint16_t port,
                         int socktype) -> std::optional<resolved_address> {
    resolved_address a;
    if (from.ss_family == AF_INET && len >= sizeof(sockaddr_in)) {
        std::memcpy(&a.addr, &from, sizeof(sockaddr_in));
        reinterpret_cast<sockaddr_in&>(a.addr).sin_port = htons(port);
        a.len = sizeof(sockaddr_in);
    } else if (from.ss_family == AF_INET6 && len >= sizeof(sockaddr_in6)) {
        std::memcpy(&a.addr, &from, sizeof(sockaddr_in6));
        reinterpret_cast<sockaddr_in6&>(a.addr).sin6_port = htons(port);
        a.len = sizeof(sockaddr_in6);
    } else {
        return std::nullopt;
    }
    a.family = from.ss_family;
    a.socktype = socktype;
    a.protocol = socktype == SOCK_STREAM ? IPPROTO_TCP : (socktype == SOCK_DGRAM ? IPPROTO_UDP : 0);
    return a;
}

inline auto mark_unreachable(
    const std::string& host, const std::string& service, int socktype,
    std::chrono::steady_clock::time_point dialled_at = std::chrono::steady_clock::now()) -> void {
    mark_unreachable(detail::key_for(host, service, socktype), dialled_at);
}

inline auto learn(const std::string& host, const std::string& service, int socktype,
                  resolved_address from) -> void {
    learn(detail::key_for(host, service, socktype), std::vector<resolved_address>{from});
}

// What resolve() runs off the calling thread: `lookup()` returning a
// lookup_outcome. Separate so a test can stand in a lookup that blocks or
// fails, which a real resolver only does on a misconfigured network.
template<typename Lookup>
auto resolve_using(const std::string& key, std::chrono::milliseconds timeout, Lookup lookup,
                   std::chrono::milliseconds max_age = default_max_age,
                   std::chrono::milliseconds cold_wait = default_cold_wait)
    -> std::optional<std::vector<resolved_address>> {
    std::shared_ptr<detail::lookup> l;
    bool start = false;
    std::optional<std::optional<std::vector<resolved_address>>> answer;
    {
        auto& reg = detail::the_registry();
        std::lock_guard lock(reg.mu);
        bool refresh = true;
        if (auto it = reg.known.find(key); it != reg.known.end()) {
            const auto& k = it->second;
            if (!k.suspect) {
                answer = k.result;
                refresh = std::chrono::steady_clock::now() - k.at >= max_age;
            }
            if ((k.suspect || !k.result) && k.hint && k.hint_at > k.unreachable_at) {
                answer = k.hint;  // DNS has nothing usable; the peer said where it is
            }
        }
        if (refresh) {
            auto& slot = reg.inflight[key];
            if (!slot) {
                slot = std::make_shared<detail::lookup>();
                start = true;
            }
            l = slot;
        }
    }

    if (start) {
        auto work = [l, key, lookup = std::move(lookup)] {
            lookup_outcome outcome = lookup();
            std::optional<std::vector<resolved_address>> result = std::move(outcome.addresses);
            {
                auto& reg = detail::the_registry();
                std::lock_guard lock(reg.mu);
                if (auto it = reg.inflight.find(key); it != reg.inflight.end() && it->second == l) {
                    reg.inflight.erase(it);
                }
                auto& known = reg.known[key];
                if (!result && outcome.transient && known.result && !known.suspect) {
                    result = known.result;  // keep the address that resolved before
                }
                known.result = result;
                known.at = std::chrono::steady_clock::now();
                known.suspect = false;
            }
            {
                std::lock_guard lock(l->mu);
                l->result = std::move(result);
                l->done = true;
            }
            l->cv.notify_all();
        };
        try {
            std::thread(work).detach();
        } catch (const std::system_error&) {
            work();  // no thread to spare: resolve here, unbounded, as before
        }
    }

    if (answer) {
        return *answer;
    }
    if (!start) {
        // A lookup is already running and someone is (or was) waiting on
        // it. Waiting here too would park this thread as well: with two
        // dead peers and two joining learners, callers parked one second
        // each on four names held all eight of tcp_rpc_client's pool
        // threads, and the live peers' heartbeats never got a thread. The
        // caller fails now and its retry collects the answer.
        std::lock_guard lock(l->mu);
        return l->done ? l->result : std::nullopt;
    }
    std::unique_lock lock(l->mu);
    auto wait = timeout;
    if (cold_wait.count() > 0 && (wait.count() <= 0 || wait > cold_wait)) {
        wait = cold_wait;
    }
    if (wait.count() > 0) {
        if (!l->cv.wait_for(lock, wait, [&] { return l->done; })) {
            return std::nullopt;
        }
    } else {
        l->cv.wait(lock, [&] { return l->done; });
    }
    return l->result;
}

// Every address `host` resolves to for `service`, in getaddrinfo() order, or
// nullopt when the name does not resolve. Address literals are converted on
// the calling thread and never touch DNS. A name answers at once from its
// last lookup, success or definite failure, refreshed in the background
// once it is older than default_max_age; a lookup that fails transiently
// leaves the last address in place, unless the dialler has reported it
// unreachable (mark_unreachable()). Only a name with no usable answer makes
// a caller wait, and only the caller that started the lookup, for at most
// the smaller of `timeout` and default_cold_wait; every other caller fails
// at once, the lookup keeps running, and a later call picks up its answer.
inline auto resolve(const std::string& host, const std::string& service, int socktype,
                    std::chrono::milliseconds timeout)
    -> std::optional<std::vector<resolved_address>> {
    if (auto literal = detail::run_getaddrinfo(host, service, socktype, AI_NUMERICHOST);
        literal.addresses) {
        return literal.addresses;
    }
    return resolve_using(
        detail::key_for(host, service, socktype), timeout,
        [host, service, socktype] { return detail::run_getaddrinfo(host, service, socktype, 0); });
}

}  // namespace kythira::net_resolve
