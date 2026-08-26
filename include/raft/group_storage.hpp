// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file group_storage.hpp
/// @brief Per-group durable state: namespacing, the batched-write extension,
///        and the tombstone set that keeps a merged-away group dead.
///
/// See `.kiro/specs/multi-raft/` design §3.
///
/// **Namespacing is mostly a construction parameter, not a wrapper.** All three
/// shipped engines already take the thing that scopes them:
/// `file_persistence_engine` takes a `data_dir` (file_persistence.hpp:43),
/// `object_store_persistence_engine` takes an object-key `prefix`, and
/// `memory_persistence_engine` is one instance per group and therefore isolated
/// already. The helpers below build the scoped location; the wrapper exists for
/// third-party engines and, more usefully, to forward the optional batching
/// extension so a group-scoped store still advertises it.
///
/// **Batching is not only a performance feature.** Without it, N ready groups
/// cost N durability barriers per tick, which is the dominant cost at scale —
/// TiKV's ready loop uses one RocksDB `WriteBatch` for every ready group at
/// once for exactly this reason. But the split apply path also depends on it
/// for atomicity (design §5.4 step E): the children's initial state and the
/// parent's advanced apply index must land together, or a crash between them
/// loses a child silently.

#include <raft/exceptions.hpp>
#include <raft/persistence.hpp>
#include <raft/shard_types.hpp>
#include <raft/types.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace kythira {

// ─────────────────────────────────────────────────────────────────────────────
// Naming
// ─────────────────────────────────────────────────────────────────────────────

/// @brief A filesystem- and object-key-safe rendering of a group id.
///
/// Only `std::uint64_t`-like and `std::string` group ids arise in practice, and
/// the string case is sanitised rather than rejected: a group id is chosen by
/// the application, and a slash in it must not be allowed to escape the
/// group's own subtree.
template<raft_group_id GroupId>
[[nodiscard]] auto group_path_component(const GroupId& group) -> std::string {
    std::string raw;
    if constexpr (std::same_as<GroupId, std::string>) {
        raw = group;
    } else {
        raw = std::to_string(group);
    }
    std::string out;
    out.reserve(raw.size());
    for (char c : raw) {
        const bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                          (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
        out.push_back(safe ? c : '_');
    }
    return out.empty() ? std::string{"_"} : out;
}

/// @brief Where one group's durable state lives under a shared data directory.
///
/// The `groups/` level exists so a host can keep its own state (the shard map,
/// the tombstone set) beside the groups without colliding with a group whose
/// id happens to spell one of those file names.
template<raft_group_id GroupId>
[[nodiscard]] auto group_data_dir(const std::filesystem::path& data_dir, const GroupId& group)
    -> std::filesystem::path {
    return data_dir / "groups" / group_path_component(group);
}

/// @brief The object-key prefix for one group under a shared store prefix.
template<raft_group_id GroupId>
[[nodiscard]] auto group_key_prefix(const std::string& prefix, const GroupId& group)
    -> std::string {
    if (prefix.empty()) {
        return "groups/" + group_path_component(group);
    }
    return prefix + "/groups/" + group_path_component(group);
}

/// @brief Delete one group's durable subtree. Returns `true` if anything was removed.
///
/// Used by merge apply, which destroys the local source replica after absorbing
/// its state — and by nothing else, because a group's data outliving its
/// replica is how a stale replica gets resurrected.
template<raft_group_id GroupId>
auto destroy_group_data(const std::filesystem::path& data_dir, const GroupId& group) -> bool {
    return std::filesystem::remove_all(group_data_dir(data_dir, group)) > 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// Batched writes
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Optional extension: a store that can group many writes behind one
/// durability barrier.
///
/// Detected with `if constexpr`, never required. An engine without it still
/// works — the tick simply pays one barrier per ready group, and split apply
/// falls back to the ordering rule in design §5.4 step E (children first, then
/// the parent's apply index) with the idempotence check making a replayed
/// split a no-op.
template<typename P>
concept batched_persistence_engine = requires(P& p) {
    /// Begin buffering writes. Nested batches are not supported.
    { p.begin_batch() } -> std::same_as<void>;
    /// Flush every buffered write and issue exactly one durability barrier.
    { p.commit_batch() } -> std::same_as<void>;
    /// Discard every buffered write, leaving the store as it was at `begin_batch()`.
    { p.abort_batch() } -> std::same_as<void>;
};

// ─────────────────────────────────────────────────────────────────────────────
// The group-scoped engine
// ─────────────────────────────────────────────────────────────────────────────

/// @brief A `persistence_engine` bound to one group.
///
/// Owns its engine — the engine was constructed at a group-scoped location by
/// `group_data_dir()` / `group_key_prefix()`, so the isolation is in the
/// location rather than in this wrapper's method bodies. What the wrapper adds
/// is uniformity (every group's store is the same type regardless of engine)
/// and, more usefully, conditional forwarding of the batching extension, so
/// that `batched_persistence_engine<group_scoped_persistence<E, G>>` holds
/// exactly when it holds of `E`.
///
/// @tparam Engine  The wrapped engine. Must satisfy `persistence_engine`.
/// @tparam GroupId Must satisfy `raft_group_id`.
template<typename Engine, raft_group_id GroupId = std::uint64_t> class group_scoped_persistence {
public:
    using engine_type = Engine;
    using group_id_type = GroupId;
    using log_entry_t = typename Engine::log_entry_t;
    using snapshot_t = typename Engine::snapshot_t;

    group_scoped_persistence(GroupId group, Engine engine)
        : _group(std::move(group)), _engine(std::move(engine)) {}

    [[nodiscard]] auto group_id() const -> const GroupId& { return _group; }
    [[nodiscard]] auto engine() -> Engine& { return _engine; }
    [[nodiscard]] auto engine() const -> const Engine& { return _engine; }

    // ── persistence_engine ───────────────────────────────────────────────────

    template<typename TermId> auto save_current_term(TermId term) -> void {
        _engine.save_current_term(term);
    }
    auto load_current_term() { return _engine.load_current_term(); }

    template<typename NodeId> auto save_voted_for(NodeId node) -> void {
        _engine.save_voted_for(std::move(node));
    }
    auto load_voted_for() { return _engine.load_voted_for(); }

    auto append_log_entry(const log_entry_t& entry) -> void { _engine.append_log_entry(entry); }
    template<typename LogIndex> auto get_log_entry(LogIndex index) {
        return _engine.get_log_entry(index);
    }
    template<typename LogIndex> auto get_log_entries(LogIndex start, LogIndex end) {
        return _engine.get_log_entries(start, end);
    }
    auto get_last_log_index() { return _engine.get_last_log_index(); }
    template<typename LogIndex> auto truncate_log(LogIndex index) -> void {
        _engine.truncate_log(index);
    }

    auto save_snapshot(const snapshot_t& snap) -> void { _engine.save_snapshot(snap); }
    auto load_snapshot() { return _engine.load_snapshot(); }
    template<typename LogIndex> auto delete_log_entries_before(LogIndex index) -> void {
        _engine.delete_log_entries_before(index);
    }

    // ── batched_persistence_engine, conditionally ────────────────────────────

    auto begin_batch() -> void
    requires batched_persistence_engine<Engine>
    {
        _engine.begin_batch();
    }
    auto commit_batch() -> void
    requires batched_persistence_engine<Engine>
    {
        _engine.commit_batch();
    }
    auto abort_batch() -> void
    requires batched_persistence_engine<Engine>
    {
        _engine.abort_batch();
    }

private:
    GroupId _group;
    Engine _engine;
};

// ─────────────────────────────────────────────────────────────────────────────
// Tombstones
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Why a group's replica was destroyed on this node.
enum class tombstone_reason : std::uint8_t {
    merged_away = 0,      ///< The group's range was absorbed by a merge survivor.
    replica_removed = 1,  ///< This node was removed from the group's configuration.
    admin = 2,            ///< An operator destroyed it explicitly.
};

inline auto operator<<(std::ostream& os, tombstone_reason r) -> std::ostream& {
    switch (r) {
        case tombstone_reason::merged_away:
            return os << "merged_away";
        case tombstone_reason::replica_removed:
            return os << "replica_removed";
        case tombstone_reason::admin:
            return os << "admin";
        default:
            return os << "unknown";
    }
}

/// @brief One tombstone record.
struct tombstone_record {
    tombstone_reason _reason{tombstone_reason::merged_away};
    /// @brief When the group was destroyed, as milliseconds since the Unix epoch.
    ///
    /// A wall-clock stamp rather than a steady-clock one because this record is
    /// durable and must survive a restart, which a steady clock's epoch does
    /// not. Nothing about correctness depends on it — it drives only garbage
    /// collection, which is why a clock this design is otherwise careful not to
    /// trust is acceptable here.
    std::int64_t _destroyed_at_ms{0};
};

/// @brief The set of groups destroyed on this node.
///
/// Without it, a partitioned peer's stale `AppendEntries` for a merged-away
/// group re-creates a replica whose range someone else now owns — two shards
/// owning one range, which the tiling invariant catches only after the damage.
/// The transport consults this before its unknown-group path, so a tombstoned
/// message never even reaches the host's replica-creation logic.
///
/// **The clock is a parameter, never read internally.** `insert()` and `gc()`
/// both take the current time from the caller. That keeps garbage collection
/// deterministic in tests, and — more to the point — keeps a durable structure
/// from acquiring a hidden dependency on a clock the rest of this design
/// deliberately does not assume is synchronised.
///
/// @tparam GroupId Must satisfy `raft_group_id`.
template<raft_group_id GroupId = std::uint64_t> class tombstone_set {
public:
    using clock_type = std::chrono::system_clock;

    tombstone_set() = default;

    /// @brief Record that `group` was destroyed here. Re-inserting keeps the
    /// *earliest* record, so a redelivered destroy does not extend the horizon.
    auto insert(const GroupId& group, tombstone_reason reason, clock_type::time_point when)
        -> void {
        const auto ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(when.time_since_epoch()).count();
        auto [it, inserted] = _records.try_emplace(
            group, tombstone_record{._reason = reason, ._destroyed_at_ms = ms});
        if (!inserted && ms < it->second._destroyed_at_ms) {
            it->second = tombstone_record{._reason = reason, ._destroyed_at_ms = ms};
        }
    }

    [[nodiscard]] auto contains(const GroupId& group) const -> bool {
        return _records.contains(group);
    }

    [[nodiscard]] auto find(const GroupId& group) const -> std::optional<tombstone_record> {
        auto it = _records.find(group);
        return it == _records.end() ? std::nullopt : std::optional{it->second};
    }

    [[nodiscard]] auto size() const -> std::size_t { return _records.size(); }
    [[nodiscard]] auto empty() const -> bool { return _records.empty(); }

    /// @brief Drop records older than `horizon`. Returns how many were dropped.
    ///
    /// A tombstone is only needed for as long as a partitioned peer could still
    /// be holding a message for the dead group. Keeping them forever is a slow
    /// leak; dropping them too early re-opens the resurrection window, which is
    /// why the horizon is a knob and defaults conservatively at the call site.
    auto gc(clock_type::time_point now, std::chrono::milliseconds horizon) -> std::size_t {
        const auto cutoff =
            std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() -
            horizon.count();
        std::size_t removed = 0;
        for (auto it = _records.begin(); it != _records.end();) {
            if (it->second._destroyed_at_ms < cutoff) {
                it = _records.erase(it);
                ++removed;
            } else {
                ++it;
            }
        }
        return removed;
    }

    auto erase(const GroupId& group) -> bool { return _records.erase(group) > 0; }
    auto clear() -> void { _records.clear(); }

    [[nodiscard]] auto records() const -> const std::unordered_map<GroupId, tombstone_record>& {
        return _records;
    }

    // ── durability ───────────────────────────────────────────────────────────

    /// @brief Render the set as a line-oriented text form.
    ///
    /// One record per line: `<group>\t<reason>\t<destroyed_at_ms>`. Text rather
    /// than a packed binary form because this file is the thing an operator
    /// reads when asking "why is this node dropping messages for group 47?",
    /// and it is small enough that the encoding costs nothing.
    [[nodiscard]] auto to_text() const -> std::string {
        std::ostringstream os;
        for (const auto& [group, rec] : _records) {
            os << render_group(group) << '\t' << static_cast<int>(rec._reason) << '\t'
               << rec._destroyed_at_ms << '\n';
        }
        return os.str();
    }

    /// @brief Rebuild a set from `to_text()` output. Unparsable lines are skipped.
    ///
    /// Skipping rather than throwing is deliberate: a truncated tombstone file
    /// after a crash must not stop the node from starting. The cost of losing
    /// one tombstone is a resurrection window that the epoch check still
    /// closes; the cost of refusing to start is the node.
    [[nodiscard]] static auto from_text(const std::string& text) -> tombstone_set {
        tombstone_set out;
        std::istringstream is(text);
        std::string line;
        while (std::getline(is, line)) {
            if (line.empty()) {
                continue;
            }
            const auto first = line.find('\t');
            if (first == std::string::npos) {
                continue;
            }
            const auto second = line.find('\t', first + 1);
            if (second == std::string::npos) {
                continue;
            }
            auto group = parse_group(line.substr(0, first));
            if (!group.has_value()) {
                continue;
            }
            try {
                const auto reason = static_cast<tombstone_reason>(
                    std::stoi(line.substr(first + 1, second - first - 1)));
                const auto ms = static_cast<std::int64_t>(std::stoll(line.substr(second + 1)));
                out._records.insert_or_assign(
                    *group, tombstone_record{._reason = reason, ._destroyed_at_ms = ms});
            } catch (const std::exception&) {
                continue;
            }
        }
        return out;
    }

    /// @brief Write the set to `path` via write-then-rename.
    auto save_to_file(const std::filesystem::path& path) const -> void {
        auto tmp = path;
        tmp += ".tmp";
        {
            std::ofstream f(tmp, std::ios::trunc | std::ios::binary);
            if (!f) {
                throw persistence_exception("tombstone_set: cannot write " + tmp.string());
            }
            const auto text = to_text();
            f.write(text.data(), static_cast<std::streamsize>(text.size()));
        }
        std::filesystem::rename(tmp, path);
    }

    /// @brief Read the set from `path`; a missing file yields an empty set.
    [[nodiscard]] static auto load_from_file(const std::filesystem::path& path) -> tombstone_set {
        std::ifstream f(path, std::ios::binary);
        if (!f) {
            return tombstone_set{};
        }
        return from_text(std::string(std::istreambuf_iterator<char>(f), {}));
    }

private:
    [[nodiscard]] static auto render_group(const GroupId& g) -> std::string {
        if constexpr (std::same_as<GroupId, std::string>) {
            return g;
        } else {
            return std::to_string(g);
        }
    }

    [[nodiscard]] static auto parse_group(const std::string& s) -> std::optional<GroupId> {
        if constexpr (std::same_as<GroupId, std::string>) {
            return s;
        } else {
            try {
                return static_cast<GroupId>(std::stoull(s));
            } catch (const std::exception&) {
                return std::nullopt;
            }
        }
    }

    std::unordered_map<GroupId, tombstone_record> _records;
};

}  // namespace kythira
