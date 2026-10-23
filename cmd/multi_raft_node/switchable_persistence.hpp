// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file switchable_persistence.hpp
/// @brief A store that is memory-, file- or S3-backed, decided at run time.
///
/// `multi_raft`'s `Types` bundle names **one** persistence engine type, and
/// `--persistence` is a swept axis that must not require a rebuild
/// (`.kiro/specs/multi-raft-host-binary/` Requirement 1.2). This holds one of
/// the three and forwards.
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
#include <raft/host_node_id.hpp>
#include <raft/persistence.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace kythira::bench {

/// @brief One group's S3 store, behind virtual calls
///        (`.kiro/specs/multi-raft-node-s3-persistence/`).
///
/// **Type-erased, and defined out of line in `s3_store.cpp`, on purpose.**
/// The S3 arm is two instantiations of `object_store_persistence_engine`
/// over `aws_s3_client`, one per fencing mode, and the SDK's S3 headers
/// behind them. Held by value here they would be parsed and instantiated in
/// every `run_*.cpp`, which already sit at about 5.4 GiB of compiler memory
/// under the `heavy_tu` pool (see `CMakeLists.txt`). Behind this interface
/// they are compiled once, in a light translation unit, and the `run_*.cpp`
/// see no SDK header at all. A virtual call per persistence operation is
/// noise beside the S3 round trip it fronts.
class s3_group_store {
public:
    using log_entry_t = kythira::log_entry<std::uint64_t, std::uint64_t>;
    using snapshot_t = kythira::snapshot<kythira::host_node_id, std::uint64_t, std::uint64_t>;

    s3_group_store() = default;
    s3_group_store(const s3_group_store&) = delete;
    auto operator=(const s3_group_store&) -> s3_group_store& = delete;
    s3_group_store(s3_group_store&&) = delete;
    auto operator=(s3_group_store&&) -> s3_group_store& = delete;
    virtual ~s3_group_store() = default;

    virtual auto save_current_term(std::uint64_t term) -> void = 0;
    virtual auto load_current_term() -> std::uint64_t = 0;
    virtual auto save_voted_for(const kythira::host_node_id& node) -> void = 0;
    virtual auto load_voted_for() -> std::optional<kythira::host_node_id> = 0;
    virtual auto append_log_entry(const log_entry_t& entry) -> void = 0;
    virtual auto get_log_entry(std::uint64_t index) -> std::optional<log_entry_t> = 0;
    virtual auto get_log_entries(std::uint64_t start, std::uint64_t end)
        -> std::vector<log_entry_t> = 0;
    virtual auto get_last_log_index() -> std::uint64_t = 0;
    virtual auto truncate_log(std::uint64_t index) -> void = 0;
    virtual auto save_snapshot(const snapshot_t& snap) -> void = 0;
    virtual auto load_snapshot() -> std::optional<snapshot_t> = 0;
    virtual auto delete_log_entries_before(std::uint64_t index) -> void = 0;
    virtual auto append_log_entry_sequenced(const log_entry_t& entry)
        -> kythira::write_sequence = 0;
    virtual auto barrier_through(kythira::write_sequence seq) -> void = 0;
    [[nodiscard]] virtual auto durable_through() const -> kythira::write_sequence = 0;
    /// The engine's own answer: `synchronous` with write-behind off,
    /// `barrier` with it on. Never a constant: `node` releases its lock for
    /// the flush only on `barrier`, so answering anything else here would
    /// either turn group commit off with no error or treat staged entries as
    /// durable.
    [[nodiscard]] virtual auto durability() const -> kythira::durability_class = 0;
    /// The engine's `flushes_issued()`, for the status surface (Requirement
    /// 11.7). Reading it adds no request.
    [[nodiscard]] virtual auto flushes_issued() const -> std::uint64_t = 0;
    /// Whether the engine has latched after a refused precondition.
    [[nodiscard]] virtual auto is_fenced() const -> bool = 0;
};

/// @brief Builds group `group`'s S3 store under the host prefix. Defined in
///        `s3_store.cpp`, which is compiled only with the S3 arm.
///
/// @throws std::runtime_error naming the group, the key and that another
///         process holds the prefix, when the engine's owner claim is refused
///         (Requirement 4.4); otherwise whatever the engine or SDK throws.
[[nodiscard]] auto make_s3_group_store(const node_options& opt, std::uint64_t group)
    -> std::unique_ptr<s3_group_store>;

/// @brief Every live S3 group store of this process, for the status surface.
///
/// Process-wide because the process hosts exactly one `multi_raft`, and the
/// stores are built inside `store_factory` and moved into the group's node,
/// where nothing else can reach them. Weak references, so a group that is
/// merged away or rebuilt drops out on its own.
class s3_store_registry {
public:
    struct summary {
        std::uint64_t flushes{0};
        std::vector<std::uint64_t> fenced_groups;
    };

    static auto instance() -> s3_store_registry& {
        static s3_store_registry registry;
        return registry;
    }

    auto add(std::uint64_t group, const std::shared_ptr<s3_group_store>& store) -> void {
        std::lock_guard lock(_mu);
        std::erase_if(_stores, [](const auto& entry) { return entry.second.expired(); });
        _stores.emplace_back(group, store);
    }

    [[nodiscard]] auto summarise() const -> summary {
        summary out;
        std::lock_guard lock(_mu);
        for (const auto& [group, weak] : _stores) {
            if (auto store = weak.lock()) {
                out.flushes += store->flushes_issued();
                if (store->is_fenced()) {
                    out.fenced_groups.push_back(group);
                }
            }
        }
        return out;
    }

private:
    mutable std::mutex _mu;
    std::vector<std::pair<std::uint64_t, std::weak_ptr<s3_group_store>>> _stores;
};

namespace detail {

// Votes and snapshot configurations name nodes, so both engines store the
// host's node id kind (`KYTHIRA_NODE_ID_KIND`).
using switchable_memory_engine =
    kythira::memory_persistence_engine<kythira::host_node_id, std::uint64_t, std::uint64_t>;
using switchable_file_engine =
    kythira::file_persistence_engine<kythira::host_node_id, std::uint64_t, std::uint64_t>;

/// The three stores the handle can hold. Exactly one is engaged.
///
/// At namespace scope rather than nested, so that `switchable_persistence` can
/// define `dispatch` — whose return type is deduced — **before** the methods
/// that call it. A deduced return type must be seen at the point of use, and
/// unlike the harness's equivalent this class is not a template, so its bodies
/// are not deferred to instantiation.
struct switchable_state {
    std::optional<switchable_memory_engine> _memory{};
    std::unique_ptr<switchable_file_engine> _file{};
    /// Shared with `s3_store_registry`, which holds it weakly.
    std::shared_ptr<s3_group_store> _s3{};
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
        if (_state->_s3) {
            return f(*_state->_s3);
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

    /// S3-backed: takes ownership of a store `make_s3_group_store` built.
    explicit switchable_persistence(std::shared_ptr<s3_group_store> store)
        : _state(std::make_shared<state>()) {
        _state->_s3 = std::move(store);
    }

    auto save_current_term(std::uint64_t term) -> void {
        dispatch([&](auto& e) { e.save_current_term(term); });
    }
    auto load_current_term() -> std::uint64_t {
        return dispatch([&](auto& e) { return e.load_current_term(); });
    }
    auto save_voted_for(const kythira::host_node_id& node) -> void {
        dispatch([&](auto& e) { e.save_voted_for(node); });
    }
    auto load_voted_for() -> std::optional<kythira::host_node_id> {
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
        if (_state->_s3) {
            return _state->_s3->durable_through();
        }
        return _state->_memory->durable_through();
    }
    [[nodiscard]] auto durability() const -> kythira::durability_class {
        if (_state->_file) {
            return _state->_file->durability();
        }
        if (_state->_s3) {
            return _state->_s3->durability();
        }
        return kythira::durability_class::none;
    }
};

/// @brief One group's store for this host, in whichever mode was configured.
///
/// One directory, or one key prefix, per group. `group_scoped_persistence`
/// owns its engine and the isolation is in the location, so two groups sharing
/// a directory would share a log file and interleave two independent index
/// spaces into it.
[[nodiscard]] inline auto make_store(const node_options& opt, std::uint64_t group)
    -> switchable_persistence {
    if (opt._persistence == persistence_mode::memory) {
        return switchable_persistence{};
    }
    if (opt._persistence == persistence_mode::s3) {
#ifdef KYTHIRA_NODE_HAS_S3_PERSISTENCE
        std::shared_ptr<s3_group_store> store = make_s3_group_store(opt, group);
        s3_store_registry::instance().add(group, store);
        return switchable_persistence{std::move(store)};
#else
        // `main` refuses the mode before any group is built; this is the
        // backstop that keeps a build without the arm linking.
        throw std::runtime_error(
            "multi_raft_node: --persistence s3 is not compiled into this build");
#endif
    }
    return switchable_persistence{opt._data_dir / ("group-" + std::to_string(group)),
                                  opt._persistence == persistence_mode::file_barrier};
}

}  // namespace kythira::bench
