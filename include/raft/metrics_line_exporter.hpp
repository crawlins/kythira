// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file metrics_line_exporter.hpp
/// @brief Shared, non-blocking, batching line-protocol export engine used by
///        the socket-push metrics backends: `telegraf_metrics`
///        (telegraf_metrics.hpp, InfluxDB line protocol) and
///        `netdata_metrics` (netdata_metrics.hpp, StatsD). The same shape as
///        otlp_exporter.hpp's `otlp_http_batch_exporter`, but the records are
///        pre-rendered protocol lines and the transport is a UDP datagram or
///        TCP stream instead of an HTTP POST.
///
/// `push()` (the only method a concept-facing backend calls) enqueues under a
/// mutex and returns — no socket, DNS, or connect work ever happens on the
/// calling thread. The background thread batches lines into
/// newline-separated payloads bounded by `max_payload_bytes` (so a UDP
/// payload never exceeds one safe datagram) and hands each payload to the
/// sender seam. Send failures drop the affected lines and count them —
/// deliberately no retry: both StatsD and Telegraf's socket listener treat
/// metric datagrams as inherently lossy, and a retrying exporter would just
/// deliver stale samples late.

#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace kythira {

// ── Sender seam ──────────────────────────────────────────────────────────────

/// Injectable transport seam: receives one ready-to-send payload (one or more
/// protocol lines joined by '\n', no trailing newline for UDP; the TCP sender
/// appends one). Returns false on send failure; the exporter counts the
/// payload's lines as dropped and moves on.
using line_sender_fn = std::function<bool(std::string_view payload)>;

namespace metrics_line_detail {

/// Shared lazy socket state for the real senders. Resolution and connection
/// happen on first use — on the exporter's background thread, never the
/// recording path — and are retried from scratch after any failure.
struct socket_state {
    socket_state(std::string host, std::uint16_t port, int socktype)
        : host(std::move(host)), port(port), socktype(socktype) {}
    ~socket_state() { close_fd(); }

    socket_state(const socket_state&) = delete;
    auto operator=(const socket_state&) -> socket_state& = delete;
    socket_state(socket_state&&) = delete;
    auto operator=(socket_state&&) -> socket_state& = delete;

    auto close_fd() -> void {
        if (fd >= 0) {
            ::close(fd);
            fd = -1;
        }
    }

    /// Resolve + connect if not already connected. Returns false on failure.
    [[nodiscard]] auto ensure_connected() -> bool {
        if (fd >= 0) return true;
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = socktype;
        addrinfo* result = nullptr;
        const auto port_str = std::to_string(port);
        if (::getaddrinfo(host.c_str(), port_str.c_str(), &hints, &result) != 0) return false;
        for (auto* ai = result; ai != nullptr; ai = ai->ai_next) {
            int candidate = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
            if (candidate < 0) continue;
            // connect() on a UDP socket just fixes the peer address so plain
            // send() works — no packet is exchanged.
            if (::connect(candidate, ai->ai_addr, ai->ai_addrlen) == 0) {
                fd = candidate;
                break;
            }
            ::close(candidate);
        }
        ::freeaddrinfo(result);
        return fd >= 0;
    }

    [[nodiscard]] auto send_all(std::string_view payload) -> bool {
        std::size_t sent = 0;
        while (sent < payload.size()) {
            auto n = ::send(fd, payload.data() + sent, payload.size() - sent, MSG_NOSIGNAL);
            if (n < 0) return false;
            sent += static_cast<std::size_t>(n);
        }
        return true;
    }

    std::string host;
    std::uint16_t port;
    int socktype;
    int fd = -1;
};

}  // namespace metrics_line_detail

/// Real UDP sender: one datagram per payload, fire-and-forget. The socket is
/// connect()ed once and reset on any send error (which also re-resolves the
/// host — compose service names can change address across container
/// restarts).
[[nodiscard]] inline auto udp_line_sender(std::string host, std::uint16_t port) -> line_sender_fn {
    auto state =
        std::make_shared<metrics_line_detail::socket_state>(std::move(host), port, SOCK_DGRAM);
    return [state](std::string_view payload) -> bool {
        if (!state->ensure_connected()) return false;
        if (!state->send_all(payload)) {
            state->close_fd();
            return false;
        }
        return true;
    };
}

/// Real TCP sender: appends the trailing newline stream protocols expect,
/// reconnects from scratch after any failure.
[[nodiscard]] inline auto tcp_line_sender(std::string host, std::uint16_t port) -> line_sender_fn {
    auto state =
        std::make_shared<metrics_line_detail::socket_state>(std::move(host), port, SOCK_STREAM);
    return [state](std::string_view payload) -> bool {
        if (!state->ensure_connected()) return false;
        std::string framed(payload);
        framed.push_back('\n');
        if (!state->send_all(framed)) {
            state->close_fd();
            return false;
        }
        return true;
    };
}

// ── Configuration ────────────────────────────────────────────────────────────

struct metrics_line_exporter_config {
    /// Max bytes per payload handed to the sender. 1400 keeps a UDP datagram
    /// under the conservative 1500-MTU-minus-headers ceiling both the StatsD
    /// protocol notes and Telegraf's socket_listener docs recommend. A single
    /// line longer than this is sent alone in its own oversized payload
    /// rather than silently dropped — TCP delivers it fine, and for UDP the
    /// OS either fragments or rejects it visibly.
    std::size_t max_payload_bytes = 1400;
    std::chrono::milliseconds flush_interval{1000};
    std::size_t max_batch_lines = 512;
    std::size_t max_queue_size = 4096;
};

// ── The exporter ─────────────────────────────────────────────────────────────

/// Pimpl'd for the same reason as otlp_http_batch_exporter: the wrapper stays
/// trivially movable while the background thread's captured pointer targets
/// the stable heap impl.
class metrics_line_exporter {
public:
    metrics_line_exporter(metrics_line_exporter_config config, line_sender_fn sender)
        : _impl(std::make_unique<impl>(std::move(config), std::move(sender))) {}

    metrics_line_exporter(metrics_line_exporter&&) noexcept = default;
    auto operator=(metrics_line_exporter&&) noexcept -> metrics_line_exporter& = default;
    metrics_line_exporter(const metrics_line_exporter&) = delete;
    auto operator=(const metrics_line_exporter&) -> metrics_line_exporter& = delete;

    ~metrics_line_exporter() = default;  // impl's destructor does the shutdown work.

    /// Never blocks on I/O.
    auto push(std::string line) -> void {
        if (_impl) _impl->push(std::move(line));
    }

    /// Overflow/send-failure visibility for tests and operators.
    [[nodiscard]] auto dropped_line_count() const -> std::uint64_t {
        return _impl ? _impl->dropped_line_count() : 0;
    }

private:
    struct impl {
        impl(metrics_line_exporter_config cfg, line_sender_fn snd)
            : config(std::move(cfg)), sender(std::move(snd)) {
            if (!sender) {
                throw std::invalid_argument("metrics_line_exporter: sender must not be empty");
            }
            worker = std::thread([this] { background_loop(); });
        }

        ~impl() {
            stop_flag.store(true, std::memory_order_relaxed);
            cv.notify_all();
            if (worker.joinable()) worker.join();
        }

        impl(const impl&) = delete;
        auto operator=(const impl&) -> impl& = delete;
        impl(impl&&) = delete;
        auto operator=(impl&&) -> impl& = delete;

        auto push(std::string line) -> void {
            std::lock_guard<std::mutex> lock(mu);
            if (queue.size() >= config.max_queue_size) {
                queue.pop_front();
                dropped.fetch_add(1, std::memory_order_relaxed);
            }
            queue.push_back(std::move(line));
            if (queue.size() >= config.max_batch_lines) cv.notify_one();
        }

        [[nodiscard]] auto dropped_line_count() const -> std::uint64_t {
            return dropped.load(std::memory_order_relaxed);
        }

        auto drain_locked() -> std::vector<std::string> {
            std::vector<std::string> batch;
            auto n = std::min(queue.size(), config.max_batch_lines);
            batch.reserve(n);
            for (std::size_t i = 0; i < n; ++i) {
                batch.push_back(std::move(queue.front()));
                queue.pop_front();
            }
            return batch;
        }

        auto wait_and_drain() -> std::vector<std::string> {
            std::unique_lock<std::mutex> lock(mu);
            cv.wait_for(lock, config.flush_interval, [this] {
                return stop_flag.load(std::memory_order_relaxed) ||
                       queue.size() >= config.max_batch_lines;
            });
            return drain_locked();
        }

        auto drain_now() -> std::vector<std::string> {
            std::lock_guard<std::mutex> lock(mu);
            return drain_locked();
        }

        /// Pack lines into payloads of at most max_payload_bytes (joined by
        /// '\n') and send each. A failed send drops that payload's lines.
        auto send_batch(const std::vector<std::string>& batch) -> void {
            std::string payload;
            std::size_t payload_lines = 0;
            auto flush_payload = [&] {
                if (payload.empty()) return;
                if (!sender(payload)) {
                    dropped.fetch_add(payload_lines, std::memory_order_relaxed);
                }
                payload.clear();
                payload_lines = 0;
            };
            for (const auto& line : batch) {
                const auto projected = payload.size() + (payload.empty() ? 0 : 1) + line.size();
                if (!payload.empty() && projected > config.max_payload_bytes) flush_payload();
                if (!payload.empty()) payload.push_back('\n');
                payload += line;
                ++payload_lines;
            }
            flush_payload();
        }

        auto background_loop() -> void {
            while (!stop_flag.load(std::memory_order_relaxed)) {
                send_batch(wait_and_drain());
            }
            // One final best-effort flush — a single drain, not a loop, so a
            // still-growing backlog cannot delay process exit.
            send_batch(drain_now());
        }

        metrics_line_exporter_config config;
        line_sender_fn sender;

        std::mutex mu;
        std::condition_variable cv;
        std::deque<std::string> queue;
        std::atomic<bool> stop_flag{false};
        std::atomic<std::uint64_t> dropped{0};
        // Assigned in the constructor body (not the init list) so every other
        // member is fully constructed before background_loop() can observe
        // them.
        std::thread worker;
    };

    std::unique_ptr<impl> _impl;
};

}  // namespace kythira
