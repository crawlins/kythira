// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file coap_io_probe.cpp
/// @brief Gate probe for `.kiro/specs/coap-client-event-driven-io/` task 1.
///
/// The event-driven client I/O design rests on six claims about libcoap
/// (design §1, L1-L6) that came from reading its source. This binary checks
/// each one against a real libcoap context, so the claims are measured on
/// every libcoap build the project links rather than trusted. It is not a
/// test of Kythira code and is not registered with ctest: it prints one line
/// per claim and is run by hand against each build, e.g.
///
///     cmake --build build --target coap_io_probe && build/tests/coap_io_probe
///
/// It needs only libcoap, so it also builds outside the tree against any
/// other libcoap, e.g. Ubuntu's:
///
///     g++ -std=c++23 tests/coap_io_probe.cpp $(pkg-config --cflags --libs libcoap-3-openssl)
///
/// The peer is a plain UDP socket owned by the probe, not a libcoap server,
/// so the probe controls exactly how many replies are queued on the client's
/// socket before the client is allowed to read any of them.
///
/// Exit status is 0 when every claim could be measured (whatever the
/// answers), 1 when the harness itself failed.

#include <coap3/coap.h>

#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <arpa/inet.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

struct probe_state {
    std::size_t dispatched{0};
    std::size_t nacks{0};
    bool all_on_caller_thread{true};
    bool all_inside_call{true};
    std::thread::id caller;
};

probe_state g_state;
thread_local bool t_inside_io_process = false;

auto on_response(coap_session_t*, const coap_pdu_t*, const coap_pdu_t*, const coap_mid_t)
    -> coap_response_t {
    ++g_state.dispatched;
    if (std::this_thread::get_id() != g_state.caller) {
        g_state.all_on_caller_thread = false;
    }
    if (!t_inside_io_process) {
        g_state.all_inside_call = false;
    }
    return COAP_RESPONSE_OK;
}

auto on_nack(coap_session_t*, const coap_pdu_t*, const coap_nack_reason_t, const coap_mid_t)
    -> void {
    ++g_state.nacks;
}

auto io_step(coap_context_t* ctx) -> void {
    t_inside_io_process = true;
    coap_io_process(ctx, COAP_IO_NO_WAIT);
    t_inside_io_process = false;
}

auto fd_readable(int fd, int timeout_ms) -> bool {
    pollfd p{fd, POLLIN, 0};
    return ::poll(&p, 1, timeout_ms) > 0 && (p.revents & POLLIN) != 0;
}

/// A UDP peer on 127.0.0.1 that answers requests only when told to.
class raw_peer {
public:
    raw_peer() {
        _fd = ::socket(AF_INET, SOCK_DGRAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ::bind(_fd, reinterpret_cast<sockaddr*>(&a), sizeof a);
        socklen_t len = sizeof a;
        ::getsockname(_fd, reinterpret_cast<sockaddr*>(&a), &len);
        _port = ntohs(a.sin_port);
        // Room for every reply the probe queues, so none is lost to a full
        // buffer on either side.
        int big = 4 << 20;
        ::setsockopt(_fd, SOL_SOCKET, SO_RCVBUF, &big, sizeof big);
        ::setsockopt(_fd, SOL_SOCKET, SO_SNDBUF, &big, sizeof big);
    }
    ~raw_peer() { ::close(_fd); }
    raw_peer(const raw_peer&) = delete;
    auto operator=(const raw_peer&) -> raw_peer& = delete;

    [[nodiscard]] auto port() const -> std::uint16_t { return _port; }

    /// Read requests until @p n have arrived or nothing more arrives within
    /// @p timeout_ms; build a reply for each. Returns how many arrived.
    auto collect(std::size_t n, int timeout_ms) -> std::size_t {
        std::size_t got_n = 0;
        while (got_n < n && fd_readable(_fd, timeout_ms)) {
            std::uint8_t buf[1500];
            sockaddr_in from{};
            socklen_t len = sizeof from;
            auto got = ::recvfrom(_fd, buf, sizeof buf, 0, reinterpret_cast<sockaddr*>(&from), &len);
            if (got < 4) {
                continue;
            }
            ++got_n;
            const std::uint8_t type = (buf[0] >> 4) & 0x3;
            const std::uint8_t tkl = buf[0] & 0xF;
            std::vector<std::uint8_t> r;
            if (type == 0 /* CON */) {
                // Piggybacked ACK: same message id, same token.
                r.push_back(static_cast<std::uint8_t>(0x40 | (2 << 4) | tkl));
                r.push_back(0x44);  // 2.04 Changed
                r.push_back(buf[2]);
                r.push_back(buf[3]);
            } else {
                // NON response: fresh message id, same token.
                r.push_back(static_cast<std::uint8_t>(0x40 | (1 << 4) | tkl));
                r.push_back(0x44);
                r.push_back(static_cast<std::uint8_t>(_next_mid >> 8));
                r.push_back(static_cast<std::uint8_t>(_next_mid & 0xFF));
                ++_next_mid;
            }
            r.insert(r.end(), buf + 4, buf + 4 + tkl);
            _pending.push_back({std::move(r), from});
        }
        return got_n;
    }

    /// Send every reply built so far, back to back.
    auto flush() -> void {
        for (auto& p : _pending) {
            ::sendto(_fd, p.reply.data(), p.reply.size(), 0, reinterpret_cast<sockaddr*>(&p.from),
                     sizeof p.from);
        }
        _pending.clear();
    }

    /// Read @p n requests, then answer all of them back to back.
    auto answer(std::size_t n) -> bool {
        if (collect(n, 2000) != n) {
            return false;
        }
        flush();
        return true;
    }

private:
    int _fd{-1};
    std::uint16_t _port{0};
    std::uint16_t _next_mid{0x1000};
    struct pending {
        std::vector<std::uint8_t> reply;
        sockaddr_in from;
    };
    std::vector<pending> _pending;
};

auto loopback(std::uint16_t port) -> coap_address_t {
    coap_address_t a;
    coap_address_init(&a);
    a.addr.sin.sin_family = AF_INET;
    a.addr.sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.addr.sin.sin_port = htons(port);
    a.size = sizeof(sockaddr_in);
    return a;
}

std::uint32_t g_token = 1;

auto send_request(coap_session_t* s, coap_pdu_type_t type) -> bool {
    coap_pdu_t* pdu = coap_pdu_init(type, COAP_REQUEST_CODE_POST, coap_new_message_id(s),
                                    coap_session_max_pdu_size(s));
    if (pdu == nullptr) {
        return false;
    }
    std::uint8_t tok[4];
    std::memcpy(tok, &g_token, sizeof tok);
    ++g_token;
    coap_add_token(pdu, sizeof tok, tok);
    return coap_send(s, pdu) != COAP_INVALID_MID;
}

struct drain_result {
    bool readable_before{false};
    std::size_t first_call_dispatches{0};
    std::size_t calls_to_drain{0};
    bool readable_while_backlog{true};  // every check while work remained
    bool readable_after{true};
};

/// Queue @p n replies on each of @p sessions' sockets, then drain them one
/// NO_WAIT call at a time, recording what each call dispatched.
auto drain_burst(coap_context_t* ctx, const std::vector<coap_session_t*>& sessions,
                 const std::vector<raw_peer*>& peers, std::size_t n, coap_pdu_type_t type, int fd)
    -> std::optional<drain_result> {
    for (auto* s : sessions) {
        for (std::size_t i = 0; i < n; ++i) {
            if (!send_request(s, type)) {
                return std::nullopt;
            }
        }
    }
    for (auto* p : peers) {
        if (!p->answer(n)) {
            return std::nullopt;
        }
    }
    // Let every reply land in the client's socket buffer before reading any.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    drain_result r;
    const std::size_t want = n * sessions.size();
    const std::size_t base = g_state.dispatched;
    r.readable_before = fd >= 0 ? fd_readable(fd, 0) : false;
    while (g_state.dispatched - base < want && r.calls_to_drain < want + 20) {
        const std::size_t before = g_state.dispatched;
        io_step(ctx);
        ++r.calls_to_drain;
        if (r.calls_to_drain == 1) {
            r.first_call_dispatches = g_state.dispatched - before;
        }
        if (fd >= 0 && g_state.dispatched - base < want && !fd_readable(fd, 0)) {
            r.readable_while_backlog = false;
        }
    }
    if (g_state.dispatched - base < want) {
        return std::nullopt;
    }
    // libcoap may still owe itself timer work (e.g. a CON exchange's cleanup);
    // give it one more step so "readable after" asks only about sockets.
    io_step(ctx);
    r.readable_after = fd >= 0 ? fd_readable(fd, 0) : false;
    return r;
}

auto yesno(bool b) -> const char* { return b ? "yes" : "no"; }

}  // namespace

auto main() -> int {
    coap_startup();
    coap_set_log_level(COAP_LOG_ERR);
    g_state.caller = std::this_thread::get_id();

    std::printf("libcoap package version : %s\n", coap_package_version());
    std::printf("libcoap build           : %s\n", coap_package_build());
#if LIBCOAP_VERSION >= 4003005U
    // Added in 4.3.5; on older builds the header macro above is all there is.
    std::printf("coap_epoll_is_supported : %d\n", coap_epoll_is_supported());
#endif

    coap_context_t* ctx = coap_new_context(nullptr);
    if (ctx == nullptr) {
        std::printf("FAIL: coap_new_context\n");
        return 1;
    }
    coap_register_response_handler(ctx, on_response);
    coap_register_nack_handler(ctx, on_nack);

    // L1 -------------------------------------------------------------------
    const int fd = coap_context_get_coap_fd(ctx);
    std::printf("\nL1 coap_context_get_coap_fd = %d\n", fd);

    raw_peer peer_a;
    raw_peer peer_b;
    raw_peer peer_c;
    auto addr_a = loopback(peer_a.port());
    coap_session_t* sa = coap_new_client_session(ctx, nullptr, &addr_a, COAP_PROTO_UDP);
    if (sa == nullptr) {
        std::printf("FAIL: session a\n");
        return 1;
    }

    // L2/L3: one socket, N replies queued, NON and CON ----------------------
    std::printf("\nL2/L3 one socket: replies queued, then one NO_WAIT call at a time\n");
    std::printf("  %-4s %-4s %-14s %-16s %-12s %-22s %-14s\n", "type", "N", "fd ready before",
                "1st call dispatch", "calls/drain", "fd ready w/ backlog", "fd ready after");
    for (auto type : {COAP_MESSAGE_NON}) {
        for (std::size_t n : {1u, 8u, 32u, 64u}) {
            auto r = drain_burst(ctx, {sa}, {&peer_a}, n, type, fd);
            if (!r) {
                std::printf("FAIL: burst n=%zu\n", n);
                return 1;
            }
            std::printf("  %-4s %-4zu %-14s %-16zu %-12zu %-22s %-14s\n",
                        type == COAP_MESSAGE_NON ? "NON" : "CON", n, yesno(r->readable_before),
                        r->first_call_dispatches, r->calls_to_drain,
                        yesno(r->readable_while_backlog), yesno(r->readable_after));
        }
    }

    // L3 per socket: three sessions (three sockets), N replies each ----------
    auto addr_b = loopback(peer_b.port());
    auto addr_c = loopback(peer_c.port());
    coap_session_t* sb = coap_new_client_session(ctx, nullptr, &addr_b, COAP_PROTO_UDP);
    coap_session_t* sc = coap_new_client_session(ctx, nullptr, &addr_c, COAP_PROTO_UDP);
    if (sb == nullptr || sc == nullptr) {
        std::printf("FAIL: sessions b/c\n");
        return 1;
    }
    std::printf("\nL3 three sockets, N NON replies queued on each\n");
    for (std::size_t n : {1u, 16u}) {
        auto r = drain_burst(ctx, {sa, sb, sc}, {&peer_a, &peer_b, &peer_c}, n, COAP_MESSAGE_NON,
                             fd);
        if (!r) {
            std::printf("FAIL: three-socket burst n=%zu\n", n);
            return 1;
        }
        std::printf("  N=%-3zu total=%-3zu first call dispatched %zu, %zu calls to drain\n", n,
                    3 * n, r->first_call_dispatches, r->calls_to_drain);
    }

    // L7: CON requests on one session, as send_rpc() sends them by default
    // (coap_transport_config::use_confirmable_messages). RFC 7252 NSTART
    // limits a session to one outstanding CON by default and libcoap holds
    // the rest back; send_rpc()'s shared per-peer session raises it to
    // max_concurrent_requests, which the second row reproduces.
    std::printf("\nL7 8 CON requests on one session (send_rpc's default message type)\n");
    for (std::uint16_t nstart : {std::uint16_t{0}, std::uint16_t{8}}) {
        raw_peer p;
        auto addr = loopback(p.port());
        coap_session_t* s = coap_new_client_session(ctx, nullptr, &addr, COAP_PROTO_UDP);
        if (nstart != 0) {
            coap_session_set_nstart(s, nstart);
        }
        const std::size_t n = 8;
        for (std::size_t i = 0; i < n; ++i) {
            send_request(s, COAP_MESSAGE_CON);
        }
        const std::size_t on_wire = p.collect(n, 100);
        const std::size_t base = g_state.dispatched;
        std::size_t steps = 0;
        std::size_t max_per_step = 0;
        std::size_t released_total = on_wire;
        while (g_state.dispatched - base < n && steps < 4 * n) {
            p.flush();
            (void)fd_readable(fd >= 0 ? fd : 0, 1000);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            const std::size_t before = g_state.dispatched;
            io_step(ctx);
            ++steps;
            max_per_step = std::max(max_per_step, g_state.dispatched - before);
            released_total += p.collect(n, 20);
        }
        std::printf("  nstart=%-7s on the wire before any step: %zu; %zu steps to complete %zu; "
                    "max dispatched by one step: %zu\n",
                    nstart == 0 ? "default" : std::to_string(nstart).c_str(), on_wire, steps,
                    g_state.dispatched - base, max_per_step);
        (void)released_total;
        coap_session_release(s);
    }

    // L4: coap_io_prepare_epoll() with and without a pending CON -------------
    std::printf("\nL4 coap_io_prepare_epoll(ctx, now)\n");
    for (int i = 0; i < 3; ++i) {
        io_step(ctx);
    }
    coap_tick_t now;
    coap_ticks(&now);
    const unsigned idle = coap_io_prepare_epoll(ctx, now);
    std::printf("  idle (nothing outstanding)        : %u ms\n", idle);

    // A CON to a peer that never answers: libcoap owns a retransmit timer.
    raw_peer silent;
    auto addr_s = loopback(silent.port());
    coap_session_t* ss = coap_new_client_session(ctx, nullptr, &addr_s, COAP_PROTO_UDP);
    send_request(ss, COAP_MESSAGE_CON);
    coap_ticks(&now);
    const unsigned pending = coap_io_prepare_epoll(ctx, now);
    std::printf("  CON outstanding, ack_timeout 2 s  : %u ms\n", pending);

    // Does the epoll fd itself become readable when that timer is due, with
    // no I/O at all? (libcoap arms a timerfd inside the epoll set.)
    if (fd >= 0) {
        const auto t0 = std::chrono::steady_clock::now();
        const bool woke = fd_readable(fd, 10000);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0)
                            .count();
        std::printf("  poll(fd, 10 s) with only the timer: %s after %lld ms\n",
                    woke ? "readable" : "timed out", static_cast<long long>(ms));
        io_step(ctx);
        coap_ticks(&now);
        std::printf("  after one step, next deadline     : %u ms\n",
                    coap_io_prepare_epoll(ctx, now));
    }

    // L5: a session created after the fd was first polled ---------------------
    std::printf("\nL5 session created after the first poll()\n");
    if (fd >= 0) {
        (void)fd_readable(fd, 0);
        raw_peer late;
        auto addr_l = loopback(late.port());
        coap_session_t* sl = coap_new_client_session(ctx, nullptr, &addr_l, COAP_PROTO_UDP);
        send_request(sl, COAP_MESSAGE_NON);
        late.answer(1);
        // Drain anything unrelated first (the silent peer's retransmits), so
        // only the late session's reply can make the fd readable.
        const std::size_t base = g_state.dispatched;
        const auto t0 = std::chrono::steady_clock::now();
        bool woke = false;
        while (std::chrono::steady_clock::now() - t0 < std::chrono::seconds(2)) {
            if (fd_readable(fd, 1000)) {
                woke = true;
                io_step(ctx);
                if (g_state.dispatched > base) {
                    break;
                }
            }
        }
        std::printf("  late session's reply woke poll(fd) and dispatched: %s\n",
                    yesno(woke && g_state.dispatched > base));
    } else {
        std::printf("  n/a (no fd)\n");
    }

    // L6 -------------------------------------------------------------------
    std::printf("\nL6 handlers ran on the calling thread: %s; inside coap_io_process: %s\n",
                yesno(g_state.all_on_caller_thread), yesno(g_state.all_inside_call));

    coap_free_context(ctx);
    coap_cleanup();
    return 0;
}
