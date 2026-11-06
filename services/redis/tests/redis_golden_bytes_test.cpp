// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file redis_golden_bytes_test.cpp
/// @brief Pins the Redis gateway's log-entry and snapshot bytes before the
///        gateway moves into `services/redis/`
///        (.kiro/specs/redis-gateway-module/ task 1, Requirement 7.1).
///
/// Three goldens, each generated from `main` and checked in under
/// `services/redis/tests/golden/redis/` (see `redis_golden_bytes.hpp`):
///
/// - `kv_commands.txt`: the encoding of every `redis_kv_*` command, with
///   edge cases (empty key and value, binary bytes, no expiry, several sweep
///   entries), and a decode round trip of each.
/// - `state_machines.txt`: a fixed, seeded workload applied to every state
///   machine the daemon can be built with, recording each apply's reply and
///   the final `get_state()`:
///   - `redis_kv_state_machine` (the default build);
///   - the same, transaction-wrapped (`CONFIG_REDIS_GATEWAY_TRANSACTIONS`);
///   - `redis_kv_mvcc_state_machine`, transaction-wrapped with `mvcc::on`
///     (`CONFIG_REDIS_GATEWAY_SNAPSHOT_ISOLATION`);
///   - `cache_kv_state_machine` (the disk-backed cache). The daemon never
///     wraps it: config.cpp refuses transactions with file persistence.
///
/// The workload uses the raw `std::mt19937_64` output, never a
/// `std::*_distribution`, whose results differ between standard libraries,
/// and draws only inside braced initialisers (evaluated left to right) or
/// one per statement, never as several function arguments.
/// Expiry deadlines are fixed numbers, never the clock.

#define BOOST_TEST_MODULE redis_golden_bytes_test
#include <boost/test/unit_test.hpp>

#include "redis_golden_bytes.hpp"

#include <kythira/redis/cache_chunk_store.hpp>
#include <kythira/redis/cache_kv_state_machine.hpp>
#include <kythira/redis/redis_kv_commands.hpp>
#include <kythira/redis/redis_kv_mvcc_state_machine.hpp>
#include <kythira/redis/redis_kv_state_machine.hpp>
#include <raft/txn_envelope.hpp>
#include <raft/txn_state_machine.hpp>

#include <unistd.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <random>
#include <string>
#include <variant>
#include <vector>

using namespace kythira;
using kythira::testing::golden::check_golden;
using kythira::testing::golden::escape;
using kythira::testing::golden::hex_block;

namespace {

constexpr std::uint64_t k_seed = 0x6b79746869726121ULL;  // "kythira!"
/// A fixed wall-clock deadline (2030-01-01T00:00:00Z in ms), never `now()`.
constexpr std::uint64_t k_deadline_ms = 1'893'456'000'000ULL;

auto bytes_of(std::string_view s) -> std::vector<std::byte> {
    std::vector<std::byte> out(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        out[i] = static_cast<std::byte>(s[i]);
    }
    return out;
}

auto as_text(const std::vector<std::byte>& b) -> std::string {
    return {reinterpret_cast<const char*>(b.data()), b.size()};  // NOLINT
}

auto sha_of_text(std::string_view s) -> redis_kv_sha256 {
    return sha256_of(bytes_of(s));
}

// ── kv_commands.txt ──────────────────────────────────────────────────────────

struct named_command {
    std::string _name;
    redis_kv_command _cmd;
};

auto every_command() -> std::vector<named_command> {
    const std::string binary("a\r\n\0b\xff", 6);
    return {
        {"set plain", redis_kv_set_command{"sccache/obj1", bytes_of("object-bytes"), 0}},
        {"set with expiry", redis_kv_set_command{"sccache/obj2", bytes_of("v"), k_deadline_ms}},
        {"set binary value", redis_kv_set_command{"sccache/bin", bytes_of(binary), 0}},
        {"set empty key and value", redis_kv_set_command{"", {}, 0}},
        {"del", redis_kv_del_command{"sccache/obj1"}},
        {"del empty key", redis_kv_del_command{""}},
        {"sweep none", redis_kv_sweep_command{}},
        {"sweep two", redis_kv_sweep_command{{{"sccache/obj2", k_deadline_ms},
                                              {"sccache/obj3", k_deadline_ms + 1}}}},
        {"evict one", redis_kv_evict_command{{"sccache/obj1"}}},
        {"evict three", redis_kv_evict_command{{"a", "bb", "ccc"}}},
        {"put_chunk",
         redis_kv_put_chunk_command{"sccache/big", sha_of_text("chunk-0"), bytes_of("chunk-0")}},
        {"put_chunk empty", redis_kv_put_chunk_command{"sccache/big", sha_of_text(""), {}}},
        {"commit two chunks",
         redis_kv_commit_command{"sccache/big",
                                 0,
                                 14,
                                 sha_of_text("chunk-0chunk-1"),
                                 {sha_of_text("chunk-0"), sha_of_text("chunk-1")}}},
        {"commit with expiry, no chunks",
         redis_kv_commit_command{"sccache/empty", k_deadline_ms, 0, sha_of_text(""), {}}},
        {"touch none", redis_kv_touch_command{}},
        {"touch two", redis_kv_touch_command{{"sccache/obj1", "sccache/big"}}},
    };
}

// ── the seeded workload ──────────────────────────────────────────────────────

/// Keys from a small pool so that sets overwrite and dels hit, values of
/// 0-63 bytes covering every byte value.
class workload {
public:
    workload() : _rng(k_seed) {}

    auto key() -> std::string { return "sccache/k" + std::to_string(next() % 12); }

    auto value() -> std::vector<std::byte> {
        std::vector<std::byte> v(next() % 64);
        for (auto& b : v) {
            b = static_cast<std::byte>(next() & 0xFFU);
        }
        return v;
    }

    auto next() -> std::uint64_t { return _rng(); }

private:
    std::mt19937_64 _rng;
};

/// The plain command stream every in-memory state machine sees: sets with
/// and without expiry, dels that hit and miss, a sweep, an eviction and a
/// touch.
auto kv_stream() -> std::vector<redis_kv_command> {
    workload w;
    std::vector<redis_kv_command> out;
    for (int i = 0; i < 48; ++i) {
        const auto pick = w.next() % 10;
        if (pick < 5) {
            out.emplace_back(redis_kv_set_command{w.key(), w.value(), 0});
        } else if (pick < 7) {
            out.emplace_back(
                redis_kv_set_command{w.key(), w.value(), k_deadline_ms + (w.next() % 1000)});
        } else if (pick < 9) {
            out.emplace_back(redis_kv_del_command{w.key()});
        } else {
            out.emplace_back(redis_kv_touch_command{{w.key(), w.key()}});
        }
    }
    out.emplace_back(
        redis_kv_sweep_command{{{w.key(), k_deadline_ms}, {w.key(), k_deadline_ms + 1}}});
    out.emplace_back(redis_kv_evict_command{{w.key(), w.key(), w.key()}});
    return out;
}

auto command_key(const redis_kv_command& c) -> std::string {
    return redis_kv_command_key(encode_redis_kv_command(c));
}

auto render_apply(std::string& out, std::uint64_t index, const redis_kv_command& c,
                  const std::vector<std::byte>& reply) -> void {
    out += "apply " + std::to_string(index) + " key=" + escape(command_key(c)) + "\n";
    out += hex_block(encode_redis_kv_command(c), "  entry ");
    out += hex_block(reply, "  reply ");
}

auto render_state(std::string& out, const std::vector<std::byte>& state) -> void {
    out += "get_state " + std::to_string(state.size()) + " bytes\n";
    out += hex_block(state, "  ");
}

auto plain_kv() -> std::string {
    std::string out = "## redis_kv_state_machine\n";
    redis_kv_state_machine<std::uint64_t> sm;
    std::uint64_t index = 0;
    for (const auto& c : kv_stream()) {
        auto reply = sm.apply(encode_redis_kv_command(c), ++index);
        render_apply(out, index, c, reply);
    }
    render_state(out, sm.get_state());
    return out;
}

/// The same stream through the transaction wrapper as plain envelopes, then
/// one transaction of each outcome: a one-phase commit, a two-phase commit
/// (prepare, commit primary, commit secondary), an abort, and a prepare left
/// pending, so the snapshot carries an intent and a record.
template<typename Wrapped>
auto wrapped(const std::string& title, Wrapped sm, bool timestamps) -> std::string {
    using env = txn_envelope<std::string>;
    using write = txn_write<std::string>;
    std::string out = "## " + title + "\n";
    std::uint64_t index = 0;
    std::uint64_t ts = 100;
    auto run = [&](const std::string& what, const env& e) {
        const auto entry = encode_txn_envelope<std::string>(e);
        const auto reply = sm.apply(entry, ++index);
        out += "apply " + std::to_string(index) + " " + what + "\n";
        out += hex_block(entry, "  entry ");
        out += hex_block(reply, "  reply ");
    };
    auto set = [](const std::string& k, std::string_view v) {
        return encode_redis_kv_command(redis_kv_set_command{k, bytes_of(v), 0});
    };
    for (const auto& c : kv_stream()) {
        const auto key = command_key(c);
        if (timestamps && std::holds_alternative<redis_kv_set_command>(c)) {
            // Under mvcc::on every write is timestamped (design "all writes
            // carry timestamps"): a one-phase write at the next oracle tick.
            ++ts;
            run("one_phase key=" + escape(key),
                txn_one_phase<std::string>{
                    txn_id{0, ts}, {write{key, encode_redis_kv_command(c)}}, ts, ts});
        } else {
            run("plain key=" + escape(key), txn_plain{encode_redis_kv_command(c)});
        }
    }
    const txn_id one{0x1111, 1};
    const txn_id two{0x2222, 2};
    const txn_id aborted{0x3333, 3};
    const txn_id pending{0x4444, 4};
    run("one_phase",
        txn_one_phase<std::string>{one, {write{"sccache/t1", set("sccache/t1", "one")}}, 200, 201});
    run("prepare primary", txn_prepare<std::string>{two,
                                                    "sccache/t2",
                                                    {write{"sccache/t2", set("sccache/t2", "a")}},
                                                    {"sccache/t1"},
                                                    210,
                                                    k_deadline_ms,
                                                    true});
    run("prepare secondary", txn_prepare<std::string>{two,
                                                      "sccache/t2",
                                                      {write{"sccache/t3", set("sccache/t3", "b")}},
                                                      {},
                                                      210,
                                                      k_deadline_ms,
                                                      false});
    run("commit primary", txn_commit_primary<std::string>{two, "sccache/t2", 211});
    run("commit secondary", txn_commit_secondary<std::string>{two, {"sccache/t3"}, 211});
    run("prepare to abort", txn_prepare<std::string>{aborted,
                                                     "sccache/t4",
                                                     {write{"sccache/t4", set("sccache/t4", "x")}},
                                                     {},
                                                     220,
                                                     k_deadline_ms,
                                                     true});
    run("abort", txn_abort<std::string>{aborted, "sccache/t4", {"sccache/t4"}});
    run("prepare left pending",
        txn_prepare<std::string>{pending,
                                 "sccache/t5",
                                 {write{"sccache/t5", set("sccache/t5", "p")}},
                                 {},
                                 230,
                                 k_deadline_ms,
                                 true});
    render_state(out, sm.get_state());
    return out;
}

/// A per-test chunk store in a fresh temporary directory.
struct chunk_dir {
    std::filesystem::path _dir;
    std::unique_ptr<cache_chunk_store> _store;
    std::shared_ptr<chunk_refcounts> _refs;

    chunk_dir() {
        static std::atomic<int> counter{0};
        _dir = std::filesystem::temp_directory_path() /
               ("kythira_redis_golden_" + std::to_string(::getpid()) + "_" +
                std::to_string(counter.fetch_add(1)));
        std::filesystem::remove_all(_dir);
        cache_chunk_store_options o;
        o._data_dir = _dir;
        o._pack_threshold_bytes = 256;
        _store = std::make_unique<cache_chunk_store>(o);
        _refs = std::make_shared<chunk_refcounts>(*_store);
    }
    ~chunk_dir() {
        _refs.reset();
        _store.reset();
        std::error_code ec;
        std::filesystem::remove_all(_dir, ec);
    }
    chunk_dir(const chunk_dir&) = delete;
    auto operator=(const chunk_dir&) -> chunk_dir& = delete;
    chunk_dir(chunk_dir&&) = delete;
    auto operator=(chunk_dir&&) -> chunk_dir& = delete;
};

/// Uploads (chunks, then a commit) of seeded values split into 16-byte
/// chunks, overwrites, a commit with an expiry, dels, a touch, a sweep and
/// an eviction.
auto cache_kv() -> std::string {
    std::string out = "## cache_kv_state_machine\n";
    chunk_dir dir;
    cache_kv_state_machine<std::uint64_t> sm(dir._refs,
                                             cache_kv_options{._orphan_grace_entries = 1000});
    workload w;
    std::uint64_t index = 0;
    auto run = [&](const redis_kv_command& c) {
        const auto reply = sm.apply(encode_redis_kv_command(c), ++index);
        render_apply(out, index, c, reply);
    };
    auto upload = [&](const std::string& key, std::uint64_t expire) {
        auto value = w.value();
        redis_kv_commit_command commit{key, expire, value.size(), sha256_of(value), {}};
        for (std::size_t off = 0; off < value.size(); off += 16) {
            const std::vector<std::byte> piece(
                value.begin() + static_cast<std::ptrdiff_t>(off),
                value.begin() + static_cast<std::ptrdiff_t>(std::min(off + 16, value.size())));
            const auto sha = sha256_of(piece);
            commit._chunks.push_back(sha);
            run(redis_kv_put_chunk_command{key, sha, piece});
        }
        run(commit);
    };
    for (int i = 0; i < 24; ++i) {
        const auto pick = w.next() % 10;
        // Each draw in its own statement: function arguments are evaluated
        // in an unspecified order, and g++ and clang differ.
        if (pick < 6) {
            const auto key = w.key();
            upload(key, 0);
        } else if (pick < 7) {
            const auto key = w.key();
            const auto expire = k_deadline_ms + (w.next() % 1000);
            upload(key, expire);
        } else if (pick < 9) {
            run(redis_kv_del_command{w.key()});
        } else {
            run(redis_kv_touch_command{{w.key(), w.key()}});
        }
    }
    run(redis_kv_sweep_command{{{w.key(), k_deadline_ms}, {w.key(), k_deadline_ms + 1}}});
    run(redis_kv_evict_command{{w.key(), w.key(), w.key()}});
    render_state(out, sm.get_state());
    return out;
}

}  // namespace

BOOST_AUTO_TEST_CASE(kv_command_encodings_match_the_golden_file, *boost::unit_test::timeout(30)) {
    std::string out =
        "# redis_kv_* command encodings (redis-gateway-module task 1).\n"
        "# Regenerate with KYTHIRA_UPDATE_GOLDENS=1; a change here is a log-entry\n"
        "# format change.\n";
    for (const auto& [name, cmd] : every_command()) {
        const auto bytes = encode_redis_kv_command(cmd);
        out += "command " + name + "\n";
        out += hex_block(bytes);
        // Every encoding decodes back to itself.
        BOOST_CHECK_MESSAGE(encode_redis_kv_command(decode_redis_kv_command(bytes)) == bytes,
                            name << " does not survive a decode round trip");
        out += "  key=" + escape(redis_kv_command_key(bytes)) + "\n";
    }
    check_golden("kv_commands.txt", out);
}

BOOST_AUTO_TEST_CASE(state_machine_bytes_match_the_golden_file, *boost::unit_test::timeout(120)) {
    using txn_kv = transactional_state_machine<redis_kv_state_machine<std::uint64_t>, std::string,
                                               redis_kv_partitioner>;
    using txn_mvcc = transactional_state_machine<redis_kv_mvcc_state_machine<std::uint64_t>,
                                                 std::string, redis_kv_partitioner, mvcc::on>;
    std::string out =
        "# Apply replies and get_state() of every state machine redis_gateway_node\n"
        "# can be built with, on a fixed seeded workload (redis-gateway-module\n"
        "# task 1). Regenerate with KYTHIRA_UPDATE_GOLDENS=1; a change here is a\n"
        "# log-entry or snapshot format change.\n";
    out += plain_kv();
    out += wrapped("transactional_state_machine<redis_kv_state_machine>", txn_kv{}, false);
    out += wrapped("transactional_state_machine<redis_kv_mvcc_state_machine, mvcc::on>", txn_mvcc{},
                   true);
    out += cache_kv();
    check_golden("state_machines.txt", out);
}
