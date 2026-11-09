// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file persistent_test_state_machine.hpp
/// @brief `test_key_value_state_machine`'s commands on a `kv_engine`
///        (.kiro/specs/persistent-state-machine/ task 4).
///
/// The multi-Raft split and merge suites run once over
/// `test_key_value_state_machine` and once over this class, so the persistent
/// path (split by range, merge by adopting a range, no copies) is held to the
/// same tests as the byte-buffer one. It takes the same commands (the same
/// `make_*_command` encodings), answers the same test helpers (`size`,
/// `contains`, `get_value`, `get_state`), and offers the same split hooks
/// (`suggest_split_keys`, `can_split_at`, `set_split_veto`).
///
/// Layout, for a host namespace `ns` and a shard range `[start, end)`, as in
/// the Redis module's `persistent_kv_state_machine`:
///
/// - `data`: `ns + key` → value.
/// - `meta`: one record at `ns + start`, holding the applied index as decimal
///   text. Inside the group's own range, so a checkpoint carries it and a
///   drop removes it.
///
/// `get_state()` serializes the range in `test_key_value_state_machine`'s
/// format for tests that compare replicas byte for byte. The node never calls
/// it: the type has no `restore_from_snapshot`, so it is not a
/// `state_machine`, and `node<Types>` takes the persistent path.

#include <raft/fault_injection.hpp>
#include <raft/memory_kv_engine.hpp>
#include <raft/persistent_state_machine.hpp>
#include <raft/test_state_machine.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace kythira {

template<typename LogIndex = std::uint64_t> class persistent_test_kv_state_machine {
    using commands = test_key_value_state_machine<LogIndex>;

public:
    using engine_type = memory_kv_engine;
    using command_type = typename commands::command_type;

    explicit persistent_test_kv_state_machine(std::string ns = {}) : _ns(std::move(ns)) {}

    // -- persistent_state_machine ------------------------------------------

    auto open(engine_type& engine, const key_range& range) -> void {
        _engine = &engine;
        _range = range;
        _ring.clear();
        _applied = 0;
        if (const auto v = _engine->get(column_family::meta, meta_key())) {
            _applied = static_cast<LogIndex>(std::stoull(*v));
        }
        _durable_floor = _applied;
    }

    auto apply(const std::vector<std::byte>& command, LogIndex index) -> std::vector<std::byte> {
        fiu_do_on("raft/state_machine/apply",
                  throw std::runtime_error("chaos: state_machine/apply"););
        if (index <= _applied) {
            return {};
        }
        write_batch batch;
        auto result = stage(command, batch);
        commit_staged(batch, index);
        return result;
    }

    // -- persistent_staged_state_machine -----------------------------------

    /// Adds `command`'s effects to `batch`. Reads see what earlier `stage`
    /// calls of the same entry wrote.
    auto stage(const std::vector<std::byte>& command, write_batch& batch)
        -> std::vector<std::byte> {
        if (command.empty()) {
            throw std::invalid_argument("Empty command");
        }
        const auto c = decode(command);
        std::vector<std::byte> result;
        switch (c.type) {
            case command_type::put:
                put_staged(batch, c.key, c.value);
                break;
            case command_type::get:
                if (const auto v = read_staged(c.key)) {
                    result = to_bytes(*v);
                }
                break;
            case command_type::del:
                batch.erase(column_family::data, _ns + c.key);
                _staged[_ns + c.key] = std::nullopt;
                break;
            case command_type::cas: {
                const auto current = read_staged(c.key);
                const bool matches = c.expected.has_value()
                                         ? (current.has_value() && *current == *c.expected)
                                         : !current.has_value();
                if (matches) {
                    put_staged(batch, c.key, c.value);
                }
                result = {matches ? std::byte{1} : std::byte{0}};
                break;
            }
            default:
                throw std::invalid_argument("Unknown command type");
        }
        return result;
    }

    /// Writes `batch` with `index` as the applied index, in one write.
    auto commit_staged(write_batch& batch, LogIndex index) -> void {
        _staged.clear();
        batch.put(column_family::meta, meta_key(), std::to_string(index));
        const auto seq = engine().write(batch);
        _applied = index;
        _ring.emplace_back(index, seq);
        trim_ring();
    }

    auto discard_staged() -> void { _staged.clear(); }

    [[nodiscard]] auto key_prefix() const -> const std::string& { return _ns; }

    /// `key`'s value as the entry being staged sees it: what an earlier
    /// `stage` of this entry wrote, else the engine's.
    [[nodiscard]] auto read_staged(const std::string& key) const -> std::optional<std::string> {
        if (const auto it = _staged.find(_ns + key); it != _staged.end()) {
            return it->second;
        }
        return engine().get(column_family::data, _ns + key);
    }

    /// Stages a put of `key`, for a wrapper's own commands (the bank test's
    /// `add`).
    auto put_staged(write_batch& batch, const std::string& key, const std::string& value) -> void {
        batch.put(column_family::data, _ns + key, value);
        _staged[_ns + key] = value;
    }

    [[nodiscard]] auto applied_index() const -> LogIndex { return _applied; }

    [[nodiscard]] auto durable_applied_index() const -> LogIndex {
        const auto durable_seq = engine().durable_sequence();
        LogIndex best = _durable_floor;
        for (const auto& [index, seq] : _ring) {
            if (seq > durable_seq) {
                break;
            }
            best = index;
        }
        return best;
    }

    /// Makes every batch applied so far durable, so the node can compact
    /// through `applied_index()` on an engine that syncs only when asked.
    auto sync_durable() -> void { engine().sync(engine().latest_sequence()); }

    auto checkpoint(const std::filesystem::path& dir) -> snapshot_manifest {
        return engine().checkpoint(engine_range(), dir);
    }

    auto ingest(const std::filesystem::path& dir, const snapshot_manifest& manifest, LogIndex index)
        -> void {
        engine().ingest(engine_range(), dir, manifest);
        _applied = index;
        _durable_floor = index;
        _ring.clear();
    }

    auto drop() -> void { engine().drop(engine_range()); }

    // -- persistent_splittable_state_machine --------------------------------

    /// Narrows this range to the part below the first cut and returns the
    /// child ranges in key order. One batch writes each child's meta record,
    /// carrying this group's applied index, and moves no data key.
    auto split_at(const std::vector<std::string>& keys) -> std::vector<key_range> {
        std::vector<std::string> cuts = keys;
        std::ranges::sort(cuts);
        cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
        std::erase_if(cuts, [&](const std::string& k) { return !_range.contains(k); });
        if (cuts.empty()) {
            return {};
        }
        if (cuts.front() == _range.start) {
            throw std::invalid_argument("persistent_test_kv_state_machine: split at range start");
        }
        std::vector<key_range> children;
        write_batch batch;
        for (std::size_t i = 0; i < cuts.size(); ++i) {
            children.push_back(key_range{cuts[i], i + 1 < cuts.size()
                                                      ? std::optional<std::string>(cuts[i + 1])
                                                      : _range.end});
            batch.put(column_family::meta, _ns + cuts[i], std::to_string(_applied));
        }
        engine().write(batch);
        _range.end = cuts.front();
        return children;
    }

    /// Widens this range over an adjacent `range` of the same engine, on
    /// either side, erasing the source's meta record in the same batch. No
    /// data key moves. A range already covered is a no-op.
    auto adopt(const key_range& range) -> void {
        if (_range.contains(range)) {
            return;
        }
        write_batch batch;
        if (_range.end.has_value() && range.start == *_range.end) {
            batch.erase(column_family::meta, _ns + range.start);
            _range.end = range.end;
        } else if (range.end.has_value() && *range.end == _range.start) {
            // The record moves to the new start, which is the source's key.
            batch.erase(column_family::meta, meta_key());
            _range.start = range.start;
        } else {
            throw std::invalid_argument(
                "persistent_test_kv_state_machine: adopt of a range that is not adjacent");
        }
        batch.put(column_family::meta, meta_key(), std::to_string(_applied));
        engine().write(batch);
    }

    // -- split policy hooks (split_policy_state_machine) --------------------

    [[nodiscard]] auto approximate_size_bytes() const -> std::size_t {
        std::size_t total = sizeof(std::uint64_t);
        for_each([&](const std::string& k, const std::string& v) {
            total += 2 * sizeof(std::uint32_t) + k.size() + v.size();
        });
        return total;
    }
    [[nodiscard]] auto approximate_key_count() const -> std::size_t { return size(); }

    /// As `test_key_value_state_machine::suggest_split_keys`: evenly spaced
    /// by key count, never the first key.
    [[nodiscard]] auto suggest_split_keys(std::size_t max) -> std::vector<std::string> {
        std::vector<std::string> keys;
        for_each([&](const std::string& k, const std::string&) { keys.push_back(k); });
        std::vector<std::string> out;
        if (max == 0 || keys.size() < 2) {
            return out;
        }
        const std::size_t wanted = std::min(max, keys.size() - 1);
        const std::size_t stride = keys.size() / (wanted + 1);
        if (stride == 0) {
            return out;
        }
        for (std::size_t i = 1; i <= keys.size() && out.size() < wanted; ++i) {
            if (i % stride == 0 && i < keys.size()) {
                out.push_back(keys[i - 1]);
            }
        }
        return out;
    }

    [[nodiscard]] auto can_split_at(const std::string& key) const -> bool {
        return _split_veto ? !_split_veto(key) : true;
    }
    auto set_split_veto(std::function<bool(const std::string&)> veto) -> void {
        _split_veto = std::move(veto);
    }

    // -- test helpers, as test_key_value_state_machine's --------------------

    [[nodiscard]] auto size() const -> std::size_t {
        std::size_t n = 0;
        for_each([&](const std::string&, const std::string&) { ++n; });
        return n;
    }
    [[nodiscard]] auto contains(const std::string& key) const -> bool {
        return get_value(key).has_value();
    }
    [[nodiscard]] auto get_value(const std::string& key) const -> std::optional<std::string> {
        if (_engine == nullptr || !_range.contains(key)) {
            return std::nullopt;
        }
        return _engine->get(column_family::data, _ns + key);
    }
    [[nodiscard]] auto get_last_applied_index() const -> LogIndex { return _applied; }
    [[nodiscard]] auto range() const -> const key_range& { return _range; }

    /// The range in `test_key_value_state_machine::get_state()`'s format, for
    /// tests that compare replicas. Not part of any concept the node uses.
    [[nodiscard]] auto get_state() const -> std::vector<std::byte> {
        std::map<std::string, std::string> store;
        for_each([&](const std::string& k, const std::string& v) { store.emplace(k, v); });
        return commands::serialize_store(store);
    }

    /// Reads `get_state()`'s output back, as the byte-buffer machine's does.
    [[nodiscard]] static auto deserialize_store(const std::vector<std::byte>& blob)
        -> std::map<std::string, std::string> {
        return commands::deserialize_store(blob);
    }

    static auto make_put_command(const std::string& key, const std::string& value)
        -> std::vector<std::byte> {
        return commands::make_put_command(key, value);
    }
    static auto make_get_command(const std::string& key) -> std::vector<std::byte> {
        return commands::make_get_command(key);
    }
    static auto make_del_command(const std::string& key) -> std::vector<std::byte> {
        return commands::make_del_command(key);
    }
    static auto make_cas_command(const std::string& key, const std::optional<std::string>& expected,
                                 const std::string& value) -> std::vector<std::byte> {
        return commands::make_cas_command(key, expected, value);
    }

private:
    struct decoded {
        command_type type{};
        std::string key;
        std::string value;
        std::optional<std::string> expected;
    };

    static auto decode(const std::vector<std::byte>& command) -> decoded {
        decoded out;
        out.type = static_cast<command_type>(command[0]);
        std::size_t offset = 1;
        auto read_string = [&](const char* what) -> std::string {
            std::uint32_t length{};
            if (offset + sizeof(length) > command.size()) {
                throw std::invalid_argument(std::string("Invalid command: missing ") + what);
            }
            std::memcpy(&length, &command[offset], sizeof(length));
            offset += sizeof(length);
            if (offset + length > command.size()) {
                throw std::invalid_argument(std::string("Invalid command: short ") + what);
            }
            std::string s(reinterpret_cast<const char*>(&command[offset]),  // NOLINT
                          length);
            offset += length;
            return s;
        };
        out.key = read_string("key");
        if (out.type == command_type::put) {
            out.value = read_string("value");
        } else if (out.type == command_type::cas) {
            if (offset + 1 > command.size()) {
                throw std::invalid_argument("Invalid CAS command: missing expected flag");
            }
            const bool present = command[offset] != std::byte{0};
            offset += 1;
            auto expected = read_string("expected");
            if (present) {
                out.expected = std::move(expected);
            }
            out.value = read_string("value");
        }
        return out;
    }

    static auto to_bytes(const std::string& s) -> std::vector<std::byte> {
        std::vector<std::byte> out(s.size());
        std::memcpy(out.data(), s.data(), s.size());
        return out;
    }

    template<typename F> auto for_each(F&& f) const -> void {
        if (_engine == nullptr) {
            return;
        }
        _engine->scan(column_family::data, engine_range(), scan_direction::forward,
                      [&](std::string_view k, std::string_view v) {
                          f(std::string(k.substr(_ns.size())), std::string(v));
                          return true;
                      });
    }

    [[nodiscard]] auto engine() const -> engine_type& {
        if (_engine == nullptr) {
            throw std::logic_error("persistent_test_kv_state_machine: not opened");
        }
        return *_engine;
    }
    [[nodiscard]] auto meta_key() const -> std::string { return _ns + _range.start; }
    [[nodiscard]] auto engine_range() const -> key_range {
        std::optional<std::string> end;
        if (_range.end.has_value()) {
            end = _ns + *_range.end;
        } else if (!_ns.empty()) {
            // The smallest key above every key with this prefix.
            std::string succ = _ns;
            while (!succ.empty() && static_cast<unsigned char>(succ.back()) == 0xFF) {
                succ.pop_back();
            }
            if (!succ.empty()) {
                succ.back() = static_cast<char>(static_cast<unsigned char>(succ.back()) + 1);
                end = succ;
            }
        }
        return key_range{_ns + _range.start, end};
    }
    auto trim_ring() -> void {
        const auto durable_seq = engine().durable_sequence();
        while (_ring.size() > 1 && _ring[1].second <= durable_seq) {
            _durable_floor = _ring.front().first;
            _ring.pop_front();
        }
    }

    std::string _ns;
    engine_type* _engine{nullptr};
    key_range _range{};
    LogIndex _applied{0};
    LogIndex _durable_floor{0};
    std::deque<std::pair<LogIndex, engine_sequence>> _ring;
    std::map<std::string, std::optional<std::string>, std::less<>> _staged;
    std::function<bool(const std::string&)> _split_veto;
};

static_assert(persistent_splittable_state_machine<persistent_test_kv_state_machine<>>);
static_assert(persistent_staged_state_machine<persistent_test_kv_state_machine<>>);
static_assert(!state_machine<persistent_test_kv_state_machine<>, std::uint64_t>);

}  // namespace kythira
