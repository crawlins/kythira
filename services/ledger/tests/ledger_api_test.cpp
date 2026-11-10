// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// The ledger catalog and the native API (.kiro/specs/verifiable-ledger/
// task 7, Requirements 2.3, 2.4, 8.1, 8.2), over an in-process backend
// that applies entries directly: ledger lifecycle and deletion
// protection, roles, one-shot and session transactions with OCC
// conflicts, reads, history, proofs that verify offline, exports, Ion and
// JSON bodies, and the error shapes.

#define BOOST_TEST_MODULE ledger_api_test
#include <boost/test/unit_test.hpp>

#include <kythira/ledger/ion_value.hpp>
#include <kythira/ledger/journal_types.hpp>
#include <kythira/ledger/journal_verifier.hpp>
#include <kythira/ledger/ledger_api.hpp>
#include <kythira/ledger/ledger_catalog.hpp>
#include <kythira/ledger/ledger_sessions.hpp>
#include <kythira/ledger/ledger_state_machine.hpp>

#include <raft/memory_kv_engine.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

using namespace kythira::ledger;
using kythira::memory_kv_engine;

using ledger_sm = basic_ledger_state_machine<memory_kv_engine>;
using catalog_sm = basic_ledger_catalog<memory_kv_engine>;

namespace {

constexpr std::int64_t t0 = 1'762'689'600'000;

/// Every group in one engine, entries applied at once.
struct local_backend {
    struct hosted {
        ledger_sm sm;
        std::unique_ptr<ledger_session_manager<ledger_sm>> sessions;
        std::mutex commit_mutex;
        std::uint64_t index = 0;
    };

    memory_kv_engine engine;
    catalog_sm cat;
    std::uint64_t catalog_index = 0;
    std::map<std::string, std::unique_ptr<hosted>> ledgers;
    std::int64_t clock = t0;
    bool leader = true;
    std::vector<export_job> started_exports;

    local_backend() { cat.open(engine, catalog_range()); }

    [[nodiscard]] auto catalog() const -> const catalog_sm& { return cat; }

    auto propose_catalog(std::vector<std::byte> entry) -> std::vector<std::byte> {
        if (!leader) {
            throw ledger_not_leader(std::string("https://leader:8443"));
        }
        return cat.apply(entry, ++catalog_index);
    }

    auto open(const ledger_record& r) -> std::optional<ledger_handle<ledger_sm>> {
        if (!leader) {
            return std::nullopt;
        }
        auto& slot = ledgers[r.ledger_id];
        if (!slot) {
            slot = std::make_unique<hosted>();
            slot->sm.open(engine, ledger_range(r.ledger_id));
            slot->sessions = std::make_unique<ledger_session_manager<ledger_sm>>(slot->sm);
        }
        hosted* h = slot.get();
        ledger_handle<ledger_sm> out;
        out.sm = &h->sm;
        out.sessions = h->sessions.get();
        out.commit_mutex = &h->commit_mutex;
        out.propose = [h](std::vector<std::byte> cmd) { return h->sm.apply(cmd, ++h->index); };
        out.start_export = [this](const export_job& j) { started_exports.push_back(j); };
        return out;
    }

    [[nodiscard]] auto term() const -> std::uint64_t { return 1; }
    auto now_ms() -> std::int64_t { return clock += 10; }
};

using api_t = ledger_api<local_backend, ledger_sm>;

struct fixture {
    local_backend backend;
    api_t api{backend, ledger_roles{}
                           .grant("CN=admin", "*", ledger_role::admin)
                           .grant("CN=app", "bank", ledger_role::write)
                           .grant("CN=auditor", "bank", ledger_role::read)};

    auto call(std::string method, std::string path, std::string body = "",
              std::string who = "CN=admin", std::map<std::string, std::string> query = {})
        -> api_response {
        api_request req;
        req.method = std::move(method);
        req.path = std::move(path);
        req.body = std::move(body);
        req.principal = std::move(who);
        req.query = std::move(query);
        return api.handle(req);
    }

    static auto json(const api_response& r) -> ion_value {
        return ion_value::parse(std::string_view(r.body));
    }

    auto make_bank() -> void {
        BOOST_REQUIRE_EQUAL(
            call("POST", "/v1/ledgers", R"({"name": "bank", "deletionProtection": false})").status,
            201);
        const auto r = call("POST", "/v1/ledgers/bank/execute",
                            R"({"statements": [{"op": "create_table", "table": "acct"}]})");
        BOOST_REQUIRE_EQUAL(r.status, 200);
    }
};

auto text(const ion_value& v, std::string_view field) -> std::string {
    const ion_value* f = v.find(field);
    return f != nullptr ? f->as_text().value_or("") : "";
}

}  // namespace

BOOST_AUTO_TEST_SUITE(catalog)

BOOST_AUTO_TEST_CASE(a_ledger_is_created_protected_deleted_and_purged) {
    fixture f;
    auto r = f.call("POST", "/v1/ledgers", R"({"name": "bank"})");
    BOOST_TEST(r.status == 201);
    const auto created = fixture::json(r);
    BOOST_TEST(text(created, "state") == "active");
    BOOST_TEST(valid_id(text(created, "ledgerId")));
    BOOST_TEST(created.find("deletionProtection")->as_bool().value_or(false));

    BOOST_TEST(f.call("POST", "/v1/ledgers", R"({"name": "bank"})").status == 409);
    BOOST_TEST(f.call("POST", "/v1/ledgers", R"({"name": "9lives"})").status == 400);

    r = f.call("DELETE", "/v1/ledgers/bank");
    BOOST_TEST(r.status == 409);
    BOOST_TEST(text(fixture::json(r), "code") == "deletion_protected");

    BOOST_TEST(f.call("PATCH", "/v1/ledgers/bank", R"({"deletionProtection": false})").status ==
               200);
    r = f.call("DELETE", "/v1/ledgers/bank");
    BOOST_TEST(r.status == 200);
    BOOST_TEST(text(fixture::json(r), "state") == "deleted");

    // Deleted: kept, listed, but closed to data routes and to a new
    // ledger of the same name until purged.
    BOOST_TEST(f.call("GET", "/v1/ledgers/bank").status == 200);
    BOOST_TEST(f.call("GET", "/v1/ledgers/bank/digest").status == 409);
    BOOST_TEST(f.call("POST", "/v1/ledgers", R"({"name": "bank"})").status == 409);

    BOOST_TEST(
        catalog_result::decode(f.backend.propose_catalog(catalog_entries::purge("bank"))).ok());
    BOOST_TEST(f.call("GET", "/v1/ledgers/bank").status == 404);
    BOOST_TEST(f.call("POST", "/v1/ledgers", R"({"name": "bank"})").status == 201);
}

BOOST_AUTO_TEST_CASE(catalog_entries_are_checked) {
    memory_kv_engine engine;
    catalog_sm cat;
    cat.open(engine, catalog_range());
    auto r = catalog_result::decode(cat.apply(catalog_entries::create("ok", "nope", true, t0), 1));
    BOOST_TEST(r.code == "invalid_entry");
    r = catalog_result::decode(cat.apply(catalog_entries::purge("ok"), 2));
    BOOST_TEST(r.code == "ledger_not_found");
    r = catalog_result::decode(cat.apply(catalog_entries::create("ok", new_id(), true, t0), 3));
    BOOST_TEST(r.ok());
    r = catalog_result::decode(cat.apply(catalog_entries::purge("ok"), 4));
    BOOST_TEST(r.code == "ledger_active");
    BOOST_TEST(cat.apply(catalog_entries::purge("ok"), 4).empty());  // replay
    BOOST_TEST(cat.applied_index() == 4U);
    BOOST_TEST(cat.list().size() == 1U);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(roles)

BOOST_AUTO_TEST_CASE(each_route_checks_its_role) {
    fixture f;
    f.make_bank();
    const std::string st = R"({"statements": [{"op": "insert", "table": "acct",
        "id": "aaaaaaaaaaaaaaaaaaaaaa", "value": {"n": 1}}]})";
    BOOST_TEST(f.call("POST", "/v1/ledgers/bank/execute", st, "CN=auditor").status == 403);
    BOOST_TEST(f.call("POST", "/v1/ledgers/bank/execute", st, "CN=app").status == 200);
    BOOST_TEST(f.call("GET", "/v1/ledgers/bank/digest", "", "CN=auditor").status == 200);
    BOOST_TEST(f.call("GET", "/v1/ledgers/bank/digest", "", "CN=stranger").status == 403);
    BOOST_TEST(f.call("DELETE", "/v1/ledgers/bank", "", "CN=app").status == 403);
    BOOST_TEST(f.call("POST", "/v1/ledgers", R"({"name": "x"})", "CN=app").status == 403);

    // The list shows only ledgers the caller can read.
    BOOST_TEST(f.call("POST", "/v1/ledgers", R"({"name": "other"})").status == 201);
    const auto mine = fixture::json(f.call("GET", "/v1/ledgers", "", "CN=auditor"));
    BOOST_TEST(mine.find("ledgers")->items().size() == 1U);
    BOOST_TEST(fixture::json(f.call("GET", "/v1/ledgers")).find("ledgers")->items().size() == 2U);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(transactions)

BOOST_AUTO_TEST_CASE(a_session_reads_writes_and_commits) {
    fixture f;
    f.make_bank();
    auto r = f.call("POST", "/v1/ledgers/bank/sessions", "", "CN=app");
    BOOST_REQUIRE_EQUAL(r.status, 201);
    const auto token = text(fixture::json(r), "token");

    r = f.call("POST", "/v1/ledgers/bank/sessions/" + token + "/insert",
               R"({"table": "acct", "value": {"owner": "ann", "balance": 10}})", "CN=app");
    BOOST_REQUIRE_EQUAL(r.status, 200);
    const auto id = text(fixture::json(r), "id");
    BOOST_TEST(valid_id(id));

    r = f.call("POST", "/v1/ledgers/bank/sessions/" + token + "/commit", "", "CN=app");
    BOOST_REQUIRE_EQUAL(r.status, 200);
    BOOST_TEST(text(fixture::json(r), "status") == "committed");

    r = f.call("GET", "/v1/ledgers/bank/tables/acct/documents/" + id, "", "CN=auditor");
    BOOST_REQUIRE_EQUAL(r.status, 200);
    BOOST_TEST(fixture::json(r).find("data")->find("balance")->as_int64().value_or(-1) == 10);

    // A read-modify-write that loses to another writer conflicts, and says
    // to retry.
    const auto t1 =
        text(fixture::json(f.call("POST", "/v1/ledgers/bank/sessions", "", "CN=app")), "token");
    r = f.call("POST", "/v1/ledgers/bank/sessions/" + t1 + "/get",
               R"({"table": "acct", "id": ")" + id + R"("})", "CN=app");
    BOOST_TEST(fixture::json(r).find("document") != nullptr);
    BOOST_REQUIRE_EQUAL(f.call("POST", "/v1/ledgers/bank/execute",
                               R"({"statements": [{"op": "replace", "table": "acct", "id": ")" +
                                   id + R"(", "value": {"owner": "ann", "balance": 99}}]})",
                               "CN=app")
                            .status,
                        200);
    r = f.call("POST", "/v1/ledgers/bank/sessions/" + t1 + "/statements",
               R"({"statements": [{"op": "replace", "table": "acct", "id": ")" + id +
                   R"(", "value": {"owner": "ann", "balance": 11}}]})",
               "CN=app");
    BOOST_TEST(r.status == 200);
    r = f.call("POST", "/v1/ledgers/bank/sessions/" + t1 + "/commit", "", "CN=app");
    BOOST_TEST(r.status == 409);
    BOOST_TEST(text(fixture::json(r), "code") == "occ_conflict");
    BOOST_TEST(fixture::json(r).find("retryable")->as_bool().value_or(false));

    // The session is gone either way.
    r = f.call("POST", "/v1/ledgers/bank/sessions/" + t1 + "/commit", "", "CN=app");
    BOOST_TEST(r.status == 409);
    BOOST_TEST(text(fixture::json(r), "code") == "session_expired");

    // Aborting, and an empty commit.
    const auto t2 =
        text(fixture::json(f.call("POST", "/v1/ledgers/bank/sessions", "", "CN=app")), "token");
    BOOST_TEST(f.call("POST", "/v1/ledgers/bank/sessions/" + t2 + "/commit", "", "CN=app").status ==
               400);
    const auto t3 =
        text(fixture::json(f.call("POST", "/v1/ledgers/bank/sessions", "", "CN=app")), "token");
    BOOST_TEST(f.call("DELETE", "/v1/ledgers/bank/sessions/" + t3, "", "CN=app").status == 200);
}

BOOST_AUTO_TEST_CASE(rejections_carry_their_codes_and_statement) {
    fixture f;
    f.make_bank();
    auto r = f.call("POST", "/v1/ledgers/bank/execute",
                    R"({"statements": [{"op": "create_table", "table": "acct"}]})");
    BOOST_TEST(r.status == 409);
    BOOST_TEST(text(fixture::json(r), "code") == "table_exists");
    BOOST_TEST(fixture::json(r).find("statement")->as_int64().value_or(-1) == 0);

    r = f.call("POST", "/v1/ledgers/bank/execute", R"({"statements": [{"op": "frob"}]})");
    BOOST_TEST(r.status == 400);
    r = f.call("POST", "/v1/ledgers/bank/execute", R"({"statements": []})");
    BOOST_TEST(r.status == 400);
    r = f.call("POST", "/v1/ledgers/bank/execute", "not json at all {");
    BOOST_TEST(r.status == 400);
    BOOST_TEST(f.call("GET", "/v1/ledgers/nope/digest").status == 404);
    BOOST_TEST(f.call("GET", "/v2/whatever").status == 404);
    BOOST_TEST(f.call("PUT", "/v1/ledgers").status == 405);
}

BOOST_AUTO_TEST_CASE(a_follower_answers_not_leader_with_the_leader) {
    fixture f;
    f.make_bank();
    f.backend.leader = false;
    auto r = f.call("GET", "/v1/ledgers/bank/digest");
    BOOST_TEST(r.status == 503);
    r = f.call("POST", "/v1/ledgers", R"({"name": "x"})");
    BOOST_TEST(r.status == 503);
    BOOST_TEST(text(fixture::json(r), "leader") == "https://leader:8443");
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(reads_and_proofs)

BOOST_AUTO_TEST_CASE(history_digest_and_proofs_verify_offline) {
    fixture f;
    f.make_bank();
    const std::string id = "bbbbbbbbbbbbbbbbbbbbbb";
    for (int i = 0; i < 4; ++i) {
        const std::string op = i == 0 ? "insert" : "replace";
        BOOST_REQUIRE_EQUAL(
            f.call("POST", "/v1/ledgers/bank/execute",
                   R"({"statements": [{"op": ")" + op + R"(", "table": "acct", "id": ")" + id +
                       R"(", "value": {"n": )" + std::to_string(i) + "}}]}")
                .status,
            200);
    }
    auto r = f.call("GET", "/v1/ledgers/bank/history/acct/" + id, "", "CN=auditor");
    BOOST_REQUIRE_EQUAL(r.status, 200);
    BOOST_TEST(fixture::json(r).find("revisions")->items().size() == 4U);
    r = f.call("GET", "/v1/ledgers/bank/history/acct", "", "CN=auditor", {{"limit", "2"}});
    BOOST_TEST(fixture::json(r).find("revisions")->items().size() == 2U);
    BOOST_TEST(fixture::json(r).find("next") != nullptr);

    // Ion responses re-hash: the proofs verify against the digest.
    api_request req;
    req.method = "GET";
    req.path = "/v1/ledgers/bank/digest";
    req.principal = "CN=auditor";
    req.want_ion = true;
    r = f.api.handle(req);
    BOOST_REQUIRE_EQUAL(r.status, 200);
    BOOST_TEST(r.content_type == "application/ion");
    const auto saved = saved_digest::from_ion(ion_value::parse(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(r.body.data()), r.body.size())));
    BOOST_TEST(saved.tip == 4U);

    req.path = "/v1/ledgers/bank/blocks/2";
    r = f.api.handle(req);
    BOOST_REQUIRE_EQUAL(r.status, 200);
    auto proof = proof_document::from_ion(ion_value::parse(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(r.body.data()), r.body.size())));
    BOOST_TEST(!check_proof(proof, saved));

    req.path = "/v1/ledgers/bank/revisions/acct/" + id + "/3";
    r = f.api.handle(req);
    BOOST_REQUIRE_EQUAL(r.status, 200);
    proof = proof_document::from_ion(ion_value::parse(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(r.body.data()), r.body.size())));
    BOOST_TEST(!check_proof(proof, saved));

    // A tip beyond the journal, a bad number.
    BOOST_TEST(
        f.call("GET", "/v1/ledgers/bank/blocks/2", "", "CN=auditor", {{"tip", "99"}}).status ==
        404);
    BOOST_TEST(f.call("GET", "/v1/ledgers/bank/blocks/x2").status == 400);

    // Tables, a scan, indexes.
    BOOST_REQUIRE_EQUAL(f.call("POST", "/v1/ledgers/bank/execute",
                               R"({"statements": [{"op": "create_index", "table": "acct",
                                   "index": "by_n", "path": ["n"]}]})")
                            .status,
                        200);
    BOOST_TEST(fixture::json(f.call("GET", "/v1/ledgers/bank/tables/acct/indexes"))
                   .find("indexes")
                   ->items()
                   .size() == 1U);
    BOOST_TEST(
        fixture::json(f.call("GET", "/v1/ledgers/bank/tables")).find("tables")->items().size() ==
        1U);
    BOOST_TEST(fixture::json(f.call("GET", "/v1/ledgers/bank/tables/acct/documents"))
                   .find("documents")
                   ->items()
                   .size() == 1U);
}

BOOST_AUTO_TEST_CASE(a_session_lookup_uses_an_index) {
    fixture f;
    f.make_bank();
    BOOST_REQUIRE_EQUAL(f.call("POST", "/v1/ledgers/bank/execute",
                               R"({"statements": [{"op": "create_index", "table": "acct",
                                   "index": "by_owner", "path": ["owner"]},
                                   {"op": "insert", "table": "acct", "id": "cccccccccccccccccccccc",
                                    "value": {"owner": "zed"}}]})")
                            .status,
                        200);
    const auto t =
        text(fixture::json(f.call("POST", "/v1/ledgers/bank/sessions", "", "CN=app")), "token");
    const auto r = f.call("POST", "/v1/ledgers/bank/sessions/" + t + "/lookup",
                          R"({"table": "acct", "index": "by_owner", "value": "zed"})", "CN=app");
    BOOST_REQUIRE_EQUAL(r.status, 200);
    BOOST_TEST(fixture::json(r).find("documents")->items().size() == 1U);
}

BOOST_AUTO_TEST_CASE(an_export_is_recorded_and_handed_to_the_runner) {
    fixture f;
    f.make_bank();
    auto r = f.call("POST", "/v1/ledgers/bank/exports",
                    R"({"start": "2025-01-01T00:00:00Z", "end": "2030-01-01T00:00:00Z",
                        "format": "ion_text", "bucket": "b", "prefix": "p/"})");
    BOOST_REQUIRE_EQUAL(r.status, 202);
    const auto job = fixture::json(r);
    BOOST_TEST(text(job, "status") == "in_progress");
    BOOST_REQUIRE_EQUAL(f.backend.started_exports.size(), 1U);
    BOOST_TEST(f.backend.started_exports[0].job_id == text(job, "jobId"));
    BOOST_TEST(f.call("GET", "/v1/ledgers/bank/exports/" + text(job, "jobId")).status == 200);
    BOOST_TEST(
        fixture::json(f.call("GET", "/v1/ledgers/bank/exports")).find("exports")->items().size() ==
        1U);
    BOOST_TEST(f.call("POST", "/v1/ledgers/bank/exports",
                      R"({"start": "2025-01-01T00:00:00Z", "end": "2030-01-01T00:00:00Z",
                          "format": "csv", "bucket": "b"})")
                   .status == 400);
    BOOST_TEST(f.call("POST", "/v1/ledgers/bank/exports",
                      R"({"start": "2025-01-01T00:00:00Z", "end": "2030-01-01T00:00:00Z",
                          "format": "json", "bucket": "b"})",
                      "CN=app")
                   .status == 403);
}

BOOST_AUTO_TEST_CASE(ion_bodies_keep_their_types) {
    fixture f;
    f.make_bank();
    api_request req;
    req.method = "POST";
    req.path = "/v1/ledgers/bank/execute";
    req.principal = "CN=app";
    req.body_is_ion = true;
    const auto body = ion_value::parse(std::string_view(
        R"({statements: [{op: "insert", table: "acct", id: "dddddddddddddddddddddd",
            value: {when: 2026-11-09T12:00:00Z, amount: 12.50, tag: gold}}]})"));
    const auto bytes = body.to_binary();
    req.body.assign(bytes.begin(), bytes.end());
    BOOST_REQUIRE_EQUAL(f.api.handle(req).status, 200);
    const auto doc =
        *f.backend.ledgers.begin()->second->sm.get_document("acct", "dddddddddddddddddddddd");
    BOOST_TEST((doc.data->find("when")->type() == kythira::ledger::ion_type::timestamp));
    BOOST_TEST((doc.data->find("amount")->type() == kythira::ledger::ion_type::decimal));
    BOOST_TEST((doc.data->find("tag")->type() == kythira::ledger::ion_type::symbol));
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_CASE(timestamps_in_requests_take_any_ion_precision) {
    const auto base = *parse_timestamp("2026-11-09T12:00:00.000Z");
    BOOST_TEST(any_timestamp("2026-11-09T12:00:00.000Z").value_or(0) == base);
    BOOST_TEST(any_timestamp("2026-11-09T12:00Z").value_or(0) == base);
    BOOST_TEST(any_timestamp("2026-11-09T12:00:00Z").value_or(0) == base);
    BOOST_TEST(any_timestamp("2026-11-09T12:00:00.5Z").value_or(0) == base + 500);
    BOOST_TEST(any_timestamp("2026-11-09T12:00:00.123456Z").value_or(0) == base + 123);
    BOOST_TEST(any_timestamp("2026-11-09T13:00+01:00").value_or(0) == base);
    BOOST_TEST(any_timestamp("2026-11-09T11:30:00-00:30").value_or(0) == base);
    BOOST_TEST(any_timestamp("2026-11-09").value_or(0) == base - (12 * 3'600'000));
    BOOST_TEST(any_timestamp("2026-11-09T").value_or(0) == base - (12 * 3'600'000));
    for (const auto* bad : {"2026-13-01", "2026-11-09T25:00Z", "2026-11-09T12:00", "yesterday",
                            "2026-11-09T12:00:00.Z", "2026-11-09T12:00Zjunk"}) {
        BOOST_TEST(!any_timestamp(bad), bad);
    }
}

BOOST_AUTO_TEST_CASE(roles_parse_from_a_file) {
    const auto roles = ledger_roles::parse(
        "# operators\n"
        "spiffe://k/admin * admin\n"
        "\n"
        "   # indented comment\n"
        "spiffe://k/app\tbank\twrite\r\n"
        "spiffe://k/app audit read\n"
        "spiffe://k/auditor * read");
    BOOST_TEST(roles.subjects() == 3U);
    BOOST_TEST(roles.allows("spiffe://k/admin", "anything", ledger_role::admin));
    BOOST_TEST(roles.allows("spiffe://k/app", "bank", ledger_role::write));
    BOOST_TEST(roles.allows("spiffe://k/app", "audit", ledger_role::read));
    BOOST_TEST(!roles.allows("spiffe://k/app", "audit", ledger_role::write));
    BOOST_TEST(!roles.allows("spiffe://k/app", "other", ledger_role::read));
    BOOST_TEST(roles.allows("spiffe://k/auditor", "bank", ledger_role::read));
    BOOST_TEST(!roles.allows("spiffe://k/auditor", "bank", ledger_role::write));
    BOOST_TEST(ledger_roles::parse("").subjects() == 0U);

    const auto error_of = [](std::string_view text) -> std::string {
        try {
            (void)ledger_roles::parse(text);
        } catch (const std::invalid_argument& e) {
            return e.what();
        }
        return "";
    };
    BOOST_TEST(error_of("a * admin\nb bank owner\n") == "roles line 2: unknown role 'owner'");
    BOOST_TEST(error_of("a *\n").starts_with("roles line 1: expected"));
    BOOST_TEST(error_of("a * read extra\n").starts_with("roles line 1: expected"));
}
