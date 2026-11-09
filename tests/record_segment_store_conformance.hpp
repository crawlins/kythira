// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file record_segment_store_conformance.hpp
/// @brief One test body for `record_segment_store`, instantiated per user
///        (.kiro/specs/kinesis-compatible-streams/ task 3, Requirement 11.1).
///
/// Kinesis instantiates it with shard-shaped streams and record-sized
/// payloads; DynamoDB's change log (.kiro/specs/dynamodb-compatible-api/
/// task 15) instantiates the same cases with its own keys and mutation-sized
/// payloads, so both users are held to one contract: ordered reads,
/// deterministic rolls and digests across replicas, durable barriers,
/// restart truncation, verified peer fetch, seals and trims.
///
/// ## Instantiating it
///
/// ```cpp
/// KYTHIRA_RECORD_SEGMENT_CONFORMANCE(my_harness, my_prefix)
/// ```
///
/// ### The harness contract
///
/// | Member | Meaning |
/// |---|---|
/// | `stream(i)` | the i-th distinct stream key the user would create |
/// | `payload(seq)` | the bytes the user would append as record `seq` (deterministic) |
/// | `tweak(options)` | adjust store options; must keep segments small enough to roll |
///
/// A harness is default-constructed once per case.

#include <raft/record_segment_store.hpp>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <ostream>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace kythira {

/// For Boost.Test failure messages.
inline auto operator<<(std::ostream& os, const record_segment_descriptor& d) -> std::ostream& {
    return os << "{stream=" << d._stream.size() << "B first=" << d._first_seq
              << " last=" << d._last_seq << " records=" << d._records << " bytes=" << d._bytes
              << " sealed=" << d._sealed << " sha0=" << static_cast<int>(d._sha256[0]) << "}";
}

}  // namespace kythira

// NOLINTNEXTLINE(cert-dcl58-cpp)
BOOST_TEST_DONT_PRINT_LOG_VALUE(std::optional<std::uint64_t>)

namespace kythira::segment_conformance {

/// A scratch directory removed when the case ends.
class scratch_dir {
public:
    scratch_dir() {
        std::random_device rd;
        _path = std::filesystem::temp_directory_path() /
                ("kythira-segments-" + std::to_string(rd()) + "-" + std::to_string(rd()));
        std::filesystem::create_directories(_path);
    }
    scratch_dir(const scratch_dir&) = delete;
    auto operator=(const scratch_dir&) -> scratch_dir& = delete;
    scratch_dir(scratch_dir&&) = delete;
    auto operator=(scratch_dir&&) -> scratch_dir& = delete;
    ~scratch_dir() {
        std::error_code ec;
        std::filesystem::remove_all(_path, ec);
    }
    [[nodiscard]] auto path() const -> const std::filesystem::path& { return _path; }
    [[nodiscard]] auto sub(const std::string& name) const -> std::filesystem::path {
        return _path / name;
    }

private:
    std::filesystem::path _path;
};

template<typename Harness>
auto options_for(Harness& h, const std::filesystem::path& dir) -> record_segment_store_options {
    record_segment_store_options o;
    o._data_dir = dir;
    o._max_segment_bytes = 64 * 1024;
    o._max_segment_age_ms = 0;
    o._index_interval_bytes = 1024;
    h.tweak(o);
    return o;
}

/// The timestamp the cases give record `seq`: one second apart.
inline auto ts_of(std::uint64_t seq) -> std::int64_t {
    return 1'700'000'000'000 + static_cast<std::int64_t>(seq) * 1000;
}

/// Append records `[from, to]` (every `step`-th) in batches of `batch`.
template<typename Harness>
auto append_range(Harness& h, record_segment_store& store, const std::string& stream,
                  std::uint64_t from, std::uint64_t to, std::uint64_t step = 1,
                  std::size_t batch = 7) -> std::vector<record_segment_descriptor> {
    std::vector<record_segment_descriptor> sealed;
    std::vector<std::vector<std::byte>> payloads;
    std::vector<segment_record_view> views;
    auto flush = [&] {
        auto r = store.append(stream, views);
        sealed.insert(sealed.end(), r._sealed.begin(), r._sealed.end());
        views.clear();
        payloads.clear();
    };
    for (std::uint64_t seq = from; seq <= to; seq += step) {
        payloads.reserve(batch);
        payloads.push_back(h.payload(seq));
        views.push_back({seq, ts_of(seq), payloads.back()});
        if (views.size() == batch) {
            flush();
        }
    }
    if (!views.empty()) {
        flush();
    }
    return sealed;
}

/// Every record from `from` to the end, through `read`.
inline auto read_all(const record_segment_store& store, const std::string& stream,
                     std::uint64_t from = 0) -> std::vector<segment_record> {
    std::vector<segment_record> out;
    std::uint64_t next = from;
    for (;;) {
        auto r = store.read(stream, next, {._max_records = 50, ._max_bytes = 256 * 1024});
        if (r._records.empty()) {
            return out;
        }
        next = r._next_seq;
        out.insert(out.end(), std::make_move_iterator(r._records.begin()),
                   std::make_move_iterator(r._records.end()));
    }
}

template<typename Harness>
void check_records(Harness& h, const std::vector<segment_record>& got, std::uint64_t from,
                   std::uint64_t to, std::uint64_t step = 1) {
    std::size_t i = 0;
    for (std::uint64_t seq = from; seq <= to; seq += step, ++i) {
        BOOST_REQUIRE_LT(i, got.size());
        BOOST_REQUIRE_EQUAL(got[i]._seq, seq);
        BOOST_REQUIRE_EQUAL(got[i]._timestamp_ms, ts_of(seq));
        BOOST_REQUIRE(got[i]._data == h.payload(seq));
    }
    BOOST_REQUIRE_EQUAL(got.size(), i);
}

inline auto file_bytes(const std::filesystem::path& p) -> std::vector<std::byte> {
    std::ifstream in(p, std::ios::binary);
    std::vector<char> c{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    std::vector<std::byte> out(c.size());
    std::memcpy(out.data(), c.data(), c.size());
    return out;
}

/// The one segment file of `stream` whose name ends in `ext`, newest last.
inline auto segment_files(const std::filesystem::path& data_dir, const std::string& stream,
                          const std::string& ext) -> std::vector<std::filesystem::path> {
    std::vector<std::filesystem::path> out;
    const auto dir = data_dir / "streams" / detail::segments::dir_name_for(stream);
    if (!std::filesystem::exists(dir)) {
        return out;
    }
    for (const auto& e : std::filesystem::directory_iterator(dir)) {
        if (e.path().extension() == ext) {
            out.push_back(e.path());
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

/// A transport that reads straight from another store, as the daemon's
/// `GET` handler would.
inline auto transport_from(const std::vector<const record_segment_store*>& peers)
    -> record_segment_store::fetch_transport {
    return [peers](const std::string& peer, const std::string& stream, std::uint64_t first,
                   std::uint64_t length) -> std::optional<std::vector<std::byte>> {
        const auto i = static_cast<std::size_t>(std::stoul(peer));
        if (i >= peers.size() || peers[i] == nullptr) {
            return std::nullopt;
        }
        return peers[i]->read_segment_bytes(stream, first, length);
    };
}

template<typename Harness> struct cases {
    static void appends_read_back_in_order_per_stream() {
        Harness h;
        scratch_dir d;
        record_segment_store store(options_for(h, d.path()));
        append_range(h, store, h.stream(0), 1, 120);
        append_range(h, store, h.stream(1), 1000, 1400, 4);
        check_records(h, read_all(store, h.stream(0)), 1, 120);
        check_records(h, read_all(store, h.stream(1)), 1000, 1400, 4);
        check_records(h, read_all(store, h.stream(1), 1001), 1004, 1400, 4);
        BOOST_TEST(read_all(store, h.stream(2)).empty());
        BOOST_TEST(store.last_seq(h.stream(0)) == std::optional<std::uint64_t>(120));
        BOOST_TEST(!store.last_seq(h.stream(2)).has_value());
        BOOST_TEST(store.streams().size() == 2U);
    }

    static void sequences_must_increase_but_may_skip() {
        Harness h;
        scratch_dir d;
        record_segment_store store(options_for(h, d.path()));
        append_range(h, store, h.stream(0), 10, 10);
        const auto p = h.payload(10);
        BOOST_CHECK_THROW(store.append(h.stream(0), segment_record_view{10, ts_of(10), p}),
                          std::invalid_argument);
        BOOST_CHECK_THROW(store.append(h.stream(0), segment_record_view{9, ts_of(9), p}),
                          std::invalid_argument);
        // A batch that goes backwards is refused whole.
        std::vector<segment_record_view> batch{{20, ts_of(20), p}, {15, ts_of(15), p}};
        BOOST_CHECK_THROW(store.append(h.stream(0), batch), std::invalid_argument);
        BOOST_TEST(store.last_seq(h.stream(0)) == std::optional<std::uint64_t>(10));
        BOOST_CHECK_NO_THROW(store.append(h.stream(0), segment_record_view{1'000'000, 0, p}));
        BOOST_CHECK_THROW(store.append("", segment_record_view{1, 0, p}), std::invalid_argument);
    }

    static void replicas_roll_identically_and_agree_on_digests() {
        Harness h;
        scratch_dir d;
        record_segment_store a(options_for(h, d.sub("a")));
        record_segment_store b(options_for(h, d.sub("b")));
        // Different batching on each replica: rolls depend on records only.
        auto sa = append_range(h, a, h.stream(0), 1, 300, 1, 1);
        auto sb = append_range(h, b, h.stream(0), 1, 300, 1, 13);
        BOOST_REQUIRE_GE(sa.size(), 2U);
        BOOST_TEST(sa == sb);
        BOOST_TEST(a.local_segments(h.stream(0)) == b.local_segments(h.stream(0)));
        for (const auto& s : a.local_segments(h.stream(0))) {
            BOOST_TEST(s._bytes <= a.options()._max_segment_bytes);
            const auto bytes = a.read_segment_bytes(s._stream, s._first_seq, s._bytes);
            BOOST_REQUIRE(bytes.has_value());
            BOOST_TEST(segment_sha256_of(*bytes) == s._sha256);
        }
        const auto fa = segment_files(d.sub("a"), h.stream(0), ".seg");
        const auto fb = segment_files(d.sub("b"), h.stream(0), ".seg");
        BOOST_REQUIRE_EQUAL(fa.size(), fb.size());
        for (std::size_t i = 0; i < fa.size(); ++i) {
            BOOST_TEST(file_bytes(fa[i]) == file_bytes(fb[i]));
        }
    }

    static void rolls_on_record_age() {
        Harness h;
        scratch_dir d;
        auto o = options_for(h, d.path());
        o._max_segment_bytes = std::uint64_t{1} << 30U;
        o._max_segment_age_ms = 10'000;  // ten records one second apart
        record_segment_store store(o);
        auto sealed = append_range(h, store, h.stream(0), 1, 35);
        BOOST_REQUIRE_EQUAL(sealed.size(), 3U);
        BOOST_TEST(sealed[0]._first_seq == 1U);
        BOOST_TEST(sealed[0]._last_seq == 10U);
        BOOST_TEST(sealed[1]._first_seq == 11U);
        BOOST_TEST(sealed[2]._last_seq == 30U);
        for (const auto& s : sealed) {
            BOOST_TEST(s._sealed);
            BOOST_TEST(s._last_timestamp_ms - s._first_timestamp_ms < 10'000);
        }
        check_records(h, read_all(store, h.stream(0)), 1, 35);
    }

    static void reads_honour_limits_and_always_progress() {
        Harness h;
        scratch_dir d;
        record_segment_store store(options_for(h, d.path()));
        append_range(h, store, h.stream(0), 1, 60);
        auto r = store.read(h.stream(0), 1, {._max_records = 5, ._max_bytes = 1U << 30U});
        BOOST_TEST(r._records.size() == 5U);
        BOOST_TEST(r._next_seq == 6U);
        r = store.read(h.stream(0), 6, {._max_records = 100, ._max_bytes = 1});
        BOOST_REQUIRE_EQUAL(r._records.size(), 1U);
        BOOST_TEST(r._records[0]._seq == 6U);
        BOOST_TEST(r._next_seq == 7U);
        r = store.read(h.stream(0), 61);
        BOOST_TEST(r._records.empty());
        BOOST_TEST(r._next_seq == 61U);
    }

    static void read_segment_walks_one_segment_at_a_time() {
        Harness h;
        scratch_dir d;
        record_segment_store store(options_for(h, d.path()));
        append_range(h, store, h.stream(0), 1, 200);
        const auto segs = store.local_segments(h.stream(0));
        BOOST_REQUIRE_GE(segs.size(), 3U);
        std::vector<segment_record> got;
        std::uint64_t next = 1;
        for (const auto& s : segs) {
            for (;;) {
                auto r = store.read_segment(h.stream(0), s._first_seq, next,
                                            {._max_records = 3, ._max_bytes = 1U << 30U});
                BOOST_REQUIRE(r.has_value());
                for (auto& rec : r->_records) {
                    BOOST_TEST(rec._seq >= s._first_seq);
                    BOOST_TEST(rec._seq <= s._last_seq);
                    got.push_back(std::move(rec));
                }
                next = r->_next_seq;
                if (r->_segment_exhausted || r->_records.empty()) {
                    break;
                }
            }
            if (s._sealed) {
                BOOST_TEST(next == s._last_seq + 1);
            }
        }
        check_records(h, got, 1, 200);
        // Seeking into the middle of a sealed segment uses the index.
        auto mid = store.read_segment(h.stream(0), segs[1]._first_seq, segs[1]._first_seq + 2);
        BOOST_REQUIRE(mid.has_value());
        BOOST_REQUIRE(!mid->_records.empty());
        BOOST_TEST(mid->_records.front()._seq == segs[1]._first_seq + 2);
        BOOST_TEST(!store.read_segment(h.stream(0), 999'999, 0).has_value());
    }

    static void a_restart_keeps_barriered_records_and_resumes_the_active_segment() {
        Harness h;
        scratch_dir d;
        std::vector<record_segment_descriptor> before;
        {
            record_segment_store store(options_for(h, d.sub("a")));
            append_range(h, store, h.stream(0), 1, 150);
            store.barrier();
            before = store.local_segments(h.stream(0));
        }
        record_segment_store store(options_for(h, d.sub("a")));
        BOOST_TEST(store.local_segments(h.stream(0)) == before);
        BOOST_TEST(store.stats()._torn_tails_truncated == 0U);
        append_range(h, store, h.stream(0), 151, 240);
        check_records(h, read_all(store, h.stream(0)), 1, 240);
        // Same as a replica that never restarted.
        record_segment_store ref(options_for(h, d.sub("ref")));
        append_range(h, ref, h.stream(0), 1, 240);
        BOOST_TEST(store.local_segments(h.stream(0)) == ref.local_segments(h.stream(0)));
    }

    static void an_open_truncates_a_torn_tail() {
        Harness h;
        scratch_dir d;
        {
            record_segment_store store(options_for(h, d.path()));
            append_range(h, store, h.stream(0), 1, 20);
            store.barrier();
        }
        const auto open = segment_files(d.path(), h.stream(0), ".open");
        BOOST_REQUIRE_EQUAL(open.size(), 1U);
        const auto good = file_bytes(open[0]);
        // Half a record's worth of a new record, as a crash mid-append leaves.
        {
            std::vector<std::byte> extra;
            const auto p = h.payload(21);
            detail::segments::encode_record(extra, {21, ts_of(21), p});
            extra.resize(extra.size() / 2);
            std::ofstream out(open[0], std::ios::binary | std::ios::app);
            out.write(reinterpret_cast<const char*>(extra.data()),
                      static_cast<std::streamsize>(extra.size()));
        }
        record_segment_store store(options_for(h, d.path()));
        BOOST_TEST(store.stats()._torn_tails_truncated == 1U);
        BOOST_TEST(file_bytes(segment_files(d.path(), h.stream(0), ".open")[0]) == good);
        BOOST_TEST(store.last_seq(h.stream(0)) == std::optional<std::uint64_t>(20));
        append_range(h, store, h.stream(0), 21, 30);
        check_records(h, read_all(store, h.stream(0)), 1, 30);
    }

    static void truncate_after_restores_the_snapshot_point() {
        Harness h;
        scratch_dir d;
        record_segment_store store(options_for(h, d.sub("a")));
        append_range(h, store, h.stream(0), 1, 300);
        const auto segs = store.local_segments(h.stream(0));
        BOOST_REQUIRE_GE(segs.size(), 3U);
        // Cut in the middle of a sealed segment: it becomes active again.
        const std::uint64_t cut = segs[1]._first_seq + 1;
        store.truncate_after(h.stream(0), cut);
        BOOST_TEST(store.last_seq(h.stream(0)) == std::optional<std::uint64_t>(cut));
        const auto after = store.local_segments(h.stream(0));
        BOOST_REQUIRE_EQUAL(after.size(), 2U);
        BOOST_TEST(after[0] == segs[0]);
        BOOST_TEST(!after[1]._sealed);
        check_records(h, read_all(store, h.stream(0)), 1, cut);
        // The log tail replays; the result matches a replica that never cut.
        append_range(h, store, h.stream(0), cut + 1, 300);
        record_segment_store ref(options_for(h, d.sub("ref")));
        append_range(h, ref, h.stream(0), 1, 300);
        BOOST_TEST(store.local_segments(h.stream(0)) == ref.local_segments(h.stream(0)));
        // A cut at a segment boundary and a cut of everything.
        store.truncate_after(h.stream(0), segs[0]._last_seq);
        BOOST_TEST(store.local_segments(h.stream(0)).size() == 1U);
        BOOST_TEST(store.local_segments(h.stream(0))[0]._sealed);
        store.truncate_after(h.stream(0), std::nullopt);
        BOOST_TEST(store.streams().empty());
        // Survives a restart.
        append_range(h, store, h.stream(1), 1, 10);
        store.truncate_after(h.stream(1), 4);
        store.barrier();
        record_segment_store reopened(options_for(h, d.sub("a")));
        check_records(h, read_all(reopened, h.stream(1)), 1, 4);
    }

    static void an_explicit_seal_closes_the_active_segment() {
        Harness h;
        scratch_dir d;
        record_segment_store store(options_for(h, d.path()));
        BOOST_TEST(!store.seal(h.stream(0)).has_value());
        append_range(h, store, h.stream(0), 1, 3);
        auto s = store.seal(h.stream(0));
        BOOST_REQUIRE(s.has_value());
        BOOST_TEST(s->_sealed);
        BOOST_TEST(s->_first_seq == 1U);
        BOOST_TEST(s->_last_seq == 3U);
        BOOST_TEST(s->_records == 3U);
        BOOST_TEST(!store.seal(h.stream(0)).has_value());
        append_range(h, store, h.stream(0), 4, 5);
        const auto segs = store.local_segments(h.stream(0));
        BOOST_REQUIRE_EQUAL(segs.size(), 2U);
        BOOST_TEST(segs[0] == *s);
        BOOST_TEST(segs[1]._first_seq == 4U);
        BOOST_TEST(!segs[1]._sealed);
        check_records(h, read_all(store, h.stream(0)), 1, 5);
    }

    static void an_interrupted_seal_finishes_on_open() {
        Harness h;
        scratch_dir d;
        record_segment_descriptor sealed;
        {
            record_segment_store store(options_for(h, d.path()));
            append_range(h, store, h.stream(0), 1, 4);
            sealed = *store.seal(h.stream(0));
        }
        // Footer written and fsynced, rename not done.
        const auto seg = segment_files(d.path(), h.stream(0), ".seg");
        BOOST_REQUIRE_EQUAL(seg.size(), 1U);
        auto open = seg[0];
        open.replace_extension(".open");
        std::filesystem::rename(seg[0], open);
        record_segment_store store(options_for(h, d.path()));
        const auto segs = store.local_segments(h.stream(0));
        BOOST_REQUIRE_EQUAL(segs.size(), 1U);
        BOOST_TEST(segs[0] == sealed);
        BOOST_TEST(segment_files(d.path(), h.stream(0), ".open").empty());
    }

    static void peer_fetch_installs_only_verified_bytes() {
        Harness h;
        scratch_dir d;
        record_segment_store leader(options_for(h, d.sub("leader")));
        record_segment_store fresh(options_for(h, d.sub("fresh")));
        append_range(h, leader, h.stream(0), 1, 200);
        const auto segs = leader.local_segments(h.stream(0));
        BOOST_REQUIRE_GE(segs.size(), 2U);
        BOOST_REQUIRE(!segs.back()._sealed);

        // Peer "0" is down, "1" lies, "2" is the leader.
        auto honest = transport_from({nullptr, nullptr, &leader});
        record_segment_store::fetch_transport transport =
            [&](const std::string& peer, const std::string& stream, std::uint64_t first,
                std::uint64_t length) -> std::optional<std::vector<std::byte>> {
            if (peer == "1") {
                auto bytes = leader.read_segment_bytes(stream, first, length);
                if (bytes && !bytes->empty()) {
                    (*bytes)[bytes->size() / 2] ^= std::byte{0x40};
                }
                return bytes;
            }
            return honest(peer, stream, first, length);
        };
        const std::vector<std::string> peers{"0", "1", "2"};
        for (const auto& s : segs) {
            BOOST_TEST(fresh.fetch_segment(s, peers, transport));
        }
        BOOST_TEST(fresh.stats()._fetch_misses == 2 * segs.size());
        BOOST_TEST(fresh.stats()._installed == segs.size());
        BOOST_TEST(fresh.local_segments(h.stream(0)) == segs);
        check_records(h, read_all(fresh, h.stream(0)), 1, 200);
        // Fetching again is a no-op.
        BOOST_TEST(fresh.fetch_segment(segs[0], peers, transport));
        BOOST_TEST(fresh.stats()._installed == segs.size());
        // The fetched active prefix keeps growing exactly like the leader's.
        append_range(h, leader, h.stream(0), 201, 260);
        append_range(h, fresh, h.stream(0), 201, 260);
        BOOST_TEST(fresh.local_segments(h.stream(0)) == leader.local_segments(h.stream(0)));
        // No peer has it.
        auto missing = segs[0];
        missing._first_seq = 999'999;
        BOOST_TEST(!fresh.fetch_segment(missing, peers, transport));
        // Survives a restart.
        fresh.barrier();
        const auto final_segs = fresh.local_segments(h.stream(0));
        record_segment_store reopened(options_for(h, d.sub("fresh")));
        BOOST_TEST(reopened.local_segments(h.stream(0)) == final_segs);
    }

    static void install_refuses_bytes_that_disagree_with_the_descriptor() {
        Harness h;
        scratch_dir d;
        record_segment_store a(options_for(h, d.sub("a")));
        record_segment_store b(options_for(h, d.sub("b")));
        append_range(h, a, h.stream(0), 1, 200);
        const auto segs = a.local_segments(h.stream(0));
        const auto& s = segs.front();
        auto bytes = *a.read_segment_bytes(s._stream, s._first_seq, s._bytes);

        auto wrong = s;
        wrong._sha256[0] ^= std::byte{1};
        BOOST_CHECK_THROW(b.install_segment(wrong, bytes), std::invalid_argument);
        // The digest matches but the descriptor lies about the content.
        wrong = s;
        wrong._last_seq += 1;
        BOOST_CHECK_THROW(b.install_segment(wrong, bytes), std::invalid_argument);
        wrong = s;
        wrong._stream = h.stream(1);
        BOOST_CHECK_THROW(b.install_segment(wrong, bytes), std::invalid_argument);
        wrong = s;
        wrong._sealed = !s._sealed;
        BOOST_CHECK_THROW(b.install_segment(wrong, bytes), std::invalid_argument);
        auto shorter = bytes;
        shorter.pop_back();
        BOOST_CHECK_THROW(b.install_segment(s, shorter), std::invalid_argument);
        BOOST_TEST(b.streams().empty());
        // An active segment cannot be installed below a later one.
        const auto& later = segs[1];
        BOOST_REQUIRE(b.install_segment(
            later, *a.read_segment_bytes(later._stream, later._first_seq, later._bytes)));
        record_segment_store c(options_for(h, d.sub("c")));
        append_range(h, c, h.stream(0), 1, 2);
        const auto early = c.local_segments(h.stream(0)).front();
        BOOST_REQUIRE(!early._sealed);
        BOOST_CHECK_THROW(
            b.install_segment(early,
                              *c.read_segment_bytes(early._stream, early._first_seq, early._bytes)),
            std::invalid_argument);
        BOOST_TEST(b.install_segment(s, bytes));
        BOOST_TEST(!b.install_segment(s, bytes));
        check_records(h, read_all(b, h.stream(0), s._first_seq), s._first_seq, later._last_seq);
    }

    static void trims_drop_only_sealed_segments_below_the_horizon() {
        Harness h;
        scratch_dir d;
        record_segment_store store(options_for(h, d.path()));
        append_range(h, store, h.stream(0), 1, 300);
        const auto segs = store.local_segments(h.stream(0));
        BOOST_REQUIRE_GE(segs.size(), 3U);
        // A horizon inside segment 1 keeps segment 1.
        auto removed = store.trim_before(h.stream(0), segs[1]._first_seq + 1);
        BOOST_TEST(removed == std::vector<std::uint64_t>{segs[0]._first_seq});
        check_records(h, read_all(store, h.stream(0), segs[1]._first_seq), segs[1]._first_seq, 300);
        // Everything sealed goes; the active segment stays.
        removed = store.trim_before(h.stream(0), 1'000'000);
        BOOST_TEST(removed.size() == segs.size() - 2);
        const auto left = store.local_segments(h.stream(0));
        BOOST_REQUIRE_EQUAL(left.size(), 1U);
        BOOST_TEST(!left[0]._sealed);
        // Tiering drops one sealed copy.
        store.seal(h.stream(0));
        BOOST_TEST(store.remove_segment(h.stream(0), left[0]._first_seq));
        BOOST_TEST(!store.remove_segment(h.stream(0), left[0]._first_seq));
        BOOST_TEST(store.streams().empty());
    }

    static void descriptors_round_trip_through_their_encoding() {
        Harness h;
        scratch_dir d;
        record_segment_store store(options_for(h, d.path()));
        append_range(h, store, h.stream(0), 1, 150);
        const auto segs = store.local_segments(h.stream(0));
        std::vector<std::byte> buf;
        for (const auto& s : segs) {
            const auto e = encode_segment_descriptor(s);
            buf.insert(buf.end(), e.begin(), e.end());
        }
        std::span<const std::byte> in(buf);
        for (const auto& s : segs) {
            auto got = decode_segment_descriptor(in);
            BOOST_REQUIRE(got.has_value());
            BOOST_TEST(*got == s);
        }
        BOOST_TEST(in.empty());
        std::span<const std::byte> truncated(buf.data(), 10);
        BOOST_TEST(!decode_segment_descriptor(truncated).has_value());
        BOOST_TEST(truncated.size() == 10U);
    }

    static void damaged_segments_are_never_served_as_records() {
        Harness h;
        scratch_dir d;
        std::vector<record_segment_descriptor> segs;
        {
            record_segment_store store(options_for(h, d.path()));
            append_range(h, store, h.stream(0), 1, 300);
            store.barrier();
            segs = store.local_segments(h.stream(0));
        }
        const auto sealed = segment_files(d.path(), h.stream(0), ".seg");
        BOOST_REQUIRE_GE(sealed.size(), 2U);
        // Bit rot in the middle of one sealed segment, and a smashed footer
        // on another.
        auto flip = [](const std::filesystem::path& p, std::size_t at) {
            auto bytes = file_bytes(p);
            bytes[at] ^= std::byte{0x01};
            std::ofstream out(p, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(bytes.data()),
                      static_cast<std::streamsize>(bytes.size()));
        };
        flip(sealed[0], static_cast<std::size_t>(std::filesystem::file_size(sealed[0]) / 2));
        flip(sealed[1], static_cast<std::size_t>(std::filesystem::file_size(sealed[1]) - 2));
        record_segment_store store(options_for(h, d.path()));
        BOOST_TEST(store.stats()._quarantined == 1U);
        BOOST_TEST(!store.describe(h.stream(0), segs[1]._first_seq).has_value());
        // The rotted one is still listed, but its digest no longer matches
        // the index and reading it throws instead of returning bad bytes.
        BOOST_TEST(!store.has_segment(segs[0]));
        BOOST_CHECK_THROW((void)store.read_segment(
                              h.stream(0), segs[0]._first_seq, 0,
                              {._max_records = 100000, ._max_bytes = std::uint64_t{1} << 40U}),
                          record_segment_error);
        BOOST_TEST(store.stats()._read_crc_failures == 1U);
        // Repair: drop it and fetch a good copy.
        record_segment_store good(options_for(h, d.sub("good")));
        append_range(h, good, h.stream(0), 1, 300);
        BOOST_TEST(store.remove_segment(h.stream(0), segs[0]._first_seq));
        const std::vector<std::string> peers{"0"};
        BOOST_TEST(store.fetch_segment(segs[0], peers, transport_from({&good})));
        BOOST_TEST(store.fetch_segment(segs[1], peers, transport_from({&good})));
        check_records(h, read_all(store, h.stream(0)), 1, 300);
    }

    static void reads_run_beside_appends() {
        Harness h;
        scratch_dir d;
        record_segment_store store(options_for(h, d.path()));
        std::atomic<bool> done{false};
        std::thread writer([&] {
            append_range(h, store, h.stream(0), 1, 400, 1, 3);
            done = true;
        });
        std::uint64_t next = 1;
        std::uint64_t seen = 0;
        while (!done || next <= 400) {
            auto r = store.read(h.stream(0), next, {._max_records = 17, ._max_bytes = 1U << 20U});
            for (const auto& rec : r._records) {
                BOOST_REQUIRE_EQUAL(rec._seq, next);
                BOOST_REQUIRE(rec._data == h.payload(rec._seq));
                ++next;
                ++seen;
            }
            if (done && r._records.empty() && next > 400) {
                break;
            }
        }
        writer.join();
        BOOST_TEST(seen == 400U);
    }
};

}  // namespace kythira::segment_conformance

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define KYTHIRA_RSC_CASE(harness_type, case_name)                       \
    BOOST_AUTO_TEST_CASE(case_name, *boost::unit_test::timeout(120)) {  \
        kythira::segment_conformance::cases<harness_type>::case_name(); \
    }

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define KYTHIRA_RECORD_SEGMENT_CONFORMANCE(harness_type, suite_prefix)                      \
    BOOST_AUTO_TEST_SUITE(suite_prefix##_record_segment_conformance)                        \
    KYTHIRA_RSC_CASE(harness_type, appends_read_back_in_order_per_stream)                   \
    KYTHIRA_RSC_CASE(harness_type, sequences_must_increase_but_may_skip)                    \
    KYTHIRA_RSC_CASE(harness_type, replicas_roll_identically_and_agree_on_digests)          \
    KYTHIRA_RSC_CASE(harness_type, rolls_on_record_age)                                     \
    KYTHIRA_RSC_CASE(harness_type, reads_honour_limits_and_always_progress)                 \
    KYTHIRA_RSC_CASE(harness_type, read_segment_walks_one_segment_at_a_time)                \
    KYTHIRA_RSC_CASE(harness_type,                                                          \
                     a_restart_keeps_barriered_records_and_resumes_the_active_segment)      \
    KYTHIRA_RSC_CASE(harness_type, an_open_truncates_a_torn_tail)                           \
    KYTHIRA_RSC_CASE(harness_type, truncate_after_restores_the_snapshot_point)              \
    KYTHIRA_RSC_CASE(harness_type, an_explicit_seal_closes_the_active_segment)              \
    KYTHIRA_RSC_CASE(harness_type, an_interrupted_seal_finishes_on_open)                    \
    KYTHIRA_RSC_CASE(harness_type, peer_fetch_installs_only_verified_bytes)                 \
    KYTHIRA_RSC_CASE(harness_type, install_refuses_bytes_that_disagree_with_the_descriptor) \
    KYTHIRA_RSC_CASE(harness_type, trims_drop_only_sealed_segments_below_the_horizon)       \
    KYTHIRA_RSC_CASE(harness_type, descriptors_round_trip_through_their_encoding)           \
    KYTHIRA_RSC_CASE(harness_type, damaged_segments_are_never_served_as_records)            \
    KYTHIRA_RSC_CASE(harness_type, reads_run_beside_appends)                                \
    BOOST_AUTO_TEST_SUITE_END()
