// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file control_server.hpp
/// @brief Health, readiness, routing and the host's own description — on a
///        **different port** from the data path.
///
/// `.kiro/specs/multi-raft-host-binary/` Requirement 2.4 asks that the data
/// path be kept separate from any control surface, so that a measurement
/// cannot accidentally include control traffic. A second listening socket is
/// the only separation that survives somebody counting packets rather than
/// requests, and it costs one thread.
///
/// The driver uses this **before** a measured window and never inside one: it
/// waits on `/ready`, reads the leader map once to seed its routing cache, and
/// records `/describe` on the row so a reader knows what the host was
/// configured as without trusting the command line that launched it.

#include "config.hpp"

#include <httplib.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace kythira::bench {

template<typename Host> class control_server {
public:
    control_server(Host& host, const node_options& options, std::string transport_name,
                   std::string bind_address, std::uint16_t port)
        : _host(host),
          _options(options),
          _transport(std::move(transport_name)),
          _bind(std::move(bind_address)),
          _port(port),
          _started(std::chrono::steady_clock::now()) {}

    ~control_server() { stop(); }
    control_server(const control_server&) = delete;
    auto operator=(const control_server&) -> control_server& = delete;

    auto start() -> void {
        // As on the data port. The network probe below measures round trips,
        // and a 40 ms Nagle stall would be reported as the network's.
        _server.set_tcp_nodelay(true);
        _server.Get("/health", [this](const httplib::Request&, httplib::Response& res) {
            res.set_content(
                _host.is_running() ? R"({"status":"running"})" : R"({"status":"stopped"})",
                "application/json");
            res.status = _host.is_running() ? 200 : 503;
        });

        // **Readiness is "every group has a leader", not "the process is up".**
        // A driver that starts offering load before the cluster has elected
        // measures the election, and this project has already learned that a
        // window containing an election is not a window measuring a workload.
        _server.Get("/ready", [this](const httplib::Request&, httplib::Response& res) {
            const auto missing = groups_without_leader();
            res.status = missing == 0 ? 200 : 503;
            std::ostringstream out;
            out << R"({"ready":)" << (missing == 0 ? "true" : "false")
                << R"(,"groups_without_leader":)" << missing << R"(,"uptime_ms":)"
                << std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - _started)
                       .count()
                << '}';
            res.set_content(out.str(), "application/json");
        });

        // Which groups this host leads, so a driver can build a routing cache
        // out of band rather than by paying a not-leader round trip per key
        // inside the window. The not-leader answer still exists and is still
        // counted — this only keeps the *first* request of a window from being
        // one.
        _server.Get("/leaders", [this](const httplib::Request&, httplib::Response& res) {
            std::ostringstream out;
            out << '{' << R"("node_id":)" << _options._node_id << R"(,"leads":[)";
            bool first = true;
            for (auto group : _host.group_ids()) {
                auto* node = _host.group_node(group);
                if (node == nullptr || !node->is_leader()) {
                    continue;
                }
                if (!first) {
                    out << ',';
                }
                first = false;
                out << group;
            }
            out << "]}";
            res.set_content(out.str(), "application/json");
        });

        // What this host actually is, read off the running configuration
        // rather than off the command line that started it. A row records this
        // so that "the host was configured as X" is evidence rather than an
        // assumption about how somebody launched it.
        _server.Get("/describe", [this](const httplib::Request&, httplib::Response& res) {
            std::ostringstream out;
            out << '{' << R"("node_id":)" << _options._node_id << R"(,"transport":")" << _transport
                << R"(","groups":)" << _options._groups << R"(,"key_count":)" << _options._key_count
                << R"(,"tick_interval_ms":)" << _options._tick_interval.count()
                << R"(,"persistence":")" << persistence_name() << R"(","durable":)"
                << (durable() ? "true" : "false") << R"(,"shard_split":"pre-split")"
                << R"(,"voters":)" << _options._voters.size() << '}';
            res.set_content(out.str(), "application/json");
        });

        // ── Requirement 5.4: the network, measured BEFORE the window ─────
        //
        // `.kiro/specs/multi-raft-performance/` task 11 established that the
        // per-stream inter-round interval tracks the RPC round trip, which
        // makes the network between the nodes the axis most likely to explain
        // a Tier E result. A cluster number without it is not reproducible, so
        // the driver asks each host to measure its own peers before offering
        // any load, and records the answer on the row.
        //
        // It is on the CONTROL port, and it is a host-to-host measurement
        // rather than a driver-to-host one, because what the requirement is
        // about is the network Raft runs over — not the one the client does.

        // Echo: the far end of somebody else's bandwidth probe. Returns the
        // body it was given, so the measured quantity is a real round trip of
        // real bytes rather than a latency multiplied by an assumption.
        _server.Post("/echo", [](const httplib::Request& req, httplib::Response& res) {
            res.set_content(req.body, "application/octet-stream");
        });

        _server.Get("/probe", [this](const httplib::Request&, httplib::Response& res) {
            res.set_content(probe_peers(), "application/json");
        });

        // The sum of every group's current term, as a cheap "did anything
        // re-elect" probe. The in-process harness has `kv_cluster::term_sum()`
        // for exactly this and differences two samples around a window; an
        // out-of-process driver cannot reach into the hosts, so the hosts
        // offer it.
        //
        // **An election inside a measured window is reported, never asserted.**
        // A window that contained one is not measuring a workload, and a row
        // that cannot say whether it contained one cannot be read at all.
        _server.Get("/terms", [this](const httplib::Request&, httplib::Response& res) {
            std::uint64_t total = 0;
            std::size_t groups = 0;
            for (auto group : _host.group_ids()) {
                if (auto* node = _host.group_node(group); node != nullptr) {
                    total += node->get_current_term();
                    ++groups;
                }
            }
            std::ostringstream out;
            out << R"({"node_id":)" << _options._node_id << R"(,"groups":)" << groups
                << R"(,"term_sum":)" << total << '}';
            res.set_content(out.str(), "application/json");
        });

        _thread = std::thread([this] { _server.listen(_bind.c_str(), _port); });
        _server.wait_until_ready();
    }

    auto stop() -> void {
        if (_stopped.exchange(true)) {
            return;
        }
        _server.stop();
        if (_thread.joinable()) {
            _thread.join();
        }
    }

private:
    /// @brief Round-trip time and bandwidth to every peer, measured now.
    ///
    /// Round trip is the median of `k_rtt_samples` empty requests, not the
    /// mean: one scheduling hiccup on a shared machine moves a mean and does
    /// not move a median, and a network figure that a single outlier can move
    /// is not one anybody should size a cluster from.
    ///
    /// Bandwidth is `k_probe_bytes` echoed back, so the number covers both
    /// directions of a real transfer. It is deliberately not derived from the
    /// round trip and an assumed MTU.
    ///
    /// **This is a control-port measurement of the peers' control ports.** It
    /// therefore describes the same network the Raft transport uses and not
    /// the Raft transport itself; the row says so rather than implying the
    /// figure is a Raft RPC's.
    [[nodiscard]] auto probe_peers() -> std::string {
        static constexpr int k_rtt_samples = 11;
        static constexpr std::size_t k_probe_bytes = 1U << 20;

        std::ostringstream out;
        out << R"({"node_id":)" << _options._node_id << R"(,"peers":[)";
        bool first = true;
        for (const auto& [peer_id, url] : _options._peers) {
            if (peer_id == _options._node_id) {
                continue;
            }
            if (!first) {
                out << ',';
            }
            first = false;

            const auto endpoint = control_endpoint_of(peer_id);
            if (!endpoint.has_value()) {
                // No `--peer-control` for this peer. Reported as unmeasured
                // rather than skipped: a peer missing from the list would look
                // like a peer that does not exist, and Requirement 18.4 of
                // `.kiro/specs/multi-raft-performance/` is that an absent
                // figure is null and never guessed.
                out << '{' << R"("peer":)" << peer_id
                    << R"(,"rtt_median_us":null,"bandwidth_mib_per_sec":null,)"
                    << R"("reason":"no --peer-control address"})";
                continue;
            }

            httplib::Client client(endpoint->first, endpoint->second);
            client.set_connection_timeout(std::chrono::seconds{5});
            client.set_read_timeout(std::chrono::seconds{30});
            client.set_keep_alive(true);
            client.set_tcp_nodelay(true);

            std::vector<double> rtt_us;
            rtt_us.reserve(k_rtt_samples);
            for (int i = 0; i < k_rtt_samples; ++i) {
                const auto started = std::chrono::steady_clock::now();
                auto res = client.Get("/health");
                if (!res) {
                    continue;
                }
                rtt_us.push_back(std::chrono::duration<double, std::micro>(
                                     std::chrono::steady_clock::now() - started)
                                     .count());
            }
            std::sort(rtt_us.begin(), rtt_us.end());

            const std::string payload(k_probe_bytes, 'x');
            const auto bw_started = std::chrono::steady_clock::now();
            auto echoed = client.Post("/echo", payload, "application/octet-stream");
            const auto bw_elapsed =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - bw_started)
                    .count();

            out << '{' << R"("peer":)" << peer_id << R"(,"rtt_samples":)" << rtt_us.size()
                << R"(,"rtt_median_us":)";
            if (rtt_us.empty()) {
                // Not stated, never guessed. An unreachable peer's round trip
                // is absent, and a zero here would read as "instantaneous".
                out << "null";
            } else {
                out << rtt_us[rtt_us.size() / 2];
            }
            out << R"(,"probe_bytes":)" << k_probe_bytes << R"(,"bandwidth_mib_per_sec":)";
            if (!echoed || echoed->body.size() != payload.size() || bw_elapsed <= 0.0) {
                out << "null";
            } else {
                // Both directions of a 1 MiB transfer in `bw_elapsed`.
                out << (2.0 * static_cast<double>(k_probe_bytes) / (1024.0 * 1024.0)) / bw_elapsed;
            }
            out << '}';
        }
        out << "]}";
        return out.str();
    }

    /// A peer's control address, from `--peer-control` and from nowhere else.
    ///
    /// **Deliberately not derived from the peer's Raft URL.** A convention
    /// ("control is Raft plus two") would silently probe whatever happens to be
    /// listening on that port and report the answer as this cluster's network.
    [[nodiscard]] auto control_endpoint_of(std::uint64_t peer_id) const
        -> std::optional<std::pair<std::string, std::uint16_t>> {
        const auto it = _options._peer_control.find(peer_id);
        if (it == _options._peer_control.end()) {
            return std::nullopt;
        }
        const auto colon = it->second.rfind(':');
        if (colon == std::string::npos) {
            return std::nullopt;
        }
        try {
            return std::pair{it->second.substr(0, colon),
                             static_cast<std::uint16_t>(std::stoi(it->second.substr(colon + 1)))};
        } catch (const std::exception&) {
            return std::nullopt;
        }
    }

    [[nodiscard]] auto groups_without_leader() -> std::size_t {
        std::size_t missing = 0;
        for (auto group : _host.group_ids()) {
            auto* node = _host.group_node(group);
            // `known_leader()` on a follower names whoever last sent it an
            // AppendEntries, which is the only evidence a follower has that an
            // election finished.
            if (node == nullptr || (!node->is_leader() && !node->known_leader().has_value())) {
                ++missing;
            }
        }
        return missing;
    }

    [[nodiscard]] auto persistence_name() const -> std::string_view {
        switch (_options._persistence) {
            case persistence_mode::memory:
                return "memory";
            case persistence_mode::file_buffered:
                return "file/buffered (NOT DURABLE)";
            case persistence_mode::file_barrier:
                return "file/barrier";
        }
        return "unknown";
    }

    /// Requirement 5.4 of `.kiro/specs/durable-append-barrier/`, answered on
    /// the wire: a configuration that cannot take a barrier is refused the word
    /// durable here rather than being described optimistically and corrected
    /// in prose later.
    [[nodiscard]] auto durable() const -> bool {
        return _options._persistence == persistence_mode::file_barrier;
    }

    Host& _host;
    const node_options& _options;
    std::string _transport;
    std::string _bind;
    std::uint16_t _port;
    std::chrono::steady_clock::time_point _started;
    httplib::Server _server;
    std::thread _thread;
    std::atomic<bool> _stopped{false};
};

}  // namespace kythira::bench
