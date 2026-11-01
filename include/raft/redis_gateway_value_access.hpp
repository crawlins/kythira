// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file redis_gateway_value_access.hpp
/// @brief How `redis_gateway` reads a value out of whichever state machine its
///        host runs (.kiro/specs/disk-backed-build-cache/ task 3,
///        Requirement 2.7).
///
/// The gateway's RESP handlers need five things from a looked-up entry: its
/// expiry, its length, whether this node can serve it, its bytes, and whether
/// it equals a value being written. `redis_kv_state_machine` holds the bytes
/// in the entry itself. `cache_kv_state_machine` holds a manifest naming
/// chunks in the node's chunk store, which may not all be here yet.
///
/// Each specialization supplies:
///
/// - `context`, taken under the node lock beside the entry (`context_of`), so
///   the bytes can be read after the lock is released. For the cache it is
///   the node-wide `chunk_refcounts`, held by `shared_ptr` so the counts
///   (and, when the owner aliases them, the store) outlive the lock.
/// - `size(entry)`, `servable(entry)`, `read(context, entry)` and
///   `equals(entry, bytes)`.
///
/// `read` returns `nullopt` when the bytes cannot be produced here (a chunk
/// went missing or failed its hash after `lookup`); the gateway answers that
/// as a miss rather than serve bytes it cannot vouch for.

#include <raft/cache_kv_state_machine.hpp>
#include <raft/redis_kv_state_machine.hpp>

#include <cstddef>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace kythira {

/// A value's bytes as the gateway writes them to a socket: either a view into
/// the entry (which the caller's handle keeps alive) or a buffer the state
/// machine assembled. `view()` is computed on each call, so moving or copying
/// this never leaves a span pointing into a moved-from buffer.
struct redis_gateway_value_bytes {
    std::vector<std::byte> _owned;
    std::span<const std::byte> _borrowed;
    bool _is_owned = false;

    [[nodiscard]] auto view() const noexcept -> std::span<const std::byte> {
        return _is_owned ? std::span<const std::byte>(_owned) : _borrowed;
    }
};

template<typename StateMachine> struct redis_gateway_value_access;

template<typename LogIndex> struct redis_gateway_value_access<redis_kv_state_machine<LogIndex>> {
    using state_machine_type = redis_kv_state_machine<LogIndex>;
    using entry_handle = typename state_machine_type::entry_handle;
    using entry_type = redis_kv_value_entry;
    struct context {};

    [[nodiscard]] static auto context_of(const state_machine_type&) -> context { return {}; }
    [[nodiscard]] static auto size(const entry_type& e) noexcept -> std::size_t {
        return e._value.size();
    }
    [[nodiscard]] static auto servable(const entry_type&) noexcept -> bool { return true; }
    [[nodiscard]] static auto read(const context&, const entry_type& e)
        -> std::optional<redis_gateway_value_bytes> {
        return redis_gateway_value_bytes{{}, std::span<const std::byte>(e._value), false};
    }
    [[nodiscard]] static auto equals(const entry_type& e, std::span<const std::byte> value)
        -> bool {
        return e._value.size() == value.size() &&
               (value.empty() || std::memcmp(e._value.data(), value.data(), value.size()) == 0);
    }
};

template<typename LogIndex> struct redis_gateway_value_access<cache_kv_state_machine<LogIndex>> {
    using state_machine_type = cache_kv_state_machine<LogIndex>;
    using entry_handle = typename state_machine_type::entry_handle;
    using entry_type = cache_manifest;
    using context = std::shared_ptr<chunk_refcounts>;

    [[nodiscard]] static auto context_of(const state_machine_type& sm) -> context {
        return sm.refcounts();
    }
    [[nodiscard]] static auto size(const entry_type& m) noexcept -> std::size_t {
        return static_cast<std::size_t>(m._total_len);
    }
    /// The `has_all` bit: every chunk the manifest names is on this node.
    [[nodiscard]] static auto servable(const entry_type& m) noexcept -> bool { return m.has_all(); }
    [[nodiscard]] static auto read(const context& refs, const entry_type& m)
        -> std::optional<redis_gateway_value_bytes> {
        auto bytes = read_cache_value(refs->store(), m);
        if (!bytes) {
            // Hand it back to the fetcher, and stop calling it servable.
            m.set_has_all(false);
            return std::nullopt;
        }
        return redis_gateway_value_bytes{std::move(*bytes), {}, true};
    }
    /// Values are content-addressed, so equal length and equal sha256 is
    /// equality, and the chunks need not be read back.
    [[nodiscard]] static auto equals(const entry_type& m, std::span<const std::byte> value)
        -> bool {
        return m._total_len == value.size() && sha256_of(value) == m._value_sha;
    }
};

}  // namespace kythira
