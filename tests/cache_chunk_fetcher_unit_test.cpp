// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file cache_chunk_fetcher_unit_test.cpp
/// @brief `cache_chunk_fetcher` (.kiro/specs/disk-backed-build-cache/ task 5;
///        Requirements 4.3, 4.5, 4.6 and 10.1), with two or three in-process
///        nodes whose stores the fetch callback reads directly.

#define BOOST_TEST_MODULE cache_chunk_fetcher_unit_test
#include <boost/test/unit_test.hpp>

#include <raft/cache_chunk_fetcher.hpp>
#include <raft/cache_kv_state_machine.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <unistd.h>

BOOST_TEST_DONT_PRINT_LOG_VALUE(std::vector<std::byte>)

namespace fs = std::filesystem;
using namespace kythira;
using namespace std::chrono_literals;

namespace {

using sm_type = cache_kv_state_machine<std::uint64_t>;
using fetcher_type = cache_chunk_fetcher<int, std::uint64_t>;

/// One node: a data directory, its chunk store and node-wide counts.
struct node_fixture {
    fs::path _dir;
    std::unique_ptr<cache_chunk_store> _store;
    std::shared_ptr<chunk_refcounts> _refs;

    node_fixture() {
        static std::atomic<int> counter{0};
        _dir = fs::temp_directory_path() / ("kythira_fetcher_" + std::to_string(::getpid()) + "_" +
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
    return out;
}

/// One upload of `value` as log entries: a `put_chunk` per `chunk` bytes,
/// then the `commit`. `shas` receives the chunk names.
auto upload(const std::string& key, const std::vector<std::byte>& value, std::size_t chunk,
            std::vector<chunk_sha>* shas = nullptr) -> std::vector<std::vector<std::byte>> {
    std::vector<std::vector<std::byte>> out;
    redis_kv_commit_command c{key, 0, value.size(), sha256_of(value), {}};
    for (std::size_t off = 0; off < value.size(); off += chunk) {
        const std::vector<std::byte> piece(
            value.begin() + static_cast<std::ptrdiff_t>(off),
            value.begin() + static_cast<std::ptrdiff_t>(std::min(value.size(), off + chunk)));
        const auto sha = sha256_of(piece);
        out.push_back(encode_redis_kv_command(redis_kv_put_chunk_command{key, sha, piece}));
        c._chunks.push_back(sha);
        if (shas != nullptr) {
            shas->push_back(sha);
        }
    }
    out.push_back(encode_redis_kv_command(c));
    return out;
}

void apply_all(sm_type& sm, std::uint64_t& index, const std::vector<std::vector<std::byte>>& es) {
    for (const auto& e : es) {
        (void)sm.apply(e, ++index);
    }
}

/// A replica with its node lock, as `multi_raft` would hold it, and a
/// fetcher whose `want` the state machine feeds.
struct replica {
    node_fixture& _node;
    std::mutex _lock;
    std::unique_ptr<fetcher_type> _fetcher;
    std::optional<sm_type> _sm;
    std::atomic<int> _refreshes{0};

    replica(node_fixture& node, fetcher_type::peers_fn peers, fetcher_type::fetch_fn fetch,
            cache_chunk_fetcher_options options = {})
        : _node(node) {
        _fetcher = std::make_unique<fetcher_type>(
            node._refs, std::move(peers), std::move(fetch),
            [this](const std::uint64_t&) {
                const std::lock_guard lock(_lock);
                _sm->refresh_has_all();
                ++_refreshes;
            },
            options);
        cache_kv_options o;
        o._on_missing_chunks = [this](std::vector<chunk_sha> shas) { _fetcher->want(1, shas); };
        _sm.emplace(node._refs, std::move(o));
    }
    ~replica() { _fetcher->stop(); }
    replica(const replica&) = delete;
    auto operator=(const replica&) -> replica& = delete;
    replica(replica&&) = delete;
    auto operator=(replica&&) -> replica& = delete;

    void restore(const std::vector<std::byte>& state, std::uint64_t index) {
        const std::lock_guard lock(_lock);
        _sm->restore_from_snapshot(state, index);
    }
    [[nodiscard]] auto servable(const std::string& key) -> bool {
        const std::lock_guard lock(_lock);
        auto m = _sm->lookup(key);
        return m && m->has_all();
    }
    [[nodiscard]] auto value(const std::string& key) -> std::optional<std::vector<std::byte>> {
        cache_manifest_ptr m;
        {
            const std::lock_guard lock(_lock);
            m = _sm->lookup(key);
        }
        if (!m) {
            return std::nullopt;
        }
        return read_cache_value(*_node._store, *m);
    }
};

/// A fetch callback reading from the given nodes' stores, by peer number.
auto read_from(std::map<int, node_fixture*> peers) -> fetcher_type::fetch_fn {
    return [peers = std::move(peers)](
               const int& peer, const chunk_sha& sha) -> std::optional<std::vector<std::byte>> {
        auto it = peers.find(peer);
        if (it == peers.end()) {
            return std::nullopt;
        }
        return it->second->_store->read(sha);
    };
}

}  // namespace

BOOST_AUTO_TEST_SUITE(cache_chunk_fetcher_tests)

// Task 5's acceptance: a manifest restored onto an empty store is
// unservable, the fetcher brings its chunks from a peer, and the key
// becomes servable here (Requirements 4.3 and 4.4).
BOOST_AUTO_TEST_CASE(restore_on_an_empty_store_fetches_and_becomes_servable,
                     *boost::unit_test::timeout(60)) {
    node_fixture a;
    node_fixture b;
    sm_type source(a._refs);
    std::uint64_t idx = 0;
    const auto big = pattern(10000, 1);
    const auto small = pattern(100, 2);
    apply_all(source, idx, upload("vcpkg/big", big, 4096));
    apply_all(source, idx, upload("sccache/small", small, 4096));

    replica r(b, [](const std::uint64_t&) { return std::vector<int>{1}; }, read_from({{1, &a}}));
    r.restore(source.get_state(), idx);
    BOOST_CHECK(!r.servable("vcpkg/big"));
    BOOST_CHECK(!r.servable("sccache/small"));
    BOOST_CHECK_EQUAL(r._fetcher->pending(), 4U);  // three chunks of big, one of small
    BOOST_CHECK(!r.value("vcpkg/big"));

    r._fetcher->start();
    BOOST_REQUIRE(r._fetcher->wait_idle(10s));
    BOOST_CHECK(r.servable("vcpkg/big"));
    BOOST_CHECK(r.servable("sccache/small"));
    BOOST_CHECK(r.value("vcpkg/big") == big);
    BOOST_CHECK(r.value("sccache/small") == small);
    BOOST_CHECK_EQUAL(r._fetcher->stats()._fetched.load(), 4U);
    BOOST_CHECK_EQUAL(r._fetcher->stats()._fetched_bytes.load(), 10100U);
    BOOST_CHECK_GE(r._refreshes.load(), 1);
    BOOST_CHECK_EQUAL(r._fetcher->outstanding(1), 0U);
}

// Peers are asked in the order given (leader first), and a peer that is down
// or sends the wrong bytes is skipped (Requirement 4.5).
BOOST_AUTO_TEST_CASE(peers_in_order_and_bad_bytes_are_skipped, *boost::unit_test::timeout(60)) {
    node_fixture good;
    node_fixture b;
    sm_type source(good._refs);
    std::uint64_t idx = 0;
    const auto v = pattern(5000, 3);
    apply_all(source, idx, upload("k", v, 8192));

    std::vector<int> asked;
    std::mutex asked_mu;
    auto fetch = [&](const int& peer,
                     const chunk_sha& sha) -> std::optional<std::vector<std::byte>> {
        {
            const std::lock_guard lock(asked_mu);
            asked.push_back(peer);
        }
        if (peer == 7) {
            return std::nullopt;  // down
        }
        if (peer == 8) {
            return pattern(5000, 99);  // lying
        }
        return good._store->read(sha);
    };
    replica r(b, [](const std::uint64_t&) { return std::vector<int>{7, 8, 9}; }, fetch);
    r.restore(source.get_state(), idx);
    BOOST_REQUIRE(r._fetcher->run_once());
    BOOST_CHECK(r.value("k") == v);
    BOOST_CHECK((asked == std::vector<int>{7, 8, 9}));
    BOOST_CHECK_EQUAL(r._fetcher->stats()._peer_misses.load(), 1U);
    BOOST_CHECK_EQUAL(r._fetcher->stats()._verify_failures.load(), 1U);
    BOOST_CHECK_EQUAL(r._fetcher->stats()._fetched.load(), 1U);
    BOOST_CHECK(r.servable("k"));
}

// A chunk nobody supplies is retried with a growing delay, then fetched once
// a peer has it.
BOOST_AUTO_TEST_CASE(a_chunk_no_peer_has_is_retried_with_backoff, *boost::unit_test::timeout(60)) {
    node_fixture a;
    node_fixture b;
    sm_type source(a._refs);
    std::uint64_t idx = 0;
    const auto v = pattern(3000, 4);
    apply_all(source, idx, upload("k", v, 8192));

    std::atomic<int> calls{0};
    auto fetch = [&](const int&, const chunk_sha& sha) -> std::optional<std::vector<std::byte>> {
        if (calls.fetch_add(1) < 3) {
            return std::nullopt;
        }
        return a._store->read(sha);
    };
    cache_chunk_fetcher_options o;
    o._retry_delay = 20ms;
    replica r(b, [](const std::uint64_t&) { return std::vector<int>{1}; }, fetch, o);
    r.restore(source.get_state(), idx);
    const auto started = std::chrono::steady_clock::now();
    r._fetcher->start();
    BOOST_REQUIRE(r._fetcher->wait_idle(10s));
    const auto took = std::chrono::steady_clock::now() - started;
    BOOST_CHECK_EQUAL(r._fetcher->stats()._retries.load(), 3U);
    // 20 + 40 + 80 ms of backoff.
    BOOST_CHECK_GE(took, 140ms);
    BOOST_CHECK(r.value("k") == v);
}

// A key deleted while its chunk is queued leaves nothing behind: the chunk is
// dropped, not stored as an orphan.
BOOST_AUTO_TEST_CASE(an_unreferenced_chunk_is_dropped_not_stored, *boost::unit_test::timeout(60)) {
    node_fixture a;
    node_fixture b;
    // A one-entry grace, run out before the snapshot, so the snapshot's only
    // reference to the chunk is the key's.
    cache_kv_options short_grace;
    short_grace._orphan_grace_entries = 1;
    sm_type source(a._refs, short_grace);
    std::uint64_t idx = 0;
    std::vector<chunk_sha> shas;
    apply_all(source, idx, upload("k", pattern(3000, 5), 8192, &shas));
    apply_all(source, idx,
              {encode_redis_kv_command(redis_kv_del_command{"\x01none"}),
               encode_redis_kv_command(redis_kv_del_command{"\x01none"})});
    BOOST_REQUIRE_EQUAL(source.pending_chunk_count(), 0U);

    replica r(b, [](const std::uint64_t&) { return std::vector<int>{1}; }, read_from({{1, &a}}));
    r.restore(source.get_state(), idx);
    {
        const std::lock_guard lock(r._lock);
        (void)r._sm->apply(encode_redis_kv_command(redis_kv_del_command{"k"}), ++idx);
    }
    BOOST_REQUIRE(r._fetcher->run_once());
    BOOST_CHECK_EQUAL(r._fetcher->stats()._dropped_unreferenced.load(), 1U);
    BOOST_CHECK_EQUAL(r._fetcher->stats()._fetched.load(), 0U);
    BOOST_CHECK(!b._store->contains(shas.at(0)));
    BOOST_CHECK_EQUAL(r._fetcher->pending(), 0U);
}

// A chunk two shards want is fetched once, and both shards are refreshed.
BOOST_AUTO_TEST_CASE(a_chunk_two_shards_want_is_fetched_once, *boost::unit_test::timeout(60)) {
    node_fixture a;
    node_fixture b;
    sm_type source(a._refs);
    std::uint64_t idx = 0;
    std::vector<chunk_sha> shas;
    apply_all(source, idx, upload("k", pattern(3000, 6), 8192, &shas));
    sm_type holder(b._refs);  // keeps the chunk referenced on b
    {
        std::uint64_t i = 0;
        holder.restore_from_snapshot(source.get_state(), ++i);
    }

    std::atomic<int> fetches{0};
    std::vector<std::uint64_t> refreshed;
    std::mutex mu;
    fetcher_type f(
        b._refs, [](const std::uint64_t&) { return std::vector<int>{1}; },
        [&](const int&, const chunk_sha& sha) {
            ++fetches;
            return a._store->read(sha);
        },
        [&](const std::uint64_t& g) {
            const std::lock_guard lock(mu);
            refreshed.push_back(g);
        });
    f.want(1, shas);
    f.want(2, shas);
    f.want(2, shas);  // twice from one shard counts once
    BOOST_CHECK_EQUAL(f.pending(), 1U);
    BOOST_CHECK_EQUAL(f.outstanding(2), 1U);
    BOOST_REQUIRE(f.run_once());
    BOOST_CHECK(!f.run_once());
    BOOST_CHECK_EQUAL(fetches.load(), 1);
    std::sort(refreshed.begin(), refreshed.end());
    BOOST_CHECK((refreshed == std::vector<std::uint64_t>{1, 2}));
}

// The token bucket holds the fetch rate to its configured bytes per second
// (Requirement 4.6).
BOOST_AUTO_TEST_CASE(the_token_bucket_caps_the_fetch_rate, *boost::unit_test::timeout(60)) {
    node_fixture a;
    node_fixture b;
    sm_type source(a._refs);
    std::uint64_t idx = 0;
    // 16 chunks of 64 KiB: 1 MiB.
    const auto v = pattern(std::size_t{1} << 20U, 7);
    apply_all(source, idx, upload("k", v, std::size_t{64} * 1024));

    cache_chunk_fetcher_options o;
    o._bytes_per_sec = std::uint64_t{4} * 1024 * 1024;  // 4 MiB/s
    o._burst_bytes = std::uint64_t{64} * 1024;
    o._workers = 4;
    replica r(b, [](const std::uint64_t&) { return std::vector<int>{1}; }, read_from({{1, &a}}), o);
    r.restore(source.get_state(), idx);
    const auto started = std::chrono::steady_clock::now();
    r._fetcher->start();
    BOOST_REQUIRE(r._fetcher->wait_idle(20s));
    const auto took = std::chrono::steady_clock::now() - started;
    // 1 MiB at 4 MiB/s with a 64 KiB burst: at least ~234 ms. Workers
    // check the bucket before a fetch and charge after, so up to one chunk
    // per worker can overshoot; allow for it.
    BOOST_CHECK_GE(took, 150ms);
    BOOST_CHECK(r.value("k") == v);
}

// The bucket on its own: idle it allows the burst, then charges become debt
// that must be waited out at the configured rate.
BOOST_AUTO_TEST_CASE(token_bucket_arithmetic, *boost::unit_test::timeout(10)) {
    cache_token_bucket unlimited(0, 0);
    unlimited.charge(1U << 30U);
    BOOST_CHECK(unlimited.wait_time() == 0ns);

    cache_token_bucket b(1'000'000, 100'000);
    BOOST_CHECK(b.wait_time() == 0ns);
    b.charge(100'000);
    BOOST_CHECK(b.wait_time() <= 1ms);  // spent exactly the burst
    b.charge(500'000);
    const auto wait = b.wait_time();
    BOOST_CHECK_GE(wait, 450ms);
    BOOST_CHECK_LE(wait, 500ms);
}

// A `commit` binding pending chunks that came in a snapshot without their
// bytes reports them to the fetcher, as a restore does.
BOOST_AUTO_TEST_CASE(a_commit_over_snapshot_pending_chunks_wants_them,
                     *boost::unit_test::timeout(60)) {
    node_fixture a;
    node_fixture b;
    sm_type source(a._refs);
    std::uint64_t idx = 0;
    const auto v = pattern(9000, 8);
    auto entries = upload("k", v, 4096);
    const auto commit = entries.back();
    entries.pop_back();
    apply_all(source, idx, entries);  // chunks pending, no commit yet

    replica r(b, [](const std::uint64_t&) { return std::vector<int>{1}; }, read_from({{1, &a}}));
    r.restore(source.get_state(), idx);
    BOOST_CHECK_EQUAL(r._fetcher->pending(), 3U);  // the pending chunks themselves
    while (r._fetcher->run_once()) {
    }
    BOOST_CHECK_EQUAL(r._fetcher->pending(), 0U);
    // Lose one again, as a crash before fsync would, then commit.
    std::vector<chunk_sha> shas;
    (void)upload("k", v, 4096, &shas);
    BOOST_REQUIRE(b._store->remove(shas.at(1)));
    {
        const std::lock_guard lock(r._lock);
        BOOST_CHECK((r._sm->apply(commit, ++idx) == std::vector<std::byte>{std::byte{1}}));
    }
    BOOST_CHECK(!r.servable("k"));
    BOOST_CHECK_EQUAL(r._fetcher->pending(), 1U);
    BOOST_REQUIRE(r._fetcher->run_once());
    BOOST_CHECK(r.servable("k"));
    BOOST_CHECK(r.value("k") == v);
}

BOOST_AUTO_TEST_SUITE_END()
