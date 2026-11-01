// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file cache_kv_state_machine_unit_test.cpp
/// @brief `cache_kv_state_machine` (.kiro/specs/disk-backed-build-cache/
///        task 3; Requirements 2.1-2.3, 3.1-3.7, 4.1, 4.2 and 4.4), and the
///        design's correctness properties 1-4 as randomized trials.

#define BOOST_TEST_MODULE cache_kv_state_machine_unit_test
#include <boost/test/unit_test.hpp>

#include <raft/cache_kv_state_machine.hpp>

#include <malloc.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <vector>

#include <unistd.h>

BOOST_TEST_DONT_PRINT_LOG_VALUE(std::vector<std::byte>)

namespace fs = std::filesystem;
using namespace kythira;

namespace {

using sm_type = cache_kv_state_machine<std::uint64_t>;

/// One node: a data directory, its chunk store and node-wide counts.
struct node_fixture {
    fs::path _dir;
    std::unique_ptr<cache_chunk_store> _store;
    std::shared_ptr<chunk_refcounts> _refs;

    node_fixture() {
        static std::atomic<int> counter{0};
        _dir = fs::temp_directory_path() / ("kythira_cache_sm_" + std::to_string(::getpid()) + "_" +
                                            std::to_string(counter.fetch_add(1)));
        fs::remove_all(_dir);
        cache_chunk_store_options o;
        o._data_dir = _dir;
        o._pack_threshold_bytes = 256;
        _store = std::make_unique<cache_chunk_store>(o);
        _refs = std::make_shared<chunk_refcounts>(*_store);
    }
    ~node_fixture() {
        _refs.reset();
        _store.reset();
        std::error_code ec;
        fs::remove_all(_dir, ec);
    }
    node_fixture(const node_fixture&) = delete;
    auto operator=(const node_fixture&) -> node_fixture& = delete;
    node_fixture(node_fixture&&) = delete;
    auto operator=(node_fixture&&) -> node_fixture& = delete;

    [[nodiscard]] auto make_sm(std::uint64_t grace = 1000) const -> sm_type {
        return sm_type(_refs, cache_kv_options{._orphan_grace_entries = grace});
    }
    [[nodiscard]] auto chunks_on_disk() const -> std::size_t {
        std::size_t n = 0;
        _store->for_each_chunk([&n](const chunk_sha&, std::uint64_t) { ++n; });
        return n;
    }
};

auto pattern(std::size_t size, std::uint32_t seed) -> std::vector<std::byte> {
    std::vector<std::byte> out(size);
    std::uint32_t x = (seed * 2654435761U) + 7;
    for (auto& b : out) {
        x ^= x << 13U;
        x ^= x >> 17U;
        x ^= x << 5U;
        b = static_cast<std::byte>(x);
    }
    for (std::size_t i = 0; i < std::min<std::size_t>(size, 4); ++i) {
        out[i] = static_cast<std::byte>(seed >> (8U * i));
    }
    return out;
}

/// The log entries one upload of `value` produces: one `put_chunk` per
/// `chunk` bytes, then the `commit`.
auto upload(const std::string& key, const std::vector<std::byte>& value, std::size_t chunk,
            std::uint64_t expire = 0) -> std::vector<std::vector<std::byte>> {
    std::vector<std::vector<std::byte>> out;
    redis_kv_commit_command c{key, expire, value.size(), sha256_of(value), {}};
    for (std::size_t off = 0; off < value.size(); off += chunk) {
        const std::vector<std::byte> piece(
            value.begin() + static_cast<std::ptrdiff_t>(off),
            value.begin() + static_cast<std::ptrdiff_t>(std::min(value.size(), off + chunk)));
        const auto sha = sha256_of(piece);
        out.push_back(encode_redis_kv_command(redis_kv_put_chunk_command{key, sha, piece}));
        c._chunks.push_back(sha);
    }
    out.push_back(encode_redis_kv_command(c));
    return out;
}

/// Apply entries from `index`, returning the last status byte.
auto apply_all(sm_type& sm, std::uint64_t& index, const std::vector<std::vector<std::byte>>& es)
    -> std::byte {
    std::byte last{0};
    for (const auto& e : es) {
        last = sm.apply(e, ++index).at(0);
    }
    return last;
}

auto apply_one(sm_type& sm, std::uint64_t& index, const std::vector<std::byte>& e) -> std::byte {
    return sm.apply(e, ++index).at(0);
}

/// An entry that changes nothing, to move the log index on.
auto filler() -> std::vector<std::byte> {
    return encode_redis_kv_command(redis_kv_del_command{"\x01no-such-key"});
}

auto value_of(const node_fixture& node, const sm_type& sm, const std::string& key)
    -> std::optional<std::vector<std::byte>> {
    auto m = sm.lookup(key);
    if (!m) {
        return std::nullopt;
    }
    return read_cache_value(*node._store, *m);
}

}  // namespace

BOOST_AUTO_TEST_CASE(upload_commit_and_read_back, *boost::unit_test::timeout(30)) {
    node_fixture node;
    auto sm = node.make_sm();
    std::uint64_t idx = 0;
    const auto value = pattern(10000, 1);
    const auto entries = upload("vcpkg/zlib/1.3.1/abc", value, 4096);
    // Not visible until the commit applies (Requirement 3.2).
    for (std::size_t i = 0; i + 1 < entries.size(); ++i) {
        BOOST_CHECK(apply_one(sm, idx, entries[i]) == std::byte{1});
        BOOST_CHECK(sm.lookup("vcpkg/zlib/1.3.1/abc") == nullptr);
    }
    BOOST_CHECK(apply_one(sm, idx, entries.back()) == std::byte{1});
    auto m = sm.lookup("vcpkg/zlib/1.3.1/abc");
    BOOST_REQUIRE(m != nullptr);
    BOOST_CHECK_EQUAL(m->chunks().size(), 3U);
    BOOST_CHECK(m->has_all());
    BOOST_CHECK(m->_value_sha == sha256_of(value));
    BOOST_CHECK(value_of(node, sm, "vcpkg/zlib/1.3.1/abc") == value);
    BOOST_CHECK_EQUAL(sm.approximate_size_bytes(), 10000U);
    BOOST_CHECK_EQUAL(sm.approximate_key_count(), 1U);
    BOOST_CHECK_EQUAL(sm.last_applied_index(), idx);
}

// `raft`'s node configuration default-constructs the state machine before
// `multi_raft` moves the factory's over it. The placeholder holds nothing,
// refuses to apply rather than lose chunks, and is replaced cleanly.
BOOST_AUTO_TEST_CASE(a_default_constructed_machine_is_an_unbound_placeholder,
                     *boost::unit_test::timeout(30)) {
    node_fixture node;
    sm_type placeholder;
    BOOST_CHECK_EQUAL(placeholder.approximate_key_count(), 0U);
    BOOST_CHECK(placeholder.lookup("k") == nullptr);
    std::uint64_t idx = 0;
    const auto entries = upload("k", pattern(100, 7), 4096);
    BOOST_CHECK_THROW(placeholder.apply(entries.front(), 1), std::logic_error);
    BOOST_CHECK_THROW(placeholder.restore_from_snapshot(placeholder.get_state(), 1),
                      std::logic_error);

    placeholder = node.make_sm();
    for (const auto& e : entries) {
        apply_one(placeholder, idx, e);
    }
    BOOST_CHECK(value_of(node, placeholder, "k") == pattern(100, 7));
}

BOOST_AUTO_TEST_CASE(resp_set_is_one_entry_and_dedupes, *boost::unit_test::timeout(60)) {
    node_fixture node;
    auto sm = node.make_sm();
    std::uint64_t idx = 0;
    const auto big = pattern(cache_set_piece_bytes + 100, 2);
    apply_one(sm, idx, encode_redis_kv_command(redis_kv_set_command{"a", big, 0}));
    apply_one(sm, idx, encode_redis_kv_command(redis_kv_set_command{"b", big, 0}));
    apply_one(sm, idx, encode_redis_kv_command(redis_kv_set_command{"empty", {}, 0}));
    BOOST_CHECK_EQUAL(sm.lookup("a")->chunks().size(), 2U);
    BOOST_CHECK(value_of(node, sm, "a") == big);
    BOOST_CHECK(value_of(node, sm, "b") == big);
    BOOST_CHECK(value_of(node, sm, "empty") == std::vector<std::byte>{});
    BOOST_CHECK(sm.lookup("empty")->chunks().empty());
    // Identical values are stored once per node (Requirement 3.3).
    BOOST_CHECK_EQUAL(node.chunks_on_disk(), 2U);
    // Deleting one copy keeps the chunks; deleting both frees them (3.5).
    apply_one(sm, idx, encode_redis_kv_command(redis_kv_del_command{"a"}));
    BOOST_CHECK_EQUAL(node.chunks_on_disk(), 2U);
    apply_one(sm, idx, encode_redis_kv_command(redis_kv_del_command{"b"}));
    BOOST_CHECK_EQUAL(node.chunks_on_disk(), 0U);
    BOOST_CHECK_EQUAL(sm.referenced_chunk_count(), 0U);
}

BOOST_AUTO_TEST_CASE(replace_releases_the_old_value_but_keeps_shared_chunks,
                     *boost::unit_test::timeout(30)) {
    node_fixture node;
    auto sm = node.make_sm();
    std::uint64_t idx = 0;
    apply_one(sm, idx, encode_redis_kv_command(redis_kv_set_command{"k", pattern(500, 3), 0}));
    BOOST_CHECK_EQUAL(node.chunks_on_disk(), 1U);
    // Same bytes again: the shared chunk must survive the swap.
    apply_one(sm, idx, encode_redis_kv_command(redis_kv_set_command{"k", pattern(500, 3), 9}));
    BOOST_CHECK_EQUAL(node.chunks_on_disk(), 1U);
    BOOST_CHECK(value_of(node, sm, "k") == pattern(500, 3));
    BOOST_CHECK_EQUAL(sm.expiring_key_count(), 1U);
    // Different bytes: the old chunk goes.
    apply_one(sm, idx, encode_redis_kv_command(redis_kv_set_command{"k", pattern(500, 4), 0}));
    BOOST_CHECK_EQUAL(node.chunks_on_disk(), 1U);
    BOOST_CHECK(value_of(node, sm, "k") == pattern(500, 4));
    BOOST_CHECK_EQUAL(sm.expiring_key_count(), 0U);
}

BOOST_AUTO_TEST_CASE(commit_naming_an_unknown_chunk_is_a_no_op, *boost::unit_test::timeout(10)) {
    node_fixture node;
    auto sm = node.make_sm();
    std::uint64_t idx = 0;
    const auto v = pattern(100, 5);
    BOOST_CHECK(apply_one(sm, idx,
                          encode_redis_kv_command(redis_kv_commit_command{
                              "k", 0, v.size(), sha256_of(v), {sha256_of(v)}})) == std::byte{0});
    BOOST_CHECK(sm.lookup("k") == nullptr);
    // A put_chunk whose bytes do not match its name is refused and stores
    // nothing.
    BOOST_CHECK(apply_one(sm, idx,
                          encode_redis_kv_command(redis_kv_put_chunk_command{
                              "k", sha256_of(pattern(100, 6)), v})) == std::byte{0});
    BOOST_CHECK_EQUAL(node.chunks_on_disk(), 0U);
    BOOST_CHECK_EQUAL(sm.pending_chunk_count(), 0U);
}

BOOST_AUTO_TEST_CASE(orphans_expire_after_the_grace_in_entries, *boost::unit_test::timeout(30)) {
    node_fixture node;
    auto sm = node.make_sm(10);
    std::uint64_t idx = 0;
    auto entries = upload("abandoned", pattern(3000, 7), 1000);
    entries.pop_back();  // the client went away before the commit
    apply_all(sm, idx, entries);
    BOOST_CHECK_EQUAL(sm.pending_chunk_count(), 3U);
    BOOST_CHECK_EQUAL(node.chunks_on_disk(), 3U);
    const std::uint64_t last_put = idx;
    while (idx < last_put + 10) {
        apply_one(sm, idx, filler());
    }
    // Exactly the grace: still pending.
    BOOST_CHECK_EQUAL(sm.pending_chunk_count(), 1U);
    apply_one(sm, idx, filler());
    BOOST_CHECK_EQUAL(sm.pending_chunk_count(), 0U);
    BOOST_CHECK_EQUAL(node.chunks_on_disk(), 0U);

    // A commit after the grace binds nothing (the front end answers 503).
    auto late = upload("late", pattern(500, 8), 1000);
    apply_one(sm, idx, late[0]);
    for (int i = 0; i < 11; ++i) {
        apply_one(sm, idx, filler());
    }
    BOOST_CHECK(apply_one(sm, idx, late[1]) == std::byte{0});
    BOOST_CHECK(sm.lookup("late") == nullptr);
}

BOOST_AUTO_TEST_CASE(committed_chunks_outlive_the_grace, *boost::unit_test::timeout(30)) {
    node_fixture node;
    auto sm = node.make_sm(5);
    std::uint64_t idx = 0;
    const auto v = pattern(2000, 9);
    apply_all(sm, idx, upload("k", v, 700));
    for (int i = 0; i < 20; ++i) {
        apply_one(sm, idx, filler());
    }
    BOOST_CHECK_EQUAL(sm.pending_chunk_count(), 0U);
    BOOST_CHECK(value_of(node, sm, "k") == v);
    // Sweep with a stale deadline does nothing; evict removes.
    apply_one(sm, idx, encode_redis_kv_command(redis_kv_sweep_command{{{"k", 5}}}));
    BOOST_CHECK(sm.lookup("k") != nullptr);
    apply_one(sm, idx, encode_redis_kv_command(redis_kv_evict_command{{"k"}}));
    BOOST_CHECK(sm.lookup("k") == nullptr);
    BOOST_CHECK_EQUAL(node.chunks_on_disk(), 0U);
}

/// Requirement 4.1/4.3/4.4: a replica that restores a snapshot naming chunks
/// it does not have serves nothing for those keys until they arrive.
BOOST_AUTO_TEST_CASE(restore_on_an_empty_node_marks_keys_unservable,
                     *boost::unit_test::timeout(30)) {
    node_fixture leader;
    auto sm = leader.make_sm();
    std::uint64_t idx = 0;
    apply_all(sm, idx, upload("one", pattern(1500, 10), 1000));
    apply_all(sm, idx, upload("two", pattern(100, 11), 1000));
    const auto state = sm.get_state();

    node_fixture fresh;
    auto replica = fresh.make_sm();
    replica.restore_from_snapshot(state, idx);
    BOOST_CHECK(replica.get_state() == state);
    BOOST_CHECK_EQUAL(replica.last_applied_index(), idx);
    BOOST_CHECK(!replica.lookup("one")->has_all());
    const auto missing = replica.missing_chunks();
    BOOST_CHECK_EQUAL(missing.size(), 3U);
    BOOST_CHECK_EQUAL(replica.refresh_has_all(), 0U);

    // The fetcher's job, done by hand: copy the bytes over.
    for (const auto& sha : missing) {
        auto bytes = leader._store->read(sha);
        BOOST_REQUIRE(bytes);
        fresh._store->put(sha, *bytes);
    }
    BOOST_CHECK_EQUAL(replica.refresh_has_all(), 2U);
    BOOST_CHECK(value_of(fresh, replica, "one") == pattern(1500, 10));
}

/// Two replicas on one node share one store. A chunk is deleted only when
/// neither references it, and a replica going away deletes nothing.
BOOST_AUTO_TEST_CASE(replicas_on_one_node_share_chunks, *boost::unit_test::timeout(30)) {
    node_fixture node;
    std::uint64_t ia = 0;
    std::uint64_t ib = 0;
    auto a = node.make_sm();
    const auto v = pattern(800, 12);
    apply_one(a, ia, encode_redis_kv_command(redis_kv_set_command{"x", v, 0}));
    {
        auto b = node.make_sm();
        apply_one(b, ib, encode_redis_kv_command(redis_kv_set_command{"y", v, 0}));
        BOOST_CHECK_EQUAL(node._refs->count(sha256_of(v)), 2U);
        apply_one(a, ia, encode_redis_kv_command(redis_kv_del_command{"x"}));
        BOOST_CHECK_EQUAL(node.chunks_on_disk(), 1U);
        // b is destroyed here, still holding "y".
    }
    BOOST_CHECK_EQUAL(node._refs->count(sha256_of(v)), 0U);
    BOOST_CHECK_EQUAL(node.chunks_on_disk(), 1U);
}

BOOST_AUTO_TEST_CASE(move_keeps_references_once, *boost::unit_test::timeout(10)) {
    node_fixture node;
    std::uint64_t idx = 0;
    auto a = node.make_sm();
    apply_one(a, idx, encode_redis_kv_command(redis_kv_set_command{"x", pattern(10, 13), 0}));
    auto b = std::move(a);
    BOOST_CHECK_EQUAL(node._refs->size(), 1U);
    auto c = node.make_sm();
    c = std::move(b);
    BOOST_CHECK_EQUAL(node._refs->size(), 1U);
    BOOST_CHECK(value_of(node, c, "x") == pattern(10, 13));
}

/// Requirement 2.2: no more than 256 bytes per key plus the key, measured
/// with 100,000 sccache-shaped keys (one chunk each), node-wide counts
/// included.
BOOST_AUTO_TEST_CASE(index_memory_per_key, *boost::unit_test::timeout(120)) {
    node_fixture node;
    constexpr std::uint32_t keys = 100000;
    std::vector<std::byte> blob;
    std::size_t key_bytes = 0;
    {
        // Build the state directly: 100,000 commits would need 100,000 chunk
        // files for no extra coverage of the index's footprint.
        using namespace redis_kv_detail;
        put_u32(blob, 2);
        put_u32(blob, keys);
        std::map<std::string, chunk_sha> sorted;
        for (std::uint32_t i = 0; i < keys; ++i) {
            const auto sha = sha256_of(pattern(16, i));
            const auto hex = to_hex(sha);
            sorted.emplace("sccache/" + hex.substr(0, 1) + "/" + hex.substr(1, 1) + "/" +
                               hex.substr(2, 1) + "/" + hex,
                           sha);
        }
        for (const auto& [key, sha] : sorted) {
            key_bytes += key.size();
            put_u32(blob, static_cast<std::uint32_t>(key.size()));
            put_string(blob, key);
            put_u64(blob, 0);
            put_u64(blob, 900000);
            put_bytes(blob, sha);
            put_u32(blob, 1);
            put_bytes(blob, sha);
        }
        put_u32(blob, 0);
    }
    auto sm = node.make_sm();
    ::malloc_trim(0);
    const auto before = ::mallinfo2().uordblks;
    sm.restore_from_snapshot(blob, 1);
    const auto after = ::mallinfo2().uordblks;
    const double per_key = static_cast<double>(after - before) / keys;
    const double key_avg = static_cast<double>(key_bytes) / keys;
    BOOST_TEST_MESSAGE("index bytes per key: " << per_key << " (key itself " << key_avg
                                               << ", budget " << 256 + key_avg << ")");
    BOOST_CHECK_EQUAL(sm.approximate_key_count(), keys);
    BOOST_CHECK_LE(per_key, 256.0 + key_avg);
}

// ---- correctness properties (design.md) -------------------------------------

namespace {

/// A random workload over a small key space: uploads of various sizes, some
/// abandoned before the commit, RESP sets, deletes, sweeps and fillers.
auto random_log(std::mt19937& rng, std::size_t length) -> std::vector<std::vector<std::byte>> {
    std::vector<std::vector<std::byte>> log;
    std::uniform_int_distribution<int> op(0, 9);
    std::uniform_int_distribution<int> key(0, 11);
    std::uniform_int_distribution<std::uint32_t> seed(0, 40);  // repeats => dedupe
    std::uniform_int_distribution<std::size_t> size(0, 3000);
    while (log.size() < length) {
        const auto k = "key/" + std::to_string(key(rng));
        switch (op(rng)) {
            case 0:
            case 1:
            case 2: {
                auto es = upload(k, pattern(size(rng), seed(rng)), 1000, rng() % 3 == 0 ? 50 : 0);
                log.insert(log.end(), es.begin(), es.end());
                break;
            }
            case 3: {
                auto es = upload(k, pattern(size(rng), seed(rng)), 700);
                es.pop_back();  // abandoned
                log.insert(log.end(), es.begin(), es.end());
                break;
            }
            case 4:
            case 5:
                log.push_back(encode_redis_kv_command(
                    redis_kv_set_command{k, pattern(size(rng) / 4, seed(rng)), 0}));
                break;
            case 6:
                log.push_back(encode_redis_kv_command(redis_kv_del_command{k}));
                break;
            case 7:
                log.push_back(encode_redis_kv_command(redis_kv_sweep_command{{{k, 50}}}));
                break;
            default:
                log.push_back(filler());
                break;
        }
    }
    return log;
}

}  // namespace

/// Property 1: two replicas that apply one log end equal, whatever chunks
/// either had on disk beforehand.
BOOST_AUTO_TEST_CASE(property_1_determinism, *boost::unit_test::timeout(300)) {
    std::mt19937 rng(20261101);
    for (int trial = 0; trial < 20; ++trial) {
        const auto log = random_log(rng, 120);
        node_fixture a;
        node_fixture b;
        // b's disk already holds a few of the chunks the log will write, and
        // one unrelated chunk.
        for (std::uint32_t s = 0; s < 40; s += 7) {
            const auto v = pattern(500, s);
            b._store->put(sha256_of(v), v);
        }
        auto sa = a.make_sm(30);
        auto sb = b.make_sm(30);
        std::uint64_t ia = 0;
        std::uint64_t ib = 0;
        apply_all(sa, ia, log);
        apply_all(sb, ib, log);
        BOOST_REQUIRE_MESSAGE(sa.get_state() == sb.get_state(), "trial " << trial);
    }
}

/// Property 2: any key read is its whole committed value, hash-correct; and
/// it is the value of the last commit or set for it.
BOOST_AUTO_TEST_CASE(property_2_atomic_visibility, *boost::unit_test::timeout(300)) {
    std::mt19937 rng(7);
    for (int trial = 0; trial < 10; ++trial) {
        node_fixture node;
        auto sm = node.make_sm(25);
        std::uint64_t idx = 0;
        std::map<std::string, std::vector<std::byte>> model;
        std::uniform_int_distribution<int> key(0, 5);
        for (int step = 0; step < 60; ++step) {
            const auto k = "k" + std::to_string(key(rng));
            const auto v = pattern(rng() % 2500, static_cast<std::uint32_t>(rng() % 30));
            auto es = upload(k, v, 600);
            for (std::size_t i = 0; i < es.size(); ++i) {
                apply_one(sm, idx, es[i]);
                // Mid-upload, a reader sees the previous value or nothing.
                const auto got = value_of(node, sm, k);
                if (i + 1 < es.size()) {
                    auto it = model.find(k);
                    BOOST_REQUIRE(got ==
                                  (it == model.end() ? std::nullopt : std::optional(it->second)));
                }
            }
            model[k] = v;
            if (rng() % 5 == 0) {
                apply_one(sm, idx, encode_redis_kv_command(redis_kv_del_command{k}));
                model.erase(k);
            }
            for (const auto& [mk, mv] : model) {
                BOOST_REQUIRE_MESSAGE(value_of(node, sm, mk) == mv,
                                      "trial " << trial << " key " << mk);
            }
        }
    }
}

/// Property 3: after the grace period, every chunk on disk is referenced by
/// a committed manifest or a pending entry, and every referenced chunk is on
/// disk.
BOOST_AUTO_TEST_CASE(property_3_no_unreferenced_growth, *boost::unit_test::timeout(300)) {
    std::mt19937 rng(99);
    for (int trial = 0; trial < 20; ++trial) {
        node_fixture node;
        auto sm = node.make_sm(15);
        std::uint64_t idx = 0;
        apply_all(sm, idx, random_log(rng, 150));
        for (int i = 0; i < 16; ++i) {
            apply_one(sm, idx, filler());
        }
        BOOST_CHECK_EQUAL(sm.pending_chunk_count(), 0U);
        std::set<chunk_sha> referenced;
        sm.for_each_key([&](const std::string&, const cache_manifest& m) {
            referenced.insert(m.chunks().begin(), m.chunks().end());
        });
        std::set<chunk_sha> on_disk;
        node._store->for_each_chunk(
            [&](const chunk_sha& sha, std::uint64_t) { on_disk.insert(sha); });
        BOOST_REQUIRE_MESSAGE(on_disk == referenced, "trial " << trial << ": " << on_disk.size()
                                                              << " on disk, " << referenced.size()
                                                              << " referenced");
        BOOST_CHECK(sm.missing_chunks().empty());
    }
}

/// Property 4: split at any cuts, restore the leftmost child, absorb the
/// rest in order, and `get_state()` is byte-identical; each child holds only
/// its own keys and the whole pending table.
BOOST_AUTO_TEST_CASE(property_4_split_absorb_law, *boost::unit_test::timeout(300)) {
    std::mt19937 rng(4242);
    for (int trial = 0; trial < 30; ++trial) {
        node_fixture node;
        auto sm = node.make_sm(500);
        std::uint64_t idx = 0;
        apply_all(sm, idx, random_log(rng, 100));
        const auto original = sm.get_state();
        std::vector<std::string> cuts;
        for (int i = 0, n = static_cast<int>(rng() % 4); i < n; ++i) {
            cuts.push_back("key/" + std::to_string(rng() % 14));
        }
        const auto blobs = sm.split_state(cuts);

        auto merged = node.make_sm(500);
        merged.restore_from_snapshot(blobs.at(0), idx);
        for (std::size_t i = 1; i < blobs.size(); ++i) {
            merged.absorb(blobs[i], shard_range<std::string>{});
        }
        BOOST_REQUIRE_MESSAGE(merged.get_state() == original, "trial " << trial);
        BOOST_CHECK_EQUAL(merged.approximate_size_bytes(), sm.approximate_size_bytes());
        BOOST_CHECK_EQUAL(merged.pending_chunk_count(), sm.pending_chunk_count());

        // Each child's keys lie in its range.
        auto sorted = cuts;
        std::sort(sorted.begin(), sorted.end());
        sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
        for (std::size_t i = 0; i < blobs.size(); ++i) {
            auto child = node.make_sm(500);
            child.restore_from_snapshot(blobs[i], idx);
            BOOST_CHECK_EQUAL(child.pending_chunk_count(), sm.pending_chunk_count());
            child.for_each_key([&](const std::string& k, const cache_manifest&) {
                if (i > 0) {
                    BOOST_CHECK(k >= sorted[i - 1]);
                }
                if (i < sorted.size()) {
                    BOOST_CHECK(k < sorted[i]);
                }
            });
        }
        // Restoring and absorbing on one node moved references around; the
        // original replica still reads every key.
        sm.for_each_key([&](const std::string& k, const cache_manifest& m) {
            BOOST_CHECK_MESSAGE(read_cache_value(*node._store, m).has_value(), "lost " << k);
        });
    }
}
