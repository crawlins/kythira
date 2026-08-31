// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file switchable_persistence.hpp
/// @brief A store that is memory- or file-backed, decided at run time.
///
/// `multi_raft`'s `Types` bundle names **one** persistence engine type, and
/// `--persistence` is a swept axis that must not require a rebuild
/// (`.kiro/specs/multi-raft-host-binary/` Requirement 1.2). This holds one of
/// the two and forwards.
///
/// **Why this is not `benchmark_persistence_engine`.** That type — in
/// `tests/multi_raft_transport_harness.hpp` — is the same shape plus the
/// durability counters, and `.kiro/specs/multi-raft-performance/` Requirement
/// 8.2 keeps counters out of the measured process. A host that counted its own
/// fsyncs would be doing work the Tier B rows did not, inside the very window
/// being compared with them. The counters stay on the driver's side of the
/// wire, where the driver can pay for them.
///
/// A handle rather than a value for the same reason the harness's is:
/// `store_factory` returns by value and `multi_raft` moves the result into the
/// group's `node`, and `file_persistence_engine` is move-constructible but not
/// move-assignable and holds a mutex.

#include "config.hpp"

#include <raft/file_persistence.hpp>
#include <raft/persistence.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace kythira::bench {

namespace detail {

using switchable_memory_engine =
    kythira::memory_persistence_engine<std::uint64_t, std::uint64_t, std::uint64_t>;
using switchable_file_engine =
    kythira::file_persistence_engine<std::uint64_t, std::uint64_t, std::uint64_t>;

/// The two engines the handle can hold. Exactly one is engaged.
///
/// At namespace scope rather than nested, so that `switchable_persistence` can
/// define `dispatch` — whose return type is deduced — **before** the methods
/// that call it. A deduced return type must be seen at the point of use, and
/// unlike the harness's equivalent this class is not a template, so its bodies
/// are not deferred to instantiation.
struct switchable_state {
    std::optional<switchable_memory_engine> _memory{};
    std::unique_ptr<switchable_file_engine> _file{};
};

}  // namespace detail

class switchable_persistence {
private:
    using state = detail::switchable_state;

    /// Defined here, above every caller, for the reason `switchable_state`'s
    /// comment gives.
    template<typename F> auto dispatch(F&& f) -> decltype(auto) {
        if (_state->_file) {
            return f(*_state->_file);
        }
        return f(*_state->_memory);
    }

    std::shared_ptr<state> _state;

public:
    using memory_engine_type = detail::switchable_memory_engine;
    using file_engine_type = detail::switchable_file_engine;
    using log_entry_t = memory_engine_type::log_entry_t;
    using snapshot_t = memory_engine_type::snapshot_t;

    /// Memory-backed.
    switchable_persistence() : _state(std::make_shared<state>()) { _state->_memory.emplace(); }

    /// File-backed at `dir`. `barriers_enabled == false` is the `file_buffered`
    /// arm: it writes through to the operating system and takes no barrier, so
    /// it is **not durable**, and `durability()` answers `buffered` rather than
    /// leaving a reader to remember.
    switchable_persistence(std::filesystem::path dir, bool barriers_enabled)
        : _state(std::make_shared<state>()) {
        _state->_file = std::make_unique<file_engine_type>(std::move(dir), barriers_enabled);
    }

    auto save_current_term(std::uint64_t term) -> void {
        dispatch([&](auto& e) { e.save_current_term(term); });
    }
    auto load_current_term() -> std::uint64_t {
        return dispatch([&](auto& e) { return e.load_current_term(); });
    }
    auto save_voted_for(std::uint64_t node) -> void {
        dispatch([&](auto& e) { e.save_voted_for(node); });
    }
    auto load_voted_for() -> std::optional<std::uint64_t> {
        return dispatch([&](auto& e) { return e.load_voted_for(); });
    }

    auto append_log_entry(const log_entry_t& entry) -> void {
        dispatch([&](auto& e) { e.append_log_entry(entry); });
    }
    auto get_log_entry(std::uint64_t index) -> std::optional<log_entry_t> {
        return dispatch([&](auto& e) { return e.get_log_entry(index); });
    }
    auto get_log_entries(std::uint64_t start, std::uint64_t end) -> std::vector<log_entry_t> {
        return dispatch([&](auto& e) { return e.get_log_entries(start, end); });
    }
    auto get_last_log_index() -> std::uint64_t {
        return dispatch([&](auto& e) { return e.get_last_log_index(); });
    }
    auto truncate_log(std::uint64_t index) -> void {
        dispatch([&](auto& e) { e.truncate_log(index); });
    }
    auto save_snapshot(const snapshot_t& snap) -> void {
        dispatch([&](auto& e) { e.save_snapshot(snap); });
    }
    auto load_snapshot() -> std::optional<snapshot_t> {
        return dispatch([&](auto& e) { return e.load_snapshot(); });
    }
    auto delete_log_entries_before(std::uint64_t index) -> void {
        dispatch([&](auto& e) { e.delete_log_entries_before(index); });
    }

    // ── barriered_persistence_engine ─────────────────────────────────────────
    //
    // Forwarded so that `node` takes the barrier at the boundary where it
    // advertises an append (`.kiro/specs/durable-append-barrier/`). Dropping
    // these would silently make every file-backed row of this host
    // non-durable, which is the shape of the defect that spec removed,
    // reintroduced by a wrapper.

    auto append_log_entry_sequenced(const log_entry_t& entry) -> kythira::write_sequence {
        return dispatch([&](auto& e) { return e.append_log_entry_sequenced(entry); });
    }
    auto barrier_through(kythira::write_sequence seq) -> void {
        dispatch([&](auto& e) { e.barrier_through(seq); });
    }
    [[nodiscard]] auto durable_through() const -> kythira::write_sequence {
        if (_state->_file) {
            return _state->_file->durable_through();
        }
        return _state->_memory->durable_through();
    }
    [[nodiscard]] auto durability() const -> kythira::durability_class {
        if (_state->_file) {
            return _state->_file->durability();
        }
        return kythira::durability_class::none;
    }
};

/// @brief One group's store for this host, in whichever mode was configured.
///
/// One directory per group. `group_scoped_persistence` owns its engine and the
/// isolation is in the location, so two groups sharing a directory would share
/// a log file and interleave two independent index spaces into it.
[[nodiscard]] inline auto make_store(const node_options& opt, std::uint64_t group)
    -> switchable_persistence {
    if (opt._persistence == persistence_mode::memory) {
        return switchable_persistence{};
    }
    return switchable_persistence{opt._data_dir / ("group-" + std::to_string(group)),
                                  opt._persistence == persistence_mode::file_barrier};
}

}  // namespace kythira::bench
