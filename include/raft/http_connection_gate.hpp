// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

// The live-connection counter behind `max_concurrent_connections` on the HTTP
// servers (.kiro/specs/http-server-request-limits/). One gate per server
// object, shared by every listener that server owns, so a "*" bind's 0.0.0.0
// and :: listeners draw on one limit rather than one each.
//
// Each accepted connection asks for a slot before anything is read from it or
// a TLS handshake starts. A connection that gets none is closed at once. The
// slot is released when the connection is destroyed.
//
// No third-party includes, so the unit test builds on every leg whichever HTTP
// transports are enabled.

#include <atomic>
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace kythira::http_detail {

class connection_gate : public std::enable_shared_from_this<connection_gate> {
public:
    /// @throws std::invalid_argument when `limit` is 0: a server that can
    ///     never hold a connection can never serve a request, so refusing to
    ///     build one is kinder than a server that silently refuses everything.
    explicit connection_gate(std::size_t limit) : _limit(limit) {
        if (limit == 0) {
            throw std::invalid_argument("max_concurrent_connections must be at least 1");
        }
    }

    /// RAII hold on one connection's worth of the limit.
    ///
    /// Holds a `shared_ptr` to its gate rather than a raw pointer: a
    /// connection can outlive its server's `stop()` (a Beast session finishing
    /// its last write, a Proxygen session torn down asynchronously), and a late
    /// release through a raw pointer would write to freed memory. The gate
    /// lives until its last slot goes.
    class slot {
    public:
        slot() = default;
        slot(const slot&) = delete;
        auto operator=(const slot&) -> slot& = delete;
        slot(slot&& other) noexcept : _gate(std::move(other._gate)) {}
        auto operator=(slot&& other) noexcept -> slot& {
            if (this != &other) {
                release();
                _gate = std::move(other._gate);
            }
            return *this;
        }
        ~slot() { release(); }

        /// True while this slot holds a share of the limit.
        explicit operator bool() const noexcept { return _gate != nullptr; }

        /// Gives the share back early. Idempotent.
        auto release() noexcept -> void {
            if (_gate) {
                _gate->_live.fetch_sub(1, std::memory_order_acq_rel);
                _gate.reset();
            }
        }

    private:
        friend class connection_gate;
        explicit slot(std::shared_ptr<connection_gate> gate) : _gate(std::move(gate)) {}
        std::shared_ptr<connection_gate> _gate;
    };

    /// A held slot, or an empty one when `limit()` connections are already
    /// live. Lock-free: Beast and Proxygen call it from event-loop threads.
    ///
    /// The gate must be owned by a `shared_ptr` (`make_shared`), since every
    /// slot keeps it alive.
    [[nodiscard]] auto try_acquire() -> slot {
        auto live = _live.load(std::memory_order_acquire);
        do {
            if (live >= _limit) {
                _refused.fetch_add(1, std::memory_order_relaxed);
                return slot{};
            }
        } while (!_live.compare_exchange_weak(live, live + 1, std::memory_order_acq_rel,
                                              std::memory_order_acquire));
        return slot{shared_from_this()};
    }

    [[nodiscard]] auto live() const noexcept -> std::size_t {
        return _live.load(std::memory_order_acquire);
    }
    [[nodiscard]] auto limit() const noexcept -> std::size_t { return _limit; }
    /// Connections refused since the gate was built.
    [[nodiscard]] auto refused() const noexcept -> std::size_t {
        return _refused.load(std::memory_order_relaxed);
    }

private:
    std::size_t _limit;
    std::atomic<std::size_t> _live{0};
    std::atomic<std::size_t> _refused{0};
};

}  // namespace kythira::http_detail
