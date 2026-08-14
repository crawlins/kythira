#define BOOST_TEST_MODULE alibaba_oss_persistence_unit_test
#include <boost/test/unit_test.hpp>

#include <raft/alibaba_oss_persistence.hpp>
#include <raft/persistence.hpp>
#include <raft/types.hpp>

#include <httplib.h>

#ifdef FIU_ENABLE
#include <fiu-control.h>
#endif

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Unit coverage for the OSS persistence engine
// (.kiro/specs/alibaba-cloud-services/ Requirement 16.5): the
// `file_persistence_unit_test.cpp` pattern — round trip *and* reload-survival
// per field, binary commands, single-slot snapshot — run against a local
// `httplib::Server` acting as an OSS bucket, plus the four things that are
// specific to a cloud engine:
//
//   * corruption at load is fatal and names the key (15.6),
//   * a PUT that fails leaves the in-memory mirror untouched (design
//     Property 1),
//   * a returned write has already reached the store — the write is
//     synchronous, not buffered (15.2),
//   * log keys are zero-padded so listing order is index order (Property 5).
//
// The mock is reached through `endpoint_override`, which flips the OSS client
// to path-style addressing (`/<bucket>/<key>`) so no wildcard DNS is needed —
// the established no-seam mocking idiom in this tree.

using namespace std::chrono_literals;

using engine_t = kythira::alibaba_oss_persistence_engine<>;
using log_entry_t = kythira::log_entry<>;
using snapshot_t = kythira::snapshot<>;

namespace {

constexpr const char* k_bucket = "kythira";
constexpr const char* k_prefix = "raft";

/// An in-memory OSS bucket: PUT/GET/DELETE/ListObjectsV2 over path-style keys,
/// with a request log (so a test can prove a write completed before its method
/// returned) and injectable PUT failures.
struct MockOss {
    httplib::Server server;
    std::thread thread;
    int port{0};

    mutable std::mutex mu;
    std::map<std::string, std::string> store;
    std::vector<std::string> requests;

    /// When non-zero, every PUT answers with this status instead of storing.
    std::atomic<int> put_status{0};
    /// Fails this many further PUTs, then goes back to storing — a transient
    /// failure the engine's single retry is supposed to ride out.
    std::atomic<int> fail_next_puts{0};
    /// Artificial server-side latency on PUT, used to show the caller waits.
    std::atomic<int> put_delay_ms{0};

    MockOss() {
        const std::string route = std::string("/") + k_bucket + "/(.*)";

        server.Put(route, [this](const httplib::Request& req, httplib::Response& res) {
            const std::string key = req.matches[1].str();
            const int delay = put_delay_ms.load();
            if (delay > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(delay));
            }
            {
                const std::lock_guard lock(mu);
                requests.push_back("PUT " + key);
            }
            int forced = put_status.load();
            if (forced == 0 && fail_next_puts.load() > 0) {
                fail_next_puts.fetch_sub(1);
                forced = 503;
            }
            if (forced != 0) {
                res.status = forced;
                res.set_content(
                    "<Error><Code>InternalError</Code>"
                    "<Message>injected</Message></Error>",
                    "application/xml");
                return;
            }
            {
                const std::lock_guard lock(mu);
                store[key] = req.body;
            }
            res.status = 200;
        });

        server.Get(route, [this](const httplib::Request& req, httplib::Response& res) {
            const std::string key = req.matches[1].str();
            if (key.empty()) {
                list(req, res);
                return;
            }
            std::string body;
            bool found = false;
            {
                const std::lock_guard lock(mu);
                requests.push_back("GET " + key);
                auto it = store.find(key);
                found = it != store.end();
                if (found) {
                    body = it->second;
                }
            }
            if (!found) {
                res.status = 404;
                res.set_content("<Error><Code>NoSuchKey</Code></Error>", "application/xml");
                return;
            }
            res.set_content(body, "application/octet-stream");
        });

        server.Delete(route, [this](const httplib::Request& req, httplib::Response& res) {
            const std::string key = req.matches[1].str();
            {
                const std::lock_guard lock(mu);
                requests.push_back("DELETE " + key);
                store.erase(key);
            }
            // OSS answers 204 whether or not the key was there.
            res.status = 204;
        });

        port = server.bind_to_any_port("127.0.0.1");
        thread = std::thread([this] { server.listen_after_bind(); });
        for (int i = 0; i < 400 && !server.is_running(); ++i) {
            std::this_thread::sleep_for(5ms);
        }
    }

    ~MockOss() {
        server.stop();
        if (thread.joinable()) {
            thread.join();
        }
    }

    MockOss(const MockOss&) = delete;
    auto operator=(const MockOss&) -> MockOss& = delete;
    MockOss(MockOss&&) = delete;
    auto operator=(MockOss&&) -> MockOss& = delete;

    [[nodiscard]] auto origin() const -> std::string {
        return "http://127.0.0.1:" + std::to_string(port);
    }

    // ── State inspection, from the test thread ───────────────────────────────

    [[nodiscard]] auto keys() const -> std::vector<std::string> {
        const std::lock_guard lock(mu);
        std::vector<std::string> out;
        out.reserve(store.size());
        for (const auto& [key, _] : store) {
            out.push_back(key);
        }
        return out;
    }

    [[nodiscard]] auto has(const std::string& key) const -> bool {
        const std::lock_guard lock(mu);
        return store.contains(key);
    }

    [[nodiscard]] auto body(const std::string& key) const -> std::string {
        const std::lock_guard lock(mu);
        auto it = store.find(key);
        return it == store.end() ? std::string{} : it->second;
    }

    auto put(const std::string& key, const std::string& value) -> void {
        const std::lock_guard lock(mu);
        store[key] = value;
    }

    [[nodiscard]] auto request_log() const -> std::vector<std::string> {
        const std::lock_guard lock(mu);
        return requests;
    }

    [[nodiscard]] auto count_requests(const std::string& verb) const -> std::size_t {
        const std::lock_guard lock(mu);
        std::size_t n = 0;
        for (const auto& r : requests) {
            if (r.starts_with(verb)) {
                ++n;
            }
        }
        return n;
    }

    auto clear_requests() -> void {
        const std::lock_guard lock(mu);
        requests.clear();
    }

private:
    /// ListObjectsV2 with `encoding-type=url`: keys come back percent-encoded,
    /// which is what real OSS does and what the client's decoder expects.
    auto list(const httplib::Request& req, httplib::Response& res) -> void {
        const std::string prefix = req.get_param_value("prefix");
        std::string body = "<?xml version=\"1.0\"?><ListBucketResult>";
        {
            const std::lock_guard lock(mu);
            requests.push_back("LIST " + prefix);
            for (const auto& [key, _] : store) {
                if (!key.starts_with(prefix)) {
                    continue;
                }
                std::string encoded;
                for (const char c : key) {
                    if (c == '/') {
                        encoded += "%2F";
                    } else {
                        encoded.push_back(c);
                    }
                }
                body += "<Key>" + encoded + "</Key>";
            }
        }
        body += "<IsTruncated>false</IsTruncated></ListBucketResult>";
        res.set_content(body, "application/xml");
    }
};

auto config_for(const MockOss& mock) -> kythira::alibaba_client_config {
    kythira::alibaba_client_config cfg;
    cfg.region = "cn-hangzhou";
    cfg.access_key_id = "test-ak";
    cfg.access_key_secret = "test-secret";
    cfg.endpoint_override = mock.origin();
    cfg.api_timeout = 10s;
    return cfg;
}

auto make_engine(const MockOss& mock) -> engine_t {
    return engine_t{config_for(mock), k_bucket, k_prefix};
}

auto make_entry(std::uint64_t term, std::uint64_t index,
                std::vector<std::byte> cmd = {std::byte{0xAB}}) -> log_entry_t {
    log_entry_t e;
    e._term = term;
    e._index = index;
    e._command = std::move(cmd);
    return e;
}

auto padded(std::uint64_t index) -> std::string {
    std::string digits = std::to_string(index);
    return std::string("raft/log/") + std::string(20 - digits.size(), '0') + digits;
}

#ifdef FIU_ENABLE
struct FiuInitFixture {
    FiuInitFixture() { fiu_init(0); }
};
BOOST_GLOBAL_FIXTURE(FiuInitFixture);
#endif

}  // namespace

// ── Concept ──────────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(alibaba_oss_persistence_concept)

// Requirement 16.1: the same satisfaction case persistence_concept_test.cpp
// makes for the in-tree engines. The header carries a file-scope static_assert
// too; this one keeps the failure legible in the test output.
BOOST_AUTO_TEST_CASE(engine_satisfies_the_persistence_engine_concept) {
    static_assert(kythira::persistence_engine<engine_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                              log_entry_t, snapshot_t>,
                  "alibaba_oss_persistence_engine must satisfy persistence_engine");
    BOOST_TEST(true);
}

BOOST_AUTO_TEST_SUITE_END()

// ── Construction ─────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(alibaba_oss_persistence_construction)

BOOST_AUTO_TEST_CASE(empty_bucket_or_prefix_is_rejected, *boost::unit_test::timeout(30)) {
    MockOss mock;
    BOOST_CHECK_THROW(engine_t(config_for(mock), "", k_prefix), std::invalid_argument);
    BOOST_CHECK_THROW(engine_t(config_for(mock), k_bucket, ""), std::invalid_argument);
    // A prefix of nothing but slashes normalises to empty, and is rejected the
    // same way rather than silently writing to the bucket root.
    BOOST_CHECK_THROW(engine_t(config_for(mock), k_bucket, "//"), std::invalid_argument);
}

// Requirement 15.5: a cold start reads the (empty) listing and writes nothing.
BOOST_AUTO_TEST_CASE(empty_prefix_yields_empty_state_and_writes_nothing,
                     *boost::unit_test::timeout(30)) {
    MockOss mock;
    engine_t eng = make_engine(mock);

    BOOST_TEST(eng.load_current_term() == 0U);
    BOOST_TEST(!eng.load_voted_for().has_value());
    BOOST_TEST(eng.get_last_log_index() == 0U);
    BOOST_TEST(!eng.load_snapshot().has_value());
    BOOST_TEST(!eng.get_log_entry(1).has_value());

    BOOST_TEST(mock.keys().empty());
    BOOST_TEST(mock.count_requests("PUT") == 0U);
    BOOST_TEST(mock.count_requests("LIST") == 1U);
}

// A trailing slash names the same objects as no trailing slash — otherwise a
// config typo would silently open a second, empty key space.
BOOST_AUTO_TEST_CASE(trailing_slash_in_prefix_is_normalised, *boost::unit_test::timeout(30)) {
    MockOss mock;
    {
        engine_t eng{config_for(mock), k_bucket, "raft/"};
        eng.save_current_term(11);
    }
    BOOST_TEST(mock.has("raft/term"));
    engine_t eng2 = make_engine(mock);
    BOOST_TEST(eng2.load_current_term() == 11U);
}

BOOST_AUTO_TEST_SUITE_END()

// ── Object layout (Requirement 14.1) ─────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(alibaba_oss_persistence_layout)

BOOST_AUTO_TEST_CASE(keys_have_the_documented_shape, *boost::unit_test::timeout(30)) {
    MockOss mock;
    engine_t eng = make_engine(mock);
    BOOST_TEST(eng.term_key() == "raft/term");
    BOOST_TEST(eng.voted_for_key() == "raft/voted_for");
    BOOST_TEST(eng.snapshot_key() == "raft/snapshot");
    BOOST_TEST(eng.log_key(1) == "raft/log/00000000000000000001");
}

// Design Property 5. The 9 → 10 boundary is where unpadded keys would invert:
// "10" sorts before "9". Pinned on the key text *and* on the store's ordering,
// which is the lexicographic order OSS lists in.
BOOST_AUTO_TEST_CASE(zero_padding_keeps_lexicographic_order_equal_to_numeric_order,
                     *boost::unit_test::timeout(30)) {
    MockOss mock;
    {
        engine_t eng = make_engine(mock);
        BOOST_TEST(eng.log_key(9) == "raft/log/00000000000000000009");
        BOOST_TEST(eng.log_key(10) == "raft/log/00000000000000000010");
        // The unpadded forms would compare the other way round; the padded
        // ones must not. BOOST_CHECK, not BOOST_TEST: the latter treats a
        // std::string as a collection and gives `<` per-element semantics,
        // which is not the lexicographic order OSS lists in.
        BOOST_CHECK(std::string("10") < std::string("9"));
        BOOST_CHECK(eng.log_key(9) < eng.log_key(10));
        // Every suffix is exactly 20 digits, at both ends of the range.
        BOOST_TEST(eng.log_key(0).size() == eng.log_key(18446744073709551615ULL).size());
        BOOST_TEST(eng.log_key(18446744073709551615ULL) == "raft/log/18446744073709551615");

        for (std::uint64_t i : {std::uint64_t{9}, std::uint64_t{10}, std::uint64_t{11},
                                std::uint64_t{100}, std::uint64_t{1}}) {
            eng.append_log_entry(make_entry(1, i));
        }
    }

    // The bucket's own (lexicographic) key order is numeric index order.
    std::vector<std::string> log_keys;
    for (const auto& key : mock.keys()) {
        if (key.starts_with("raft/log/")) {
            log_keys.push_back(key);
        }
    }
    const std::vector<std::string> expected{padded(1), padded(9), padded(10), padded(11),
                                            padded(100)};
    BOOST_TEST(log_keys == expected, boost::test_tools::per_element());

    // And a reload over that listing sees every entry, in order.
    engine_t eng2 = make_engine(mock);
    BOOST_TEST(eng2.get_last_log_index() == 100U);
    const auto range = eng2.get_log_entries(9, 11);
    BOOST_REQUIRE(range.size() == 3U);
    BOOST_TEST(range[0].index() == 9U);
    BOOST_TEST(range[1].index() == 10U);
    BOOST_TEST(range[2].index() == 11U);
}

BOOST_AUTO_TEST_SUITE_END()

// ── Term ─────────────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(alibaba_oss_persistence_term)

BOOST_AUTO_TEST_CASE(term_round_trip, *boost::unit_test::timeout(30)) {
    MockOss mock;
    engine_t eng = make_engine(mock);
    eng.save_current_term(42);
    BOOST_TEST(eng.load_current_term() == 42U);
    BOOST_TEST(mock.body("raft/term") == "42");
    eng.save_current_term(100);
    BOOST_TEST(eng.load_current_term() == 100U);
    BOOST_TEST(mock.body("raft/term") == "100");
}

BOOST_AUTO_TEST_CASE(term_survives_reload, *boost::unit_test::timeout(30)) {
    MockOss mock;
    {
        engine_t eng = make_engine(mock);
        eng.save_current_term(77);
    }
    engine_t eng2 = make_engine(mock);
    BOOST_TEST(eng2.load_current_term() == 77U);
}

// Reads answer from the mirror (Requirement 15.4): no GET is issued after
// construction, however many times the state is read.
BOOST_AUTO_TEST_CASE(reads_never_touch_the_network, *boost::unit_test::timeout(30)) {
    MockOss mock;
    {
        engine_t eng = make_engine(mock);
        eng.save_current_term(5);
        eng.save_voted_for(2);
        eng.append_log_entry(make_entry(5, 1));
    }
    engine_t eng2 = make_engine(mock);
    mock.clear_requests();

    for (int i = 0; i < 10; ++i) {
        BOOST_TEST(eng2.load_current_term() == 5U);
        BOOST_TEST(eng2.load_voted_for().value() == 2U);
        BOOST_TEST(eng2.get_log_entry(1).has_value());
        BOOST_TEST(eng2.get_log_entries(1, 1).size() == 1U);
        BOOST_TEST(eng2.get_last_log_index() == 1U);
        BOOST_TEST(!eng2.load_snapshot().has_value());
    }
    BOOST_TEST(mock.request_log().empty());
}

BOOST_AUTO_TEST_SUITE_END()

// ── votedFor ─────────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(alibaba_oss_persistence_voted_for)

BOOST_AUTO_TEST_CASE(voted_for_round_trip, *boost::unit_test::timeout(30)) {
    MockOss mock;
    engine_t eng = make_engine(mock);
    BOOST_TEST(!eng.load_voted_for().has_value());
    eng.save_voted_for(3);
    BOOST_REQUIRE(eng.load_voted_for().has_value());
    BOOST_TEST(eng.load_voted_for().value() == 3U);
    BOOST_TEST(mock.body("raft/voted_for") == "3");
}

BOOST_AUTO_TEST_CASE(voted_for_survives_reload, *boost::unit_test::timeout(30)) {
    MockOss mock;
    {
        engine_t eng = make_engine(mock);
        eng.save_voted_for(99);
    }
    engine_t eng2 = make_engine(mock);
    BOOST_REQUIRE(eng2.load_voted_for().has_value());
    BOOST_TEST(eng2.load_voted_for().value() == 99U);
}

// The layout's sentinel: "none" means "no vote in this term", not node 0.
BOOST_AUTO_TEST_CASE(the_none_sentinel_loads_as_no_vote, *boost::unit_test::timeout(30)) {
    MockOss mock;
    mock.put("raft/voted_for", "none");
    engine_t eng = make_engine(mock);
    BOOST_TEST(!eng.load_voted_for().has_value());
}

BOOST_AUTO_TEST_SUITE_END()

// ── Log ──────────────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(alibaba_oss_persistence_log)

BOOST_AUTO_TEST_CASE(append_is_exactly_one_put, *boost::unit_test::timeout(30)) {
    MockOss mock;
    engine_t eng = make_engine(mock);
    mock.clear_requests();
    eng.append_log_entry(make_entry(1, 1));
    BOOST_TEST(mock.count_requests("PUT") == 1U);
    BOOST_TEST(mock.has(padded(1)));
}

BOOST_AUTO_TEST_CASE(append_and_get, *boost::unit_test::timeout(30)) {
    MockOss mock;
    engine_t eng = make_engine(mock);
    eng.append_log_entry(make_entry(1, 1));
    eng.append_log_entry(make_entry(1, 2));
    eng.append_log_entry(make_entry(2, 3, {std::byte{0x01}, std::byte{0x02}}));

    BOOST_TEST(eng.get_last_log_index() == 3U);
    auto e1 = eng.get_log_entry(1);
    BOOST_REQUIRE(e1.has_value());
    BOOST_TEST(e1->term() == 1U);
    BOOST_TEST(e1->index() == 1U);
    auto e3 = eng.get_log_entry(3);
    BOOST_REQUIRE(e3.has_value());
    BOOST_TEST(e3->term() == 2U);
    BOOST_TEST(e3->command().size() == 2U);
    BOOST_TEST(!eng.get_log_entry(4).has_value());
}

BOOST_AUTO_TEST_CASE(log_survives_reload, *boost::unit_test::timeout(30)) {
    MockOss mock;
    {
        engine_t eng = make_engine(mock);
        eng.append_log_entry(make_entry(1, 1, {std::byte{0xAA}}));
        eng.append_log_entry(make_entry(2, 2, {std::byte{0xBB}}));
    }
    engine_t eng2 = make_engine(mock);
    BOOST_TEST(eng2.get_last_log_index() == 2U);
    auto e1 = eng2.get_log_entry(1);
    BOOST_REQUIRE(e1.has_value());
    BOOST_TEST(e1->term() == 1U);
    BOOST_TEST(std::to_integer<int>(e1->command()[0]) == 0xAA);
    auto e2 = eng2.get_log_entry(2);
    BOOST_REQUIRE(e2.has_value());
    BOOST_TEST(std::to_integer<int>(e2->command()[0]) == 0xBB);
}

// Requirement 14.3: byte-arbitrary commands survive the base64 codec and the
// HTTP round trip — all 256 byte values, null bytes included.
BOOST_AUTO_TEST_CASE(binary_command_survives_reload, *boost::unit_test::timeout(30)) {
    MockOss mock;
    std::vector<std::byte> all_bytes(256);
    for (int i = 0; i < 256; ++i) {
        all_bytes[static_cast<std::size_t>(i)] = static_cast<std::byte>(i);
    }
    {
        engine_t eng = make_engine(mock);
        eng.append_log_entry(make_entry(1, 1, all_bytes));
    }
    engine_t eng2 = make_engine(mock);
    auto e = eng2.get_log_entry(1);
    BOOST_REQUIRE(e.has_value());
    BOOST_CHECK(e->command() == all_bytes);
}

// The same shape written as a string literal, because that is how a real
// command arrives. Split literal deliberately: a hex escape is greedy, so
// "\xffbinary" would parse as one out-of-range escape — clang++-18 (what CI
// builds with) rejects it outright.
BOOST_AUTO_TEST_CASE(embedded_nulls_and_high_bytes_survive, *boost::unit_test::timeout(30)) {
    const std::string payload(
        "\x00\x01\xff"
        "binary\x00",
        11);
    std::vector<std::byte> cmd;
    cmd.reserve(payload.size());
    for (const char c : payload) {
        cmd.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
    }

    MockOss mock;
    {
        engine_t eng = make_engine(mock);
        eng.append_log_entry(make_entry(3, 7, cmd));
    }
    engine_t eng2 = make_engine(mock);
    auto e = eng2.get_log_entry(7);
    BOOST_REQUIRE(e.has_value());
    BOOST_CHECK(e->command() == cmd);
}

BOOST_AUTO_TEST_CASE(entry_type_survives_reload, *boost::unit_test::timeout(30)) {
    MockOss mock;
    {
        engine_t eng = make_engine(mock);
        log_entry_t e = make_entry(1, 1);
        e._type = kythira::entry_type::configuration;
        eng.append_log_entry(e);
        log_entry_t n = make_entry(1, 2);
        n._type = kythira::entry_type::no_op;
        eng.append_log_entry(n);
    }
    engine_t eng2 = make_engine(mock);
    BOOST_TEST((eng2.get_log_entry(1)->type() == kythira::entry_type::configuration));
    BOOST_TEST((eng2.get_log_entry(2)->type() == kythira::entry_type::no_op));
}

BOOST_AUTO_TEST_CASE(log_entries_range, *boost::unit_test::timeout(30)) {
    MockOss mock;
    engine_t eng = make_engine(mock);
    for (std::uint64_t i = 1; i <= 5; ++i) {
        eng.append_log_entry(make_entry(1, i, {static_cast<std::byte>(i)}));
    }
    auto r = eng.get_log_entries(2, 4);
    BOOST_REQUIRE(r.size() == 3U);
    BOOST_TEST(r[0].index() == 2U);
    BOOST_TEST(r[1].index() == 3U);
    BOOST_TEST(r[2].index() == 4U);
}

// A bounded batch of DELETEs — one per affected object and no others, which is
// the whole point of the one-object-per-entry layout (Requirement 14.2).
BOOST_AUTO_TEST_CASE(truncate_deletes_exactly_the_affected_objects,
                     *boost::unit_test::timeout(30)) {
    MockOss mock;
    engine_t eng = make_engine(mock);
    for (std::uint64_t i = 1; i <= 5; ++i) {
        eng.append_log_entry(make_entry(1, i));
    }
    mock.clear_requests();
    eng.truncate_log(3);

    BOOST_TEST(mock.count_requests("DELETE") == 3U);
    BOOST_TEST(mock.count_requests("PUT") == 0U);
    BOOST_TEST(eng.get_last_log_index() == 2U);
    BOOST_TEST(!eng.get_log_entry(3).has_value());
    BOOST_TEST(!eng.get_log_entry(5).has_value());
    BOOST_TEST(eng.get_log_entry(1).has_value());
    BOOST_TEST(mock.has(padded(2)));
    BOOST_TEST(!mock.has(padded(3)));
    BOOST_TEST(!mock.has(padded(5)));
}

BOOST_AUTO_TEST_CASE(truncate_survives_reload, *boost::unit_test::timeout(30)) {
    MockOss mock;
    {
        engine_t eng = make_engine(mock);
        for (std::uint64_t i = 1; i <= 5; ++i) {
            eng.append_log_entry(make_entry(1, i));
        }
        eng.truncate_log(3);
    }
    engine_t eng2 = make_engine(mock);
    BOOST_TEST(eng2.get_last_log_index() == 2U);
    BOOST_TEST(!eng2.get_log_entry(3).has_value());
    BOOST_TEST(eng2.get_log_entry(2).has_value());
}

BOOST_AUTO_TEST_CASE(delete_log_entries_before, *boost::unit_test::timeout(30)) {
    MockOss mock;
    engine_t eng = make_engine(mock);
    for (std::uint64_t i = 1; i <= 5; ++i) {
        eng.append_log_entry(make_entry(1, i));
    }
    mock.clear_requests();
    eng.delete_log_entries_before(3);

    BOOST_TEST(mock.count_requests("DELETE") == 2U);
    BOOST_TEST(!eng.get_log_entry(1).has_value());
    BOOST_TEST(!eng.get_log_entry(2).has_value());
    BOOST_TEST(eng.get_log_entry(3).has_value());
    BOOST_TEST(eng.get_last_log_index() == 5U);
    BOOST_TEST(!mock.has(padded(1)));
    BOOST_TEST(mock.has(padded(3)));
}

BOOST_AUTO_TEST_CASE(delete_before_survives_reload, *boost::unit_test::timeout(30)) {
    MockOss mock;
    {
        engine_t eng = make_engine(mock);
        for (std::uint64_t i = 1; i <= 5; ++i) {
            eng.append_log_entry(make_entry(1, i));
        }
        eng.delete_log_entries_before(4);
    }
    engine_t eng2 = make_engine(mock);
    BOOST_TEST(!eng2.get_log_entry(1).has_value());
    BOOST_TEST(!eng2.get_log_entry(3).has_value());
    BOOST_TEST(eng2.get_log_entry(4).has_value());
    BOOST_TEST(eng2.get_log_entry(5).has_value());
}

BOOST_AUTO_TEST_SUITE_END()

// ── Snapshot ─────────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(alibaba_oss_persistence_snapshot)

BOOST_AUTO_TEST_CASE(snapshot_round_trip, *boost::unit_test::timeout(30)) {
    MockOss mock;
    engine_t eng = make_engine(mock);
    BOOST_TEST(!eng.load_snapshot().has_value());

    const kythira::cluster_configuration<> cfg{{1, 2, 3}, false, std::nullopt, {}};
    snapshot_t snap;
    snap._last_included_index = 10;
    snap._last_included_term = 3;
    snap._configuration = cfg;
    snap._state_machine_state = {std::byte{0xCA}, std::byte{0xFE}, std::byte{0x00},
                                 std::byte{0xFF}};
    eng.save_snapshot(snap);

    auto loaded = eng.load_snapshot();
    BOOST_REQUIRE(loaded.has_value());
    BOOST_TEST(loaded->last_included_index() == 10U);
    BOOST_TEST(loaded->last_included_term() == 3U);
    BOOST_CHECK(loaded->state_machine_state() == snap._state_machine_state);
    BOOST_TEST(loaded->configuration().nodes() == cfg.nodes(), boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(snapshot_survives_reload, *boost::unit_test::timeout(30)) {
    MockOss mock;
    {
        engine_t eng = make_engine(mock);
        const kythira::cluster_configuration<> cfg{{1, 2}, false, std::nullopt, {}};
        snapshot_t snap;
        snap._last_included_index = 20;
        snap._last_included_term = 5;
        snap._configuration = cfg;
        snap._state_machine_state = {std::byte{0x42}};
        eng.save_snapshot(snap);
    }
    engine_t eng2 = make_engine(mock);
    auto loaded = eng2.load_snapshot();
    BOOST_REQUIRE(loaded.has_value());
    BOOST_TEST(loaded->last_included_index() == 20U);
    BOOST_TEST(loaded->last_included_term() == 5U);
    BOOST_REQUIRE(loaded->state_machine_state().size() == 1U);
    BOOST_TEST(std::to_integer<int>(loaded->state_machine_state()[0]) == 0x42);
    BOOST_TEST(loaded->configuration().nodes().size() == 2U);
}

// Requirement 15.7: one slot, overwritten in place — not an accumulating
// generation of snapshot objects. Retention belongs to the cross-provider spec.
BOOST_AUTO_TEST_CASE(snapshot_overwrites_the_single_slot, *boost::unit_test::timeout(30)) {
    MockOss mock;
    engine_t eng = make_engine(mock);
    const kythira::cluster_configuration<> cfg{{1}, false, std::nullopt, {}};
    snapshot_t s1;
    s1._last_included_index = 5;
    s1._last_included_term = 1;
    s1._configuration = cfg;
    s1._state_machine_state = {std::byte{0x01}};
    snapshot_t s2;
    s2._last_included_index = 10;
    s2._last_included_term = 2;
    s2._configuration = cfg;
    s2._state_machine_state = {std::byte{0x02}};

    eng.save_snapshot(s1);
    eng.save_snapshot(s2);

    std::size_t snapshot_objects = 0;
    for (const auto& key : mock.keys()) {
        if (key.starts_with("raft/snapshot")) {
            ++snapshot_objects;
        }
    }
    BOOST_TEST(snapshot_objects == 1U);

    auto loaded = eng.load_snapshot();
    BOOST_REQUIRE(loaded.has_value());
    BOOST_TEST(loaded->last_included_index() == 10U);
    BOOST_TEST(std::to_integer<int>(loaded->state_machine_state()[0]) == 0x02);

    // …and the surviving object is the second one, not the first.
    engine_t eng2 = make_engine(mock);
    BOOST_TEST(eng2.load_snapshot()->last_included_index() == 10U);
}

BOOST_AUTO_TEST_CASE(joint_consensus_configuration_survives_reload,
                     *boost::unit_test::timeout(30)) {
    MockOss mock;
    {
        engine_t eng = make_engine(mock);
        snapshot_t snap;
        snap._last_included_index = 1;
        snap._last_included_term = 1;
        snap._configuration =
            kythira::cluster_configuration<>{{1, 2, 3}, true, std::vector<std::uint64_t>{1, 2}, {}};
        eng.save_snapshot(snap);
    }
    engine_t eng2 = make_engine(mock);
    auto loaded = eng2.load_snapshot();
    BOOST_REQUIRE(loaded.has_value());
    BOOST_TEST(loaded->configuration().is_joint_consensus());
    BOOST_REQUIRE(loaded->configuration().old_nodes().has_value());
    BOOST_TEST(loaded->configuration().old_nodes()->size() == 2U);
}

BOOST_AUTO_TEST_SUITE_END()

// ── Durability (Requirement 15.2, design Property 1) ─────────────────────────

BOOST_AUTO_TEST_SUITE(alibaba_oss_persistence_durability)

// The write is on the calling thread's stack: when `save_current_term` returns,
// the object is already in the store and the server has already logged the PUT.
// There is no buffered or background path that could make this true only
// eventually.
BOOST_AUTO_TEST_CASE(a_returned_write_has_already_reached_the_store,
                     *boost::unit_test::timeout(30)) {
    MockOss mock;
    engine_t eng = make_engine(mock);
    mock.clear_requests();

    eng.save_current_term(7);
    BOOST_TEST(mock.body("raft/term") == "7");
    const auto after_term = mock.request_log();
    BOOST_REQUIRE(after_term.size() == 1U);
    BOOST_TEST(after_term.back() == "PUT raft/term");

    eng.save_voted_for(4);
    BOOST_TEST(mock.body("raft/voted_for") == "4");
    BOOST_TEST(mock.request_log().back() == "PUT raft/voted_for");

    eng.append_log_entry(make_entry(7, 1));
    BOOST_TEST(mock.has(padded(1)));
    BOOST_TEST(mock.request_log().back() == "PUT " + padded(1));
}

// The same property from the caller's side: a slow server makes the *method*
// slow, which is only possible if the method waits for the acknowledgement.
// This is the observable difference between "synchronous" and "queued".
BOOST_AUTO_TEST_CASE(the_caller_waits_for_the_acknowledgement, *boost::unit_test::timeout(30)) {
    MockOss mock;
    engine_t eng = make_engine(mock);
    mock.put_delay_ms.store(150);

    const auto started = std::chrono::steady_clock::now();
    eng.save_current_term(9);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - started)
                             .count();

    BOOST_TEST(elapsed >= 150);
    BOOST_TEST(mock.body("raft/term") == "9");
    mock.put_delay_ms.store(0);
}

// Design Property 1's named case: a non-2xx must never be treated as success.
// The mirror is updated only after the PUT is acknowledged, so a failed write
// leaves memory exactly equal to the store — and a reload agrees with both.
BOOST_AUTO_TEST_CASE(a_failed_put_throws_and_leaves_the_mirror_unchanged,
                     *boost::unit_test::timeout(30)) {
    MockOss mock;
    engine_t eng = make_engine(mock);
    eng.save_current_term(3);
    eng.append_log_entry(make_entry(3, 1));

    mock.put_status.store(500);
    mock.clear_requests();

    BOOST_CHECK_THROW(eng.save_current_term(4), std::runtime_error);
    BOOST_TEST(eng.load_current_term() == 3U);
    BOOST_TEST(mock.body("raft/term") == "3");

    BOOST_CHECK_THROW(eng.save_voted_for(1), std::runtime_error);
    BOOST_TEST(!eng.load_voted_for().has_value());
    BOOST_TEST(!mock.has("raft/voted_for"));

    BOOST_CHECK_THROW(eng.append_log_entry(make_entry(4, 2)), std::runtime_error);
    BOOST_TEST(eng.get_last_log_index() == 1U);
    BOOST_TEST(!mock.has(padded(2)));

    snapshot_t snap;
    snap._last_included_index = 1;
    snap._last_included_term = 3;
    snap._configuration = kythira::cluster_configuration<>{{1}, false, std::nullopt, {}};
    BOOST_CHECK_THROW(eng.save_snapshot(snap), std::runtime_error);
    BOOST_TEST(!eng.load_snapshot().has_value());

    // Requirement 15.3: at most one retry per failed PUT — four failed calls,
    // eight attempts, no more.
    BOOST_TEST(mock.count_requests("PUT") == 8U);

    mock.put_status.store(0);
    engine_t eng2 = make_engine(mock);
    BOOST_TEST(eng2.load_current_term() == 3U);
    BOOST_TEST(eng2.get_last_log_index() == 1U);
}

// The retry is what makes a transient failure survivable: the second attempt
// writes the identical bytes to the identical key, so a PUT that fails once
// still lands.
BOOST_AUTO_TEST_CASE(a_transient_put_failure_is_retried_once_and_succeeds,
                     *boost::unit_test::timeout(30)) {
    MockOss mock;
    engine_t eng = make_engine(mock);
    mock.clear_requests();

    // Fail the first attempt only.
    mock.fail_next_puts.store(1);
    eng.save_current_term(21);

    BOOST_TEST(mock.fail_next_puts.load() == 0);
    BOOST_TEST(eng.load_current_term() == 21U);
    BOOST_TEST(mock.body("raft/term") == "21");
    BOOST_TEST(mock.count_requests("PUT") == 2U);
}

BOOST_AUTO_TEST_SUITE_END()

// ── Corruption is fatal (Requirement 15.6) ───────────────────────────────────

BOOST_AUTO_TEST_SUITE(alibaba_oss_persistence_corruption)

BOOST_AUTO_TEST_CASE(a_corrupt_term_object_fails_construction_by_name,
                     *boost::unit_test::timeout(30)) {
    MockOss mock;
    mock.put("raft/term", "not-a-number");
    try {
        engine_t eng = make_engine(mock);
        BOOST_FAIL("construction must not survive a corrupt term object");
    } catch (const std::runtime_error& e) {
        const std::string what = e.what();
        BOOST_TEST(what.find("raft/term") != std::string::npos);
    }
}

// `std::stoull` would read 12 out of this and start with a plausible term —
// the exact silent-loss failure 15.6 exists to prevent.
BOOST_AUTO_TEST_CASE(a_partially_numeric_term_is_corruption_not_a_prefix,
                     *boost::unit_test::timeout(30)) {
    MockOss mock;
    mock.put("raft/term", "12garbage");
    BOOST_CHECK_THROW(make_engine(mock), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(a_corrupt_voted_for_object_fails_construction_by_name,
                     *boost::unit_test::timeout(30)) {
    MockOss mock;
    mock.put("raft/voted_for", "node-seven");
    try {
        engine_t eng = make_engine(mock);
        BOOST_FAIL("construction must not survive a corrupt voted_for object");
    } catch (const std::runtime_error& e) {
        BOOST_TEST(std::string(e.what()).find("raft/voted_for") != std::string::npos);
    }
}

BOOST_AUTO_TEST_CASE(a_truncated_log_record_fails_construction_by_name,
                     *boost::unit_test::timeout(30)) {
    MockOss mock;
    {
        engine_t eng = make_engine(mock);
        eng.append_log_entry(make_entry(1, 1));
        eng.append_log_entry(make_entry(1, 2));
    }
    // A half-uploaded object: valid prefix, no closing brace.
    mock.put(padded(2), "{\"term\":1,\"index\":2,\"comm");
    try {
        engine_t eng2 = make_engine(mock);
        BOOST_FAIL("construction must not survive a truncated log record");
    } catch (const std::runtime_error& e) {
        BOOST_TEST(std::string(e.what()).find(padded(2)) != std::string::npos);
    }
}

// A record whose index disagrees with the key it is stored under means the
// log's ordering guarantee no longer holds.
BOOST_AUTO_TEST_CASE(a_log_record_disagreeing_with_its_key_fails_construction,
                     *boost::unit_test::timeout(30)) {
    MockOss mock;
    mock.put(padded(4), "{\"term\":1,\"index\":9,\"command\":\"\",\"type\":0}");
    try {
        engine_t eng = make_engine(mock);
        BOOST_FAIL("construction must not survive a mislabelled log record");
    } catch (const std::runtime_error& e) {
        BOOST_TEST(std::string(e.what()).find(padded(4)) != std::string::npos);
    }
}

// An unpadded key cannot be ordered against the padded ones, so it is
// corruption rather than a key to skip (design Property 5's inverse).
BOOST_AUTO_TEST_CASE(an_unpadded_log_key_fails_construction, *boost::unit_test::timeout(30)) {
    MockOss mock;
    mock.put("raft/log/7", "{\"term\":1,\"index\":7,\"command\":\"\",\"type\":0}");
    BOOST_CHECK_THROW(make_engine(mock), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(a_corrupt_snapshot_fails_construction_by_name,
                     *boost::unit_test::timeout(30)) {
    MockOss mock;
    mock.put("raft/snapshot", "{\"last_included_index\": \"ten\"}");
    try {
        engine_t eng = make_engine(mock);
        BOOST_FAIL("construction must not survive a corrupt snapshot");
    } catch (const std::runtime_error& e) {
        BOOST_TEST(std::string(e.what()).find("raft/snapshot") != std::string::npos);
    }
}

// A command that is not valid base64 would decode to *some* byte string under
// the file engine's skip-invalid-characters codec; here it is corruption.
BOOST_AUTO_TEST_CASE(a_non_base64_command_fails_construction, *boost::unit_test::timeout(30)) {
    MockOss mock;
    mock.put(padded(1), "{\"term\":1,\"index\":1,\"command\":\"!!not base64!!\",\"type\":0}");
    BOOST_CHECK_THROW(make_engine(mock), std::runtime_error);
}

// Objects that are not part of this format are none of the engine's business —
// they are neither parsed nor rejected. Only keys the layout defines must parse.
BOOST_AUTO_TEST_CASE(a_foreign_object_under_the_prefix_is_ignored, *boost::unit_test::timeout(30)) {
    MockOss mock;
    mock.put("raft/README", "left here by an operator");
    engine_t eng = make_engine(mock);
    BOOST_TEST(eng.load_current_term() == 0U);
    BOOST_TEST(mock.has("raft/README"));
}

BOOST_AUTO_TEST_SUITE_END()

// ── Fault points (Requirement 15.9) ──────────────────────────────────────────

#ifdef FIU_ENABLE

BOOST_AUTO_TEST_SUITE(alibaba_oss_persistence_fault_injection)

BOOST_AUTO_TEST_CASE(the_put_object_fault_fails_the_write_without_touching_the_store,
                     *boost::unit_test::timeout(30)) {
    MockOss mock;
    engine_t eng = make_engine(mock);
    eng.save_current_term(2);
    mock.clear_requests();

    fiu_enable("raft/alibaba/oss/put_object", 1, nullptr, 0);
    BOOST_CHECK_THROW(eng.save_current_term(3), std::runtime_error);
    fiu_disable("raft/alibaba/oss/put_object");

    // The fault fires before the client call, so nothing reached the bucket and
    // the mirror still agrees with it.
    BOOST_TEST(mock.count_requests("PUT") == 0U);
    BOOST_TEST(eng.load_current_term() == 2U);
    BOOST_TEST(mock.body("raft/term") == "2");
}

BOOST_AUTO_TEST_CASE(the_delete_object_fault_fails_truncation, *boost::unit_test::timeout(30)) {
    MockOss mock;
    engine_t eng = make_engine(mock);
    eng.append_log_entry(make_entry(1, 1));
    eng.append_log_entry(make_entry(1, 2));
    mock.clear_requests();

    fiu_enable("raft/alibaba/oss/delete_object", 1, nullptr, 0);
    BOOST_CHECK_THROW(eng.truncate_log(1), std::runtime_error);
    fiu_disable("raft/alibaba/oss/delete_object");

    BOOST_TEST(mock.count_requests("DELETE") == 0U);
    BOOST_TEST(eng.get_last_log_index() == 2U);
    BOOST_TEST(mock.has(padded(1)));
}

BOOST_AUTO_TEST_CASE(the_get_object_fault_fails_construction, *boost::unit_test::timeout(30)) {
    MockOss mock;
    {
        engine_t eng = make_engine(mock);
        eng.save_current_term(5);
    }

    fiu_enable("raft/alibaba/oss/get_object", 1, nullptr, 0);
    BOOST_CHECK_THROW(make_engine(mock), std::runtime_error);
    fiu_disable("raft/alibaba/oss/get_object");
}

BOOST_AUTO_TEST_CASE(the_list_keys_fault_fails_construction, *boost::unit_test::timeout(30)) {
    MockOss mock;
    fiu_enable("raft/alibaba/oss/list_keys", 1, nullptr, 0);
    BOOST_CHECK_THROW(make_engine(mock), std::runtime_error);
    fiu_disable("raft/alibaba/oss/list_keys");

    // …and the engine builds again once the fault is gone, proving the failure
    // came from the fault and not from the fixture.
    BOOST_CHECK_NO_THROW(make_engine(mock));
}

BOOST_AUTO_TEST_SUITE_END()

#endif  // FIU_ENABLE
