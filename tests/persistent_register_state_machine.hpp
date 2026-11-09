// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

// The smallest persistent state machine that exercises `node<Types>`'s
// persistent path (.kiro/specs/persistent-state-machine/ task 3): a map of
// registers kept in a `memory_kv_engine`. A command is `key=value` and sets
// one register, or `+key`, which adds one to a counter. The engine is a template parameter,
// `memory_kv_engine` by default; the RocksDB build of the node suite uses `rocksdb_kv_engine`.
// Every apply writes the value and the applied index in one
// batch, and `durable_applied_index()` follows the engine's durable
// sequence, so a test can hold durability back, crash the engine, and watch
// the node replay from the log.
//
// The reference state machine is `persistent_kv_state_machine` in the Redis
// module; core tests cannot use it, and they need nothing of Redis.

#include <raft/memory_kv_engine.hpp>
#include <raft/persistent_state_machine.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace kythira::testing {

template<kv_engine Engine = memory_kv_engine> class basic_persistent_register_state_machine {
public:
    using engine_type = Engine;

    static constexpr std::string_view k_applied_key = "applied";

    basic_persistent_register_state_machine() = default;

    // -- persistent_state_machine ------------------------------------------

    auto open(engine_type& engine, const key_range& range) -> void {
        _engine = &engine;
        _range = range;
        _ring.clear();
        _applied = 0;
        if (const auto v = _engine->get(column_family::meta, meta_key())) {
            _applied = std::stoull(*v);
        }
        // What survived the open is durable by definition.
        _durable_floor = _applied;
        ++_opens;
    }

    auto apply(const std::vector<std::byte>& command, std::uint64_t index)
        -> std::vector<std::byte> {
        if (index <= _applied) {
            return {};
        }
        const std::string text(reinterpret_cast<const char*>(command.data()), command.size());
        const auto eq = text.find('=');
        write_batch batch;
        if (!text.empty() && text.front() == '+') {
            // `+key` adds one to a counter, so a test can tell an entry
            // applied twice from one applied once.
            const auto k = _range.start + text.substr(1);
            const auto now = engine().get(column_family::data, k);
            batch.put(column_family::data, k, std::to_string(now ? std::stoull(*now) + 1 : 1));
        } else if (eq != std::string::npos) {
            batch.put(column_family::data, _range.start + text.substr(0, eq), text.substr(eq + 1));
        }
        batch.put(column_family::meta, meta_key(), std::to_string(index));
        const auto seq = engine().write(batch);
        _applied = index;
        _ring.emplace_back(index, seq);
        ++_applies;
        return command;
    }

    [[nodiscard]] auto applied_index() const -> std::uint64_t { return _applied; }

    [[nodiscard]] auto durable_applied_index() const -> std::uint64_t {
        const auto durable_seq = engine().durable_sequence();
        std::uint64_t best = _durable_floor;
        for (const auto& [index, seq] : _ring) {
            if (seq <= durable_seq) {
                best = index;
            } else {
                break;
            }
        }
        return best;
    }

    auto checkpoint(const std::filesystem::path& dir) -> snapshot_manifest {
        return engine().checkpoint(_range, dir);
    }

    auto ingest(const std::filesystem::path& dir, const snapshot_manifest& manifest,
                std::uint64_t index) -> void {
        engine().ingest(_range, dir, manifest);
        _applied = index;
        _durable_floor = index;
        _ring.clear();
    }

    auto drop() -> void { engine().drop(_range); }

    // Optional: lets the node sync before compacting (Requirement 3.3).
    auto sync_durable() -> void {
        engine().sync(engine().latest_sequence());
        ++_syncs;
    }

    // -- test helpers ------------------------------------------------------

    [[nodiscard]] auto value_of(const std::string& key) const -> std::optional<std::string> {
        return engine().get(column_family::data, _range.start + key);
    }
    [[nodiscard]] auto applies() const -> std::size_t { return _applies; }
    [[nodiscard]] auto syncs() const -> std::size_t { return _syncs; }
    [[nodiscard]] auto opens() const -> std::size_t { return _opens; }

private:
    [[nodiscard]] auto engine() const -> engine_type& {
        if (_engine == nullptr) {
            throw std::logic_error("persistent_register_state_machine: not opened");
        }
        return *_engine;
    }
    [[nodiscard]] auto meta_key() const -> std::string {
        return _range.start + std::string(k_applied_key);
    }

    engine_type* _engine{nullptr};
    key_range _range{};
    std::uint64_t _applied{0};
    std::uint64_t _durable_floor{0};
    std::deque<std::pair<std::uint64_t, engine_sequence>> _ring;
    std::size_t _applies{0};
    std::size_t _syncs{0};
    std::size_t _opens{0};
};

using persistent_register_state_machine = basic_persistent_register_state_machine<>;

static_assert(persistent_state_machine<persistent_register_state_machine>);
static_assert(!state_machine<persistent_register_state_machine, std::uint64_t>);

}  // namespace kythira::testing
