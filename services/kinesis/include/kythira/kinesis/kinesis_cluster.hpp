// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file kinesis_cluster.hpp
/// @brief The cluster as the Kinesis operation handlers see it
///        (`.kiro/specs/kinesis-compatible-streams/` task 6).
///
/// Handlers never touch Raft directly. They propose a `stream_command`
/// under its routing key and read the state machine of the group that
/// owns a key; `stream_cluster` is that surface. The daemon implements it
/// over its `multi_raft` host (proposals forwarded to the owning group's
/// leader, reads served by the local replica); `local_stream_cluster`
/// implements it over one in-process state machine holding every key, for
/// tests and a single-node development mode.

#include <kythira/kinesis/kinesis_stream_state_machine.hpp>
#include <kythira/kinesis/kinesis_throttle.hpp>

#include <chrono>
#include <concepts>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace kythira::kinesis {

using stream_reader = std::function<void(const stream_state_machine<std::uint64_t>&)>;

/// What a handler needs from the cluster:
///
/// - `propose(key, command, admission)`: commit `command` (an encoded
///   `stream_command`) in the group owning `key` and return its decoded
///   result, or nullopt when it could not be committed in time (no leader,
///   timeout). A nullopt is ambiguous: the command may still commit. With
///   `admission::shard_limits` an append first meets the write buckets of
///   whichever node proposes it (`kinesis_throttle.hpp`).
/// - `read(key, f)`: run `f` on the state machine of the group owning `key`,
///   as this node's replica has it; false when this node cannot (no local
///   replica of that group).
/// - `now_ms()`: the wall clock leaders stamp arrival times and creation
///   times with (Requirement 8.3).
template<typename C>
concept stream_cluster = requires(C& c, const std::string& key, std::vector<std::byte> command,
                                  stream_reader f, admission a) {
    { c.propose(key, std::move(command), a) } -> std::same_as<std::optional<apply_result>>;
    { c.read(key, f) } -> std::same_as<bool>;
    { c.now_ms() } -> std::same_as<std::int64_t>;
};

/// Optional: `group_of(key)` names the group owning `key` as this node
/// routes it now, or nullopt when unknown. A cluster that has it lets
/// `PutRecords` commit the appends of shards sharing a group as one entry
/// (`append_batch_command`); without it each shard is its own proposal.
template<typename C>
concept groups_keys = requires(C& c, const std::string& key) {
    { c.group_of(key) } -> std::same_as<std::optional<std::uint64_t>>;
};

/// Every key in one in-process state machine: a one-group cluster with no
/// replication. Thread-safe; proposals apply in arrival order.
class local_stream_cluster {
public:
    using clock_fn = std::function<std::int64_t()>;

    explicit local_stream_cluster(
        record_segment_store* store = nullptr,
        clock_fn clock =
            [] {
                return std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                    .count();
            })
        : _machine(store), _clock(std::move(clock)) {}

    auto propose(const std::string& /*key*/, std::vector<std::byte> command,
                 admission mode = admission::none) -> std::optional<apply_result> {
        const std::scoped_lock lock(_mutex);
        if (_unavailable) {
            return std::nullopt;
        }
        return propose_admitted(_throttle, mode, command, [this](const auto& bytes) {
            return std::optional{decode_result(_machine.apply(bytes, ++_index))};
        });
    }

    /// The write buckets proposals with `admission::shard_limits` meet; null
    /// (the default) admits everything. Must outlive this object.
    void set_write_throttle(write_throttle* t) {
        const std::scoped_lock lock(_mutex);
        _throttle = t;
    }

    auto read(const std::string& /*key*/, const stream_reader& f) -> bool {
        const std::scoped_lock lock(_mutex);
        f(_machine);
        return true;
    }

    [[nodiscard]] auto now_ms() const -> std::int64_t { return _clock(); }

    /// One group holds every key.
    [[nodiscard]] auto group_of(const std::string& /*key*/) const -> std::optional<std::uint64_t> {
        return 0;
    }

    /// Proposals so far (tests: how many entries a request cost).
    [[nodiscard]] auto proposals() -> std::uint64_t {
        const std::scoped_lock lock(_mutex);
        return _index;
    }

    /// Tests: make every proposal fail as a lost leader would.
    void set_unavailable(bool unavailable) {
        const std::scoped_lock lock(_mutex);
        _unavailable = unavailable;
    }

private:
    std::mutex _mutex;
    stream_state_machine<std::uint64_t> _machine;
    std::uint64_t _index = 0;
    bool _unavailable = false;
    write_throttle* _throttle = nullptr;
    clock_fn _clock;
};

static_assert(stream_cluster<local_stream_cluster>);
static_assert(groups_keys<local_stream_cluster>);

}  // namespace kythira::kinesis
