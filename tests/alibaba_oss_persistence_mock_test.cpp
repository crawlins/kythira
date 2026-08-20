/// @file alibaba_oss_persistence_mock_test.cpp
/// @brief `alibaba_oss_persistence_engine` driven end to end against
///        `alibaba_mock_server`'s OSS bucket
///        (`.kiro/specs/alibaba-cloud-services/`, Requirements 16.6-16.7).
///
/// The unit tier already runs the `file_persistence_unit_test.cpp` pattern
/// against a plain `httplib::Server`. What this tier adds is the two properties
/// that server cannot have.
///
/// **Every request is signature-verified from the bytes that arrived.** The
/// mock rebuilds each OSS V4 canonical request out of the received
/// request-target, the received `x-oss-*`/`Content-Type` headers and the
/// received credential scope, then recomputes the HMAC. That is not a
/// formality: `alibaba_oss_client`'s first live `PutObject` failed with
/// `SignatureDoesNotMatch` because the signed set omitted `Content-Type` while
/// cpp-httplib's body overload put it on the wire, and **no local test could
/// see it** — the mock tier did not verify, and the golden vector could not be
/// reproduced because the vendor masked its example's secret (spike-notes.md
/// Finding 2). The `alibaba_oss_persistence_mock_signature` suite below
/// reproduces exactly that defect on purpose and requires the mock to refuse
/// it, which is what makes the class of bug locally catchable from now on.
///
/// **Multi-page listings.** Recovery is one List plus one GET per live object,
/// and the List paginates. The mock's page size is a knob, so the
/// continuation-token walk runs against a log too long for one page — the shape
/// a real bucket takes and the unit tier's single-page server never does.
///
/// Reload-survival is the recurring assertion: an engine is destroyed and a
/// fresh one constructed over the same bucket state, because "the mirror
/// remembers" and "the bytes are in the store" are different claims and only
/// the second one survives losing the instance.

#define BOOST_TEST_MODULE alibaba_oss_persistence_mock_test
#include <boost/test/unit_test.hpp>

#include <raft/alibaba_oss_client.hpp>
#include <raft/alibaba_oss_persistence.hpp>
#include <raft/persistence.hpp>
#include <raft/types.hpp>

#include "alibaba_mock_server.hpp"
#include "object_store_conformance.hpp"
#include "test_timeout_scale.hpp"

#include <httplib.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

using kythira::testing::alibaba_mock_server;
using namespace std::chrono_literals;

using engine_t = kythira::alibaba_oss_persistence_engine<>;
using log_entry_t = kythira::log_entry<>;
using snapshot_t = kythira::snapshot<>;

namespace {

constexpr const char* k_prefix = "raft";

/// A started mock plus the config that points an engine at it.
struct MockFixture {
    alibaba_mock_server server;

    MockFixture() { server.start(); }
    ~MockFixture() { server.stop(); }

    MockFixture(const MockFixture&) = delete;
    auto operator=(const MockFixture&) -> MockFixture& = delete;
    MockFixture(MockFixture&&) = delete;
    auto operator=(MockFixture&&) -> MockFixture& = delete;

    [[nodiscard]] auto config() const -> kythira::alibaba_client_config {
        kythira::alibaba_client_config cfg;
        cfg.region = "cn-hangzhou";
        cfg.access_key_id = server.access_key_id();
        cfg.access_key_secret = server.access_key_secret();
        cfg.endpoint_override = server.origin();
        cfg.api_timeout = 10s;
        return cfg;
    }

    [[nodiscard]] auto engine() const -> engine_t {
        return engine_t{config(), server.bucket(), k_prefix};
    }
};

auto make_entry(std::uint64_t term, std::uint64_t index,
                std::vector<std::byte> command = {std::byte{0xAB}}) -> log_entry_t {
    log_entry_t entry;
    entry._term = term;
    entry._index = index;
    entry._command = std::move(command);
    return entry;
}

auto padded(std::uint64_t index) -> std::string {
    const std::string digits = std::to_string(index);
    return std::string(k_prefix) + "/log/" + std::string(20 - digits.size(), '0') + digits;
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Signature verification is live
// ─────────────────────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(alibaba_oss_persistence_mock_signature)

/// **The live defect, reproduced.**
///
/// `alibaba_oss_client::prepared_headers` takes the content type as a
/// parameter, so passing an empty one reconstructs the client exactly as it was
/// when live OSS rejected it: `Content-Type` on the wire (httplib's body
/// overload always puts it there) and absent from the signed set. The mock
/// derives the signed set from the received headers, as OSS does, so the two
/// canonical requests differ by exactly one `content-type:...` line — and this
/// is rejected.
///
/// The second half of the case is what makes the first half meaningful: the
/// identical request with the content type folded into the signature is
/// accepted. Without it, this case would also pass against a mock that refused
/// everything.
BOOST_AUTO_TEST_CASE(a_put_that_sends_content_type_without_signing_it_is_rejected,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    MockFixture fixture;
    const kythira::alibaba_oss_client oss{fixture.config()};
    const std::string target = "/" + fixture.server.bucket() + "/raft/term";
    const auto when = std::chrono::system_clock::now();

    auto send = [&](const std::string& signed_content_type) {
        httplib::Headers wire;
        for (const auto& [name, value] : oss.prepared_headers(
                 fixture.server.bucket(), "raft/term", "PUT", {}, when, signed_content_type)) {
            // Withheld from the map for the same reason the client withholds
            // it: httplib sets it from the body overload below, and supplying
            // it twice puts two on the wire.
            if (name == "content-type") {
                continue;
            }
            wire.emplace(name, value);
        }
        httplib::Client client(fixture.server.origin());
        return client.Put(target, wire, std::string("42"), "application/octet-stream");
    };

    // The pre-fix client: signs no content type, sends one.
    auto rejected = send({});
    BOOST_REQUIRE(rejected);
    BOOST_CHECK_EQUAL(rejected->status, 403);
    BOOST_CHECK(rejected->body.find("SignatureDoesNotMatch") != std::string::npos);
    // Real OSS echoes the canonical request it computed, and that echo is how
    // the live defect was actually diagnosed — the difference was exactly this
    // line. Reproducing it locally is the point of the whole tier.
    BOOST_CHECK(rejected->body.find("content-type:application/octet-stream") != std::string::npos);
    BOOST_CHECK(!fixture.server.oss_has("raft/term"));
    BOOST_CHECK_EQUAL(fixture.server.signature_failures(), 1U);

    // The shipped client: signs the content type it is about to send.
    auto accepted = send("application/octet-stream");
    BOOST_REQUIRE(accepted);
    BOOST_CHECK_EQUAL(accepted->status, 200);
    BOOST_CHECK_EQUAL(fixture.server.oss_body("raft/term"), "42");
    BOOST_CHECK_EQUAL(fixture.server.signature_failures(), 1U);
}

/// The baseline. Without it, every functional case below is compatible with a
/// mock that never looked at the `Authorization` header — which is precisely
/// the state that let two OCI defects and one OSS defect ship.
BOOST_AUTO_TEST_CASE(an_unsigned_object_request_is_rejected,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    MockFixture fixture;
    fixture.server.oss_put("raft/term", "7");

    httplib::Client client(fixture.server.origin());
    auto result = client.Get("/" + fixture.server.bucket() + "/raft/term");

    BOOST_REQUIRE(result);
    BOOST_CHECK_EQUAL(result->status, 403);
    BOOST_CHECK(result->body.find("AccessDenied") != std::string::npos);
    BOOST_CHECK_EQUAL(fixture.server.signature_failures(), 1U);
}

/// One flipped hex character. If this ever answers 200, the mock has stopped
/// verifying and every other case in this file has quietly lost its meaning.
BOOST_AUTO_TEST_CASE(a_tampered_object_signature_is_rejected,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    MockFixture fixture;
    fixture.server.oss_put("raft/term", "7");
    const kythira::alibaba_oss_client oss{fixture.config()};

    auto headers = oss.prepared_headers(fixture.server.bucket(), "raft/term", "GET", {},
                                        std::chrono::system_clock::now());
    auto& authorization = headers["Authorization"];
    authorization.back() = authorization.back() == 'a' ? 'b' : 'a';

    httplib::Headers wire;
    for (const auto& [name, value] : headers) {
        wire.emplace(name, value);
    }
    httplib::Client client(fixture.server.origin());
    auto result = client.Get("/" + fixture.server.bucket() + "/raft/term", wire);

    BOOST_REQUIRE(result);
    BOOST_CHECK_EQUAL(result->status, 403);
    BOOST_CHECK(result->body.find("SignatureDoesNotMatch") != std::string::npos);
    BOOST_CHECK(fixture.server.last_rejected_canonical_request().starts_with("GET\n/"));
}

/// Signing one resource and requesting another is the path-level cousin of the
/// `Host` defect: both sides of a golden vector agree by construction, so only
/// a server that canonicalises the *received* target can see it.
BOOST_AUTO_TEST_CASE(signing_one_key_and_requesting_another_is_rejected,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    MockFixture fixture;
    fixture.server.oss_put("raft/voted_for", "3");
    const kythira::alibaba_oss_client oss{fixture.config()};

    auto headers = oss.prepared_headers(fixture.server.bucket(), "raft/term", "GET", {},
                                        std::chrono::system_clock::now());
    httplib::Headers wire;
    for (const auto& [name, value] : headers) {
        wire.emplace(name, value);
    }
    httplib::Client client(fixture.server.origin());
    auto result = client.Get("/" + fixture.server.bucket() + "/raft/voted_for", wire);

    BOOST_REQUIRE(result);
    BOOST_CHECK_EQUAL(result->status, 403);
    BOOST_CHECK(result->body.find("SignatureDoesNotMatch") != std::string::npos);
}

/// A signature made with the wrong secret is a different bug from a signature
/// over the wrong bytes, and the vendor says so with a different code.
BOOST_AUTO_TEST_CASE(an_unknown_access_key_is_named_as_such,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    MockFixture fixture;
    auto cfg = fixture.config();
    cfg.access_key_id = "not-the-mocks-key";
    const kythira::alibaba_oss_client oss{cfg};

    BOOST_CHECK_THROW((void)oss.get_object(fixture.server.bucket(), "raft/term"),
                      std::runtime_error);
    BOOST_CHECK_EQUAL(fixture.server.signature_failures(), 1U);
}

BOOST_AUTO_TEST_SUITE_END()

// ─────────────────────────────────────────────────────────────────────────────
// Construction and recovery
// ─────────────────────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(alibaba_oss_persistence_mock_construction)

/// Requirement 15.5: a cold start reads the (empty) listing and writes nothing.
BOOST_AUTO_TEST_CASE(a_cold_start_writes_no_object,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    MockFixture fixture;
    engine_t engine = fixture.engine();

    BOOST_CHECK_EQUAL(engine.load_current_term(), 0U);
    BOOST_CHECK(!engine.load_voted_for().has_value());
    BOOST_CHECK_EQUAL(engine.get_last_log_index(), 0U);
    BOOST_CHECK(!engine.load_snapshot().has_value());

    BOOST_CHECK(fixture.server.oss_keys().empty());
    BOOST_CHECK_EQUAL(fixture.server.count_requests("OSS PUT"), 0U);
    BOOST_CHECK_EQUAL(fixture.server.count_requests("OSS LIST"), 1U);
    BOOST_CHECK_EQUAL(fixture.server.signature_failures(), 0U);
}

/// Requirement 15.6: a corrupt object under the prefix is fatal at load and the
/// message names the key. A Raft node that comes back with a silently shortened
/// log can violate Log Matching; failing to start is recoverable by an operator,
/// starting with invented state is not.
BOOST_AUTO_TEST_CASE(a_corrupt_object_fails_construction_and_names_its_key,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    MockFixture fixture;
    fixture.server.oss_put("raft/term", "not-a-number");

    try {
        (void)fixture.engine();
        BOOST_FAIL("construction should have thrown on a corrupt term object");
    } catch (const std::runtime_error& e) {
        const std::string what = e.what();
        BOOST_CHECK(what.find("raft/term") != std::string::npos);
    }
}

/// The same rule for a log record whose body disagrees with the key that names
/// it — the ordering guarantee the 20-digit padding buys is worthless if a
/// record can claim a different index than its key.
BOOST_AUTO_TEST_CASE(a_log_record_disagreeing_with_its_key_fails_construction,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    MockFixture fixture;
    fixture.server.oss_put(padded(4), R"({"term":1,"index":9,"command":"qw==","type":0})");

    BOOST_CHECK_THROW((void)fixture.engine(), std::runtime_error);
}

/// A key under the prefix that this format does not define belongs to something
/// else — an operator's note, a future format's object. It is neither read nor
/// written, and must not stop the node from starting.
BOOST_AUTO_TEST_CASE(an_unrelated_key_under_the_prefix_is_ignored,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    MockFixture fixture;
    fixture.server.oss_put("raft/README", "left here by an operator");
    fixture.server.oss_put("raft/term", "5");

    engine_t engine = fixture.engine();
    BOOST_CHECK_EQUAL(engine.load_current_term(), 5U);
    BOOST_CHECK(fixture.server.oss_has("raft/README"));
}

BOOST_AUTO_TEST_SUITE_END()

// ─────────────────────────────────────────────────────────────────────────────
// Round trips and reload survival
// ─────────────────────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(alibaba_oss_persistence_mock_round_trip)

/// Every field, written by one engine and read by a *different* one over the
/// same bucket. Reload rather than read-back is the point: an in-memory mirror
/// answers a read-back correctly whether or not anything reached OSS.
BOOST_AUTO_TEST_CASE(every_field_survives_a_reload,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {
    MockFixture fixture;
    {
        engine_t engine = fixture.engine();
        engine.save_current_term(42);
        engine.save_voted_for(7);
        engine.append_log_entry(make_entry(1, 1, {std::byte{'a'}}));
        engine.append_log_entry(make_entry(2, 2, {std::byte{'b'}}));
        engine.append_log_entry(make_entry(2, 3, {std::byte{'c'}}));

        snapshot_t snap;
        snap._last_included_index = 2;
        snap._last_included_term = 2;
        snap._state_machine_state = {std::byte{'s'}, std::byte{'m'}};
        snap._configuration = kythira::cluster_configuration<>{{1, 2, 3}, false, std::nullopt, {}};
        engine.save_snapshot(snap);
    }

    engine_t reloaded = fixture.engine();
    BOOST_CHECK_EQUAL(reloaded.load_current_term(), 42U);
    BOOST_REQUIRE(reloaded.load_voted_for().has_value());
    BOOST_CHECK_EQUAL(*reloaded.load_voted_for(), 7U);
    BOOST_CHECK_EQUAL(reloaded.get_last_log_index(), 3U);

    auto entry = reloaded.get_log_entry(2);
    BOOST_REQUIRE(entry.has_value());
    BOOST_CHECK_EQUAL(entry->term(), 2U);
    BOOST_CHECK_EQUAL(entry->index(), 2U);
    BOOST_REQUIRE_EQUAL(entry->command().size(), 1U);
    BOOST_CHECK(entry->command().front() == std::byte{'b'});

    auto range = reloaded.get_log_entries(1, 3);
    BOOST_CHECK_EQUAL(range.size(), 3U);

    auto snap = reloaded.load_snapshot();
    BOOST_REQUIRE(snap.has_value());
    BOOST_CHECK_EQUAL(snap->last_included_index(), 2U);
    BOOST_CHECK_EQUAL(snap->last_included_term(), 2U);
    BOOST_CHECK_EQUAL(snap->configuration().nodes().size(), 3U);
    BOOST_CHECK_EQUAL(fixture.server.signature_failures(), 0U);
}

/// A command is arbitrary bytes, including the ones that are not text. The
/// base64 codec is what makes that safe, and a reload is what proves it — a
/// mirror-only assertion would pass even if the bytes never left the process.
///
/// (The escape is split from the following text deliberately: `"\x00\xff"` runs
/// into any hex-looking character that follows it.)
BOOST_AUTO_TEST_CASE(a_binary_command_survives_a_reload,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {
    MockFixture fixture;
    const std::string raw = std::string(
        "\x00\xff\x0a\x1b"
        "beef\x7f",
        9);
    std::vector<std::byte> command;
    command.reserve(raw.size());
    for (const char c : raw) {
        command.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
    }

    {
        engine_t engine = fixture.engine();
        engine.append_log_entry(make_entry(3, 1, command));
    }

    engine_t reloaded = fixture.engine();
    auto entry = reloaded.get_log_entry(1);
    BOOST_REQUIRE(entry.has_value());
    BOOST_REQUIRE_EQUAL(entry->command().size(), command.size());
    BOOST_CHECK(std::ranges::equal(entry->command(), command));
}

/// Requirement 15.7 / design's single-slot rule: a second snapshot overwrites
/// the first, and the bucket holds exactly one snapshot object either way.
BOOST_AUTO_TEST_CASE(a_second_snapshot_overwrites_the_single_slot,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {
    MockFixture fixture;
    {
        engine_t engine = fixture.engine();
        snapshot_t first;
        first._last_included_index = 5;
        first._last_included_term = 1;
        first._configuration = kythira::cluster_configuration<>{{1}, false, std::nullopt, {}};
        engine.save_snapshot(first);

        snapshot_t second;
        second._last_included_index = 12;
        second._last_included_term = 4;
        second._configuration = kythira::cluster_configuration<>{{1, 2}, false, std::nullopt, {}};
        engine.save_snapshot(second);
    }

    std::size_t snapshot_objects = 0;
    for (const auto& key : fixture.server.oss_keys()) {
        if (key.starts_with("raft/snapshot")) {
            ++snapshot_objects;
        }
    }
    BOOST_CHECK_EQUAL(snapshot_objects, 1U);

    engine_t reloaded = fixture.engine();
    auto snap = reloaded.load_snapshot();
    BOOST_REQUIRE(snap.has_value());
    BOOST_CHECK_EQUAL(snap->last_included_index(), 12U);
    BOOST_CHECK_EQUAL(snap->configuration().nodes().size(), 2U);
}

/// Design Property 5: 20 zero-padded digits make lexicographic key order equal
/// numeric index order, so a bucket listing recovers the log in order. Without
/// it `.../log/10` sorts before `.../log/9`.
BOOST_AUTO_TEST_CASE(log_keys_sort_lexicographically_in_index_order,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {
    MockFixture fixture;
    {
        engine_t engine = fixture.engine();
        for (const std::uint64_t index :
             {std::uint64_t{9}, std::uint64_t{10}, std::uint64_t{1}, std::uint64_t{100}}) {
            engine.append_log_entry(make_entry(1, index));
        }
    }

    std::vector<std::string> log_keys;
    for (const auto& key : fixture.server.oss_keys()) {
        if (key.starts_with("raft/log/")) {
            log_keys.push_back(key);
        }
    }
    // `oss_keys()` comes out of a std::map, i.e. already in the lexicographic
    // order OSS lists in.
    const std::vector<std::string> expected{padded(1), padded(9), padded(10), padded(100)};
    BOOST_CHECK_EQUAL_COLLECTIONS(log_keys.begin(), log_keys.end(), expected.begin(),
                                  expected.end());

    engine_t reloaded = fixture.engine();
    BOOST_CHECK_EQUAL(reloaded.get_last_log_index(), 100U);
}

/// Recovery is one List plus one GET per live object, and the List paginates.
/// The page size is set below the log length so the continuation-token walk
/// actually runs — a single-page recovery would silently lose the tail, which
/// is the exact silent-data-loss case Requirement 2.4 exists to prevent.
BOOST_AUTO_TEST_CASE(recovery_walks_a_multi_page_listing_to_exhaustion,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(120))) {
    MockFixture fixture;
    constexpr std::uint64_t k_entries = 11;
    {
        engine_t engine = fixture.engine();
        engine.save_current_term(3);
        for (std::uint64_t index = 1; index <= k_entries; ++index) {
            engine.append_log_entry(make_entry(1, index));
        }
    }

    fixture.server.set_oss_page_size(2);
    fixture.server.clear_request_log();

    engine_t reloaded = fixture.engine();
    BOOST_CHECK_EQUAL(reloaded.get_last_log_index(), k_entries);
    BOOST_CHECK_EQUAL(reloaded.get_log_entries(1, k_entries).size(), k_entries);
    BOOST_CHECK_EQUAL(reloaded.load_current_term(), 3U);

    // 12 objects at 2 per page: 6 pages, the last of which is short.
    BOOST_CHECK_EQUAL(fixture.server.count_requests("OSS LIST"), 6U);
    BOOST_CHECK_EQUAL(fixture.server.signature_failures(), 0U);
}

BOOST_AUTO_TEST_SUITE_END()

// ─────────────────────────────────────────────────────────────────────────────
// Mutation flows
// ─────────────────────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(alibaba_oss_persistence_mock_mutation)

/// Requirement 14.2: truncation is a bounded batch of DELETEs, one per affected
/// entry **and no others**. The whole reason one object per entry was chosen
/// over the file engine's single-file rewrite is that this stays O(affected)
/// instead of O(log size).
BOOST_AUTO_TEST_CASE(truncation_deletes_exactly_the_affected_objects,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {
    MockFixture fixture;
    engine_t engine = fixture.engine();
    for (std::uint64_t index = 1; index <= 5; ++index) {
        engine.append_log_entry(make_entry(1, index));
    }
    fixture.server.clear_request_log();

    engine.truncate_log(3);

    BOOST_CHECK_EQUAL(fixture.server.count_requests("OSS DELETE"), 3U);
    BOOST_CHECK(fixture.server.oss_has(padded(2)));
    BOOST_CHECK(!fixture.server.oss_has(padded(3)));
    BOOST_CHECK(!fixture.server.oss_has(padded(5)));
    BOOST_CHECK_EQUAL(engine.get_last_log_index(), 2U);

    engine_t reloaded = fixture.engine();
    BOOST_CHECK_EQUAL(reloaded.get_last_log_index(), 2U);
    BOOST_CHECK(!reloaded.get_log_entry(3).has_value());
}

/// Post-snapshot compaction, the mirror image of truncation.
BOOST_AUTO_TEST_CASE(compaction_deletes_exactly_the_entries_before_the_index,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {
    MockFixture fixture;
    engine_t engine = fixture.engine();
    for (std::uint64_t index = 1; index <= 5; ++index) {
        engine.append_log_entry(make_entry(1, index));
    }
    fixture.server.clear_request_log();

    engine.delete_log_entries_before(4);

    BOOST_CHECK_EQUAL(fixture.server.count_requests("OSS DELETE"), 3U);
    BOOST_CHECK(!fixture.server.oss_has(padded(1)));
    BOOST_CHECK(fixture.server.oss_has(padded(4)));

    engine_t reloaded = fixture.engine();
    BOOST_CHECK(!reloaded.get_log_entry(1).has_value());
    BOOST_CHECK(reloaded.get_log_entry(4).has_value());
    BOOST_CHECK_EQUAL(reloaded.get_last_log_index(), 5U);
}

/// Requirement 15.2, stated as an observable: the PUT is in the call stack of
/// the mutating method, so by the time it returns the store already has the
/// bytes. Asserted through the mock's request log because "returned" and
/// "durable" are only the same claim if nothing is buffered.
BOOST_AUTO_TEST_CASE(a_write_has_already_reached_the_store_when_it_returns,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {
    MockFixture fixture;
    engine_t engine = fixture.engine();
    fixture.server.clear_request_log();

    engine.save_current_term(19);

    // No sleep, no poll: if this needed one, the engine would be buffering.
    BOOST_CHECK_EQUAL(fixture.server.oss_body("raft/term"), "19");
    const auto log = fixture.server.request_log();
    BOOST_REQUIRE_EQUAL(log.size(), 1U);
    BOOST_CHECK_EQUAL(log.front(), "OSS PUT raft/term");
}

/// Requirement 15.3: a PUT is a deterministic full-object overwrite at a keyed
/// location, so re-sending it is idempotent by construction. One transient 503
/// is therefore ridden out rather than surfaced.
BOOST_AUTO_TEST_CASE(one_transient_put_failure_is_retried_and_succeeds,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {
    MockFixture fixture;
    engine_t engine = fixture.engine();
    fixture.server.clear_request_log();
    fixture.server.set_oss_failing_puts(1);

    BOOST_CHECK_NO_THROW(engine.save_current_term(23));

    BOOST_CHECK_EQUAL(fixture.server.oss_body("raft/term"), "23");
    // Two attempts: the failed one and the retry. A third would mean the
    // engine's "once, and only once" bound had slipped.
    BOOST_CHECK_EQUAL(fixture.server.count_requests("OSS PUT"), 2U);
}

/// Design Property 1: the mirror is updated **after** the store acknowledges,
/// never before. A write that fails twice therefore leaves memory exactly equal
/// to the bucket, and a retry re-executes the full PUT.
BOOST_AUTO_TEST_CASE(a_put_that_fails_twice_throws_and_leaves_the_mirror_untouched,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {
    MockFixture fixture;
    engine_t engine = fixture.engine();
    engine.save_current_term(4);
    fixture.server.set_oss_put_status(500);

    BOOST_CHECK_THROW(engine.save_current_term(5), std::runtime_error);

    BOOST_CHECK_EQUAL(engine.load_current_term(), 4U);
    BOOST_CHECK_EQUAL(fixture.server.oss_body("raft/term"), "4");

    // And once the store recovers, the same call succeeds and both agree again.
    fixture.server.set_oss_put_status(0);
    BOOST_CHECK_NO_THROW(engine.save_current_term(5));
    BOOST_CHECK_EQUAL(engine.load_current_term(), 5U);
    BOOST_CHECK_EQUAL(fixture.server.oss_body("raft/term"), "5");
}

/// The same ordering for the log: a failed append must not leave an entry in
/// the mirror that no reload would produce.
BOOST_AUTO_TEST_CASE(a_failed_append_leaves_no_phantom_entry,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {
    MockFixture fixture;
    engine_t engine = fixture.engine();
    engine.append_log_entry(make_entry(1, 1));
    fixture.server.set_oss_put_status(503);

    BOOST_CHECK_THROW(engine.append_log_entry(make_entry(1, 2)), std::runtime_error);

    BOOST_CHECK(!engine.get_log_entry(2).has_value());
    BOOST_CHECK_EQUAL(engine.get_last_log_index(), 1U);
    BOOST_CHECK(!fixture.server.oss_has(padded(2)));

    fixture.server.set_oss_put_status(0);
    engine_t reloaded = fixture.engine();
    BOOST_CHECK_EQUAL(reloaded.get_last_log_index(), 1U);
}

/// A full election-and-append sequence in one case, then a reload: the ordering
/// the Raft layer actually produces, rather than each method in isolation.
BOOST_AUTO_TEST_CASE(an_election_and_replication_sequence_survives_a_reload,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(120))) {
    MockFixture fixture;
    {
        engine_t engine = fixture.engine();
        // A candidate persists its term and self-vote before sending a single
        // RequestVote — the two writes on the election hot path.
        engine.save_current_term(1);
        engine.save_voted_for(1);
        engine.append_log_entry(make_entry(1, 1, {std::byte{'x'}}));
        engine.append_log_entry(make_entry(1, 2, {std::byte{'y'}}));

        // A term bump from a competing leader, then a conflicting suffix.
        engine.save_current_term(2);
        engine.truncate_log(2);
        engine.append_log_entry(make_entry(2, 2, {std::byte{'z'}}));

        // Compaction after a snapshot.
        snapshot_t snap;
        snap._last_included_index = 1;
        snap._last_included_term = 1;
        snap._configuration = kythira::cluster_configuration<>{{1, 2, 3}, false, std::nullopt, {}};
        engine.save_snapshot(snap);
        engine.delete_log_entries_before(2);
    }

    engine_t reloaded = fixture.engine();
    BOOST_CHECK_EQUAL(reloaded.load_current_term(), 2U);
    BOOST_REQUIRE(reloaded.load_voted_for().has_value());
    BOOST_CHECK_EQUAL(*reloaded.load_voted_for(), 1U);
    BOOST_CHECK(!reloaded.get_log_entry(1).has_value());
    auto surviving = reloaded.get_log_entry(2);
    BOOST_REQUIRE(surviving.has_value());
    BOOST_CHECK_EQUAL(surviving->term(), 2U);
    BOOST_CHECK(surviving->command().front() == std::byte{'z'});
    BOOST_REQUIRE(reloaded.load_snapshot().has_value());
    BOOST_CHECK_EQUAL(reloaded.load_snapshot()->last_included_index(), 1U);
    BOOST_CHECK_EQUAL(fixture.server.signature_failures(), 0U);
}

/// Two engines over the same bucket but different prefixes must not see each
/// other. The single-writer rule (Requirement 15.8) is about a shared
/// `{bucket, prefix}` pair; separate prefixes are a supported layout.
BOOST_AUTO_TEST_CASE(two_prefixes_in_one_bucket_are_independent,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {
    MockFixture fixture;
    {
        engine_t first{fixture.config(), fixture.server.bucket(), "node-a"};
        engine_t second{fixture.config(), fixture.server.bucket(), "node-b"};
        first.save_current_term(11);
        second.save_current_term(22);
    }

    engine_t first{fixture.config(), fixture.server.bucket(), "node-a"};
    engine_t second{fixture.config(), fixture.server.bucket(), "node-b"};
    BOOST_CHECK_EQUAL(first.load_current_term(), 11U);
    BOOST_CHECK_EQUAL(second.load_current_term(), 22U);
    BOOST_CHECK_EQUAL(fixture.server.signature_failures(), 0U);
}

BOOST_AUTO_TEST_SUITE_END()

// ── The engine conformance suite over the OSS mock tier (task 15.5) ──────────
//
// The same suite that proves `object_store_persistence_engine` over an
// in-memory map, run again over a real HTTP round trip with **OSS V4 signatures
// verified from the bytes that arrived**. That verification is the reason this
// tier is worth more than a plain httplib server: `alibaba_oss_client`'s first
// live PutObject failed with `SignatureDoesNotMatch` because the signed header
// set omitted `Content-Type` while cpp-httplib put it on the wire, and no local
// tier could see it until this mock started verifying.
//
// **The fencing suite is absent and that absence is the finding.** OSS is the
// one provider of the five that cannot express an overwrite compare-and-swap —
// a conditional PUT against a *current* ETag is refused `400 NotImplemented`
// (spike-notes Finding 1) — so `alibaba_oss_client` deliberately does not
// satisfy `conditional_key_object_store` and the fenced engine will not
// instantiate over it. Requirement 9.8 fires for exactly this provider.
//
// The create-only half it *does* have (`x-oss-forbid-overwrite`, refused `409
// FileAlreadyExists`) is modelled by the mock. Note that 409 means the opposite
// on S3 — "retry" rather than "you lost" — which is why no client here may map
// a bare status without reading the service's own error code.
//
// NO_INJECTION: the mock has no transient-failure knobs, so the retry cases are
// not instantiated. Stated rather than quietly skipped — a harness that claimed
// the full suite while omitting them would misreport what had been proved.

namespace {

/// Conformance harness over the started OSS mock.
struct alibaba_oss_mock_harness {
    using engine_t = kythira::object_store_persistence_engine<kythira::alibaba_oss_client>;

    alibaba_mock_server server;

    alibaba_oss_mock_harness() { server.start(); }
    ~alibaba_oss_mock_harness() { server.stop(); }

    alibaba_oss_mock_harness(const alibaba_oss_mock_harness&) = delete;
    auto operator=(const alibaba_oss_mock_harness&) -> alibaba_oss_mock_harness& = delete;
    alibaba_oss_mock_harness(alibaba_oss_mock_harness&&) = delete;
    auto operator=(alibaba_oss_mock_harness&&) -> alibaba_oss_mock_harness& = delete;

    [[nodiscard]] auto client() const -> kythira::alibaba_oss_client {
        kythira::alibaba_client_config cfg;
        cfg.region = "cn-hangzhou";
        cfg.access_key_id = server.access_key_id();
        cfg.access_key_secret = server.access_key_secret();
        cfg.endpoint_override = server.origin();
        cfg.api_timeout = 10s;
        return kythira::alibaba_oss_client{cfg};
    }

    [[nodiscard]] auto bucket() const -> std::string { return server.bucket(); }
    [[nodiscard]] auto prefix() const -> std::string { return "node-1"; }

    [[nodiscard]] auto make_engine() const -> engine_t {
        return engine_t{client(), bucket(), prefix()};
    }
    [[nodiscard]] auto make_engine(const std::string& b, const std::string& p) const -> engine_t {
        return engine_t{client(), b, p};
    }
    [[nodiscard]] auto make_engine(kythira::object_persistence_options opts) const -> engine_t {
        return engine_t{client(), bucket(), prefix(), std::move(opts)};
    }

    // Inspection goes straight into the server's map, never over HTTP: the
    // suite calls `keys()` before `count_requests("LIST")`, so an inspecting
    // LIST would look like one the engine made.
    auto seed(const std::string& key, std::string_view body) -> void {
        server.seed_oss_object(key, body);
    }
    [[nodiscard]] auto keys() const -> std::vector<std::string> { return server.oss_keys(); }
    [[nodiscard]] auto has(const std::string& key) const -> bool { return server.oss_has(key); }
    [[nodiscard]] auto body(const std::string& key) const -> std::string {
        return server.oss_body(key);
    }

    [[nodiscard]] auto request_log() const -> std::vector<std::string> {
        return server.oss_requests();
    }
    [[nodiscard]] auto count_requests(std::string_view verb) const -> std::size_t {
        return server.count_oss_requests(verb);
    }
    auto clear_requests() -> void { server.clear_oss_requests(); }
};

/// The macro pastes its store argument into an identifier, so a
/// namespace-qualified type does not compile. Alias first — the same shape this
/// file's `BOOST_GLOBAL_FIXTURE` note describes.
using oss_client_t = kythira::alibaba_oss_client;

}  // namespace

KYTHIRA_OBJECT_STORE_CONFORMANCE_NO_INJECTION(alibaba_oss_mock_harness, oss_client_t)
