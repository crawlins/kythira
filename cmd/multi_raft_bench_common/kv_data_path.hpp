// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file kv_data_path.hpp
/// @brief The client-facing key-value data path shared by `cmd/multi_raft_node`
///        (which serves it) and `cmd/multi_raft_bench` (which offers load to
///        it). `.kiro/specs/multi-raft-host-binary/` Requirement 2.
///
/// **Why this is in `cmd/` and not in `include/raft/`.** Requirement 6 asks
/// that this not become a product: the host is a measurement and testing host,
/// not a supported server, and a client-facing API in the library's public
/// headers is exactly how it would stop being one. It lives beside the two
/// binaries that use it, and the agreement test reaches it the way
/// `chaos_node`'s tests reach `http_control.hpp` — by include path.
///
/// **What is on the wire, and what is deliberately not invented.**
/// Requirement 2.3 says to reuse the existing serializer registry rather than
/// invent a wire format, and the strongest form of that is the one taken here:
/// **the request body IS the state-machine command**, produced by the same
/// `kv_put` / `kv_get` / `kv_del` in `tests/multi_raft_kv_workload.hpp` that
/// the in-process harness submits. The host never parses it — Kythira never
/// parses a command — it forwards the bytes to `submit_command`. So there is
/// exactly one encoding of the workload in this project, which is the property
/// Requirement 4.1 actually needs: a Tier C row and a Tier B row must differ in
/// the tier and in nothing else.
///
/// The registry supplies the media type that labels those bytes
/// (`Content-Type` / `Accept`), so a row can say which encoding it carried and
/// the two ends can disagree loudly rather than quietly. Everything else —
/// the routing key, the read kind, the not-leader answer — travels as HTTP's
/// own metadata (query string, status code, headers), which is a format this
/// project did not invent either.
///
/// **Not-leader is returned, never forwarded** (Requirement 2.2). A host that
/// does not lead the target shard answers 421 with the leader's id when it
/// knows one. Forwarding would move the routing cost inside the cluster, and
/// routing cost is a thing this project measures deliberately —
/// `.kiro/specs/multi-raft-performance/` Requirement 8.3 exists to price it and
/// four runs across three machines have bounded it at ≤22.1 µs. A cluster that
/// silently forwarded would make that measurement impossible to repeat at
/// Tier C.

#include <httplib.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace kythira::bench {

// ─────────────────────────────────────────────────────────────────────────────
// The surface
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Paths on the **data** port. Nothing else is served there.
///
/// Requirement 2.4: the data path is kept separate from any control or
/// fault-injection surface, and the separation is a second listening socket
/// rather than a path prefix. A prefix would still let a control request be
/// counted in a measured window by anything watching the port; a second socket
/// cannot.
struct data_path_routes {
    static constexpr std::string_view k_put = "/kv/put";
    static constexpr std::string_view k_get = "/kv/get";
    static constexpr std::string_view k_delete = "/kv/delete";
};

/// @brief Which of the three read kinds the caller wants.
///
/// Requirement 2.5. They differ by three orders of magnitude — this project
/// measured a local stale read at 0.8 µs against a linearizable point read at
/// 5940 µs — so a single default would make every read row meaningless.
enum class wire_read_kind : std::uint8_t {
    /// `multi_raft::read_state`: a linearizable read of the whole shard,
    /// paying a heartbeat quorum and no log entry.
    state = 0,
    /// A `GET` submitted as a proposal: a log entry and a replication round.
    /// The expensive one, and the reason `read_state` beating it is worth
    /// reporting.
    log = 1,
    /// This replica's state machine, read where it stands. No consensus, no
    /// freshness guarantee, and the cheapest thing the system can do.
    local = 2,
};

[[nodiscard]] inline auto to_string(wire_read_kind k) -> std::string_view {
    switch (k) {
        case wire_read_kind::state:
            return "state";
        case wire_read_kind::log:
            return "log";
        case wire_read_kind::local:
            return "local";
    }
    return "unknown";
}

[[nodiscard]] inline auto parse_read_kind(std::string_view s) -> std::optional<wire_read_kind> {
    if (s == "state") {
        return wire_read_kind::state;
    }
    if (s == "log") {
        return wire_read_kind::log;
    }
    if (s == "local") {
        return wire_read_kind::local;
    }
    return std::nullopt;
}

/// @brief HTTP status codes this path assigns meaning to.
///
/// 421 (Misdirected Request) for not-leader is the closest standard code to
/// what is actually true — the request reached a server that cannot answer it
/// and the client should retry elsewhere — and it is deliberately not 503,
/// which a load balancer would treat as a failing host rather than as a
/// correct answer.
struct data_path_status {
    static constexpr int k_ok = 200;
    static constexpr int k_no_such_key = 404;
    static constexpr int k_misdirected = 421;
    static constexpr int k_bad_request = 400;
    static constexpr int k_unavailable = 503;
};

/// @brief The header a misdirected answer carries the leader in.
///
/// A header rather than a body so that a client can act on it without decoding
/// anything, and so the answer is the same shape whatever serializer the row
/// is carrying. Absent when this host does not know who leads.
inline constexpr std::string_view k_leader_header = "X-Kythira-Leader";
/// The group whose range holds the key, when the host could resolve it.
inline constexpr std::string_view k_group_header = "X-Kythira-Group";

// ─────────────────────────────────────────────────────────────────────────────
// What a call came back as
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Why an operation did not complete, in the same taxonomy
///        `operation_tally` counts.
///
/// Deliberately the same five buckets the in-process harness uses, so a Tier C
/// row's failure column can be read beside a Tier B row's without a mapping
/// table in the reader's head.
enum class kv_outcome : std::uint8_t {
    ok = 0,
    not_leader = 1,
    timeout = 2,
    no_such_key = 3,
    other = 4,
};

struct kv_result {
    kv_outcome _outcome{kv_outcome::other};
    /// The value, for a read that found one. Empty otherwise.
    std::vector<std::byte> _value;
    /// Who leads the shard, if the answer said so. `nullopt` on any other
    /// outcome, and also on a not-leader answer from a host that does not know.
    std::optional<std::uint64_t> _leader;
    std::optional<std::uint64_t> _group;

    [[nodiscard]] auto ok() const -> bool { return _outcome == kv_outcome::ok; }
};

// ─────────────────────────────────────────────────────────────────────────────
// Encoding helpers
// ─────────────────────────────────────────────────────────────────────────────

[[nodiscard]] inline auto to_bytes(const std::string& s) -> std::vector<std::byte> {
    std::vector<std::byte> out;
    out.reserve(s.size());
    for (char c : s) {
        out.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
    }
    return out;
}

[[nodiscard]] inline auto to_string_body(const std::vector<std::byte>& b) -> std::string {
    std::string out;
    out.reserve(b.size());
    for (auto x : b) {
        out.push_back(static_cast<char>(x));
    }
    return out;
}

/// @brief Percent-encode a key for the query string.
///
/// The workload's keys are `kv_key(n)` — ASCII and already safe — but a key is
/// caller data and a data path that corrupts one class of key silently is
/// worse than one that is slower. Encoding everything outside the unreserved
/// set costs nothing at these lengths.
[[nodiscard]] inline auto percent_encode(std::string_view s) -> std::string {
    static constexpr std::string_view k_hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                                (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
                                c == '~';
        if (unreserved) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(k_hex[c >> 4]);
            out.push_back(k_hex[c & 0x0F]);
        }
    }
    return out;
}

// ─────────────────────────────────────────────────────────────────────────────
// The client
// ─────────────────────────────────────────────────────────────────────────────

/// @brief One connection to one host's data port.
///
/// Held per (worker, host) by the driver rather than shared, because
/// `httplib::Client` serialises requests on one connection and a shared client
/// would turn the driver's concurrency into a queue — which would be measured
/// as the cluster's latency. That is precisely the confound
/// `.kiro/specs/multi-raft-host-binary/` Requirement 3.1 exists to remove, so
/// reintroducing it inside the driver would be self-defeating.
class kv_data_client {
public:
    kv_data_client(const std::string& host, std::uint16_t port, std::string media_type,
                   std::chrono::milliseconds connect_timeout,
                   std::chrono::milliseconds request_timeout)
        : _client(host, port), _media_type(std::move(media_type)) {
        _client.set_connection_timeout(connect_timeout);
        _client.set_read_timeout(request_timeout);
        _client.set_write_timeout(request_timeout);
        _client.set_keep_alive(true);
        // **`TCP_NODELAY`, and it is worth 40 ms per request.** cpp-httplib
        // defaults `CPPHTTPLIB_TCP_NODELAY` to `false`, so Nagle's algorithm
        // holds a small write waiting for an ACK that the peer's delayed-ACK
        // timer will not send for 40 ms. The first Tier C row taken without
        // this measured 14.6 ops/sec at a 251 ms p50, with an inter-node probe
        // RTT of 41 ms on loopback — a number that describes the TCP stack and
        // not the cluster.
        //
        // Set here and NOT on `cpp_httplib_client`/`cpp_httplib_server` in
        // `include/`, deliberately: those carry Raft RPCs and are a swept axis
        // of `.kiro/specs/multi-raft-performance/`, so changing them would
        // silently move every Tier B row this project has published. That the
        // Raft transport has the same default is an observation for that spec,
        // recorded rather than acted on from here.
        _client.set_tcp_nodelay(true);
    }

    /// @brief Submit a write command for `key`.
    ///
    /// `command` is the state-machine command the shared workload produced —
    /// this function does not build one, which is the point.
    auto put(const std::string& key, const std::vector<std::byte>& command) -> kv_result {
        auto res =
            _client.Post(query(data_path_routes::k_put, key), to_string_body(command), _media_type);
        return interpret(res);
    }

    auto del(const std::string& key, const std::vector<std::byte>& command) -> kv_result {
        auto res = _client.Post(query(data_path_routes::k_delete, key), to_string_body(command),
                                _media_type);
        return interpret(res);
    }

    /// @brief Read `key` at the requested consistency.
    ///
    /// `command` is used only by `wire_read_kind::log`, which submits it as a
    /// proposal; the other two kinds ignore it and the host does not read the
    /// body for them.
    auto get(const std::string& key, wire_read_kind kind, const std::vector<std::byte>& command)
        -> kv_result {
        auto path = query(data_path_routes::k_get, key);
        path += "&read=";
        path += to_string(kind);
        auto res = _client.Post(path, to_string_body(command), _media_type);
        return interpret(res);
    }

private:
    [[nodiscard]] static auto query(std::string_view route, const std::string& key) -> std::string {
        std::string path(route);
        path += "?key=";
        path += percent_encode(key);
        return path;
    }

    [[nodiscard]] static auto header_u64(const httplib::Response& res, std::string_view name)
        -> std::optional<std::uint64_t> {
        const std::string key{name};
        if (!res.has_header(key.c_str())) {
            return std::nullopt;
        }
        try {
            return static_cast<std::uint64_t>(std::stoull(res.get_header_value(key.c_str())));
        } catch (const std::exception&) {
            return std::nullopt;
        }
    }

    [[nodiscard]] static auto interpret(const httplib::Result& res) -> kv_result {
        kv_result out;
        if (!res) {
            // A transport failure is a timeout as far as the tally is
            // concerned: cpp-httplib reports a read deadline and a reset
            // connection through the same channel, and inventing a
            // distinction the library does not make would be a guess with a
            // number attached.
            out._outcome = kv_outcome::timeout;
            return out;
        }
        out._leader = header_u64(*res, k_leader_header);
        out._group = header_u64(*res, k_group_header);
        switch (res->status) {
            case data_path_status::k_ok:
                out._outcome = kv_outcome::ok;
                out._value = to_bytes(res->body);
                break;
            case data_path_status::k_no_such_key:
                out._outcome = kv_outcome::no_such_key;
                break;
            case data_path_status::k_misdirected:
                out._outcome = kv_outcome::not_leader;
                break;
            default:
                out._outcome = kv_outcome::other;
                break;
        }
        return out;
    }

    httplib::Client _client;
    std::string _media_type;
};

}  // namespace kythira::bench
