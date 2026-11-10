// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// The ledger API over HTTPS with mutual TLS, and the C++ client
// (.kiro/specs/verifiable-ledger/ task 7, Requirements 8.1-8.4): a real
// beast_https_server with certificates from a test CA, the API over an
// in-process backend, and ledger_client over https_transport. Covers
// identity from the client certificate, roles, Ion bodies end to end,
// percent-encoded targets, body limits, OCC retry, leader redirects and
// endpoint failover.

#define BOOST_TEST_MODULE ledger_http_test
#include <boost/test/unit_test.hpp>

#include <kythira/ledger/ion_value.hpp>
#include <kythira/ledger/journal_types.hpp>
#include <kythira/ledger/journal_verifier.hpp>
#include <kythira/ledger/ledger_api.hpp>
#include <kythira/ledger/ledger_catalog.hpp>
#include <kythira/ledger/ledger_client.hpp>
#include <kythira/ledger/ledger_http.hpp>
#include <kythira/ledger/ledger_sessions.hpp>
#include <kythira/ledger/ledger_state_machine.hpp>

#include <raft/beast_https_server.hpp>
#include <raft/certificate_authority.hpp>
#include <raft/memory_kv_engine.hpp>
#include <raft/private_file.hpp>

#include <atomic>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <sys/wait.h>

using namespace kythira::ledger;
using kythira::memory_kv_engine;

using ledger_sm = basic_ledger_state_machine<memory_kv_engine>;
using catalog_sm = basic_ledger_catalog<memory_kv_engine>;

namespace {

constexpr std::string_view admin_id = "spiffe://kythira/ledger/admin";
constexpr std::string_view app_id = "spiffe://kythira/ledger/app";
constexpr std::string_view auditor_id = "spiffe://kythira/ledger/auditor";

/// Certificates: a server leaf for localhost, three clients with URI SANs,
/// one client with only a DNS SAN, and one from a CA the server does not
/// trust.
struct pki {
    std::filesystem::path dir;
    std::string ca_path;
    std::string server_cert;
    std::string server_key;
    struct pair {
        std::string cert;
        std::string key;
    };
    pair admin;
    pair app;
    pair auditor;
    pair nameless;
    pair stranger;

    pki() {
        dir = raft::make_private_temp_directory(std::filesystem::temp_directory_path(),
                                                "ledger-http-test-");
        raft::testing::certificate_authority ca;
        raft::testing::certificate_authority other;
        auto write = [&](const std::string& name, const std::string& pem) {
            const auto p = (dir / name).string();
            raft::write_private_file(p, pem);
            return p;
        };
        ca_path = write("ca.pem", ca.root_certificate_pem());
        raft::testing::leaf_certificate_options server;
        server.subject.common_name = "localhost";
        server.dns_names = {"localhost"};
        server.ip_addresses = {"127.0.0.1"};
        auto s = ca.issue(server);
        server_cert = write("server.pem", s.chain_pem);
        server_key = write("server.key", s.private_key_pem);

        auto client = [&](raft::testing::certificate_authority& from, const std::string& name,
                          std::optional<std::string_view> uri) {
            raft::testing::leaf_certificate_options o;
            o.subject.common_name = name;
            if (uri) {
                o.uri_names = {std::string(*uri)};
            } else {
                o.dns_names = {name + ".clients.test"};  // a SAN, but no URI
            }
            o.server_auth = false;
            o.client_auth = true;
            auto c = from.issue(o);
            return pair{write(name + ".pem", c.chain_pem), write(name + ".key", c.private_key_pem)};
        };
        admin = client(ca, "admin", admin_id);
        app = client(ca, "app", app_id);
        auditor = client(ca, "auditor", auditor_id);
        nameless = client(ca, "nameless", std::nullopt);
        stranger = client(other, "stranger", admin_id);
    }
    ~pki() {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
    pki(const pki&) = delete;
    auto operator=(const pki&) -> pki& = delete;
    pki(pki&&) = delete;
    auto operator=(pki&&) -> pki& = delete;
};

auto shared_pki() -> const pki& {
    static const pki p;
    return p;
}

/// Every group in one engine with entries applied at once, safe for the
/// server's one-thread-per-connection model. A follower backend sends
/// every write to `leader_url`.
struct local_backend {
    struct hosted {
        ledger_sm sm;
        std::unique_ptr<ledger_session_manager<ledger_sm>> sessions;
        std::mutex commit_mutex;
        std::mutex apply_mutex;
        std::uint64_t index = 0;
    };

    std::mutex mu;
    memory_kv_engine engine;
    catalog_sm cat;
    std::uint64_t catalog_index = 0;
    std::map<std::string, std::unique_ptr<hosted>> ledgers;
    std::atomic<std::int64_t> clock{1'762'689'600'000};
    std::optional<std::string> leader_url;  // set: this member follows
    /// Called before each ledger entry is proposed (test hook).
    std::function<void()> before_propose;

    local_backend() { cat.open(engine, catalog_range()); }

    [[nodiscard]] auto catalog() const -> const catalog_sm& { return cat; }

    auto propose_catalog(std::vector<std::byte> entry) -> std::vector<std::byte> {
        if (leader_url) {
            throw ledger_not_leader(leader_url);
        }
        const std::lock_guard lock(mu);
        return cat.apply(entry, ++catalog_index);
    }

    auto open(const ledger_record& r) -> std::optional<ledger_handle<ledger_sm>> {
        if (leader_url) {
            return std::nullopt;
        }
        hosted* h = nullptr;
        {
            const std::lock_guard lock(mu);
            auto& slot = ledgers[r.ledger_id];
            if (!slot) {
                slot = std::make_unique<hosted>();
                slot->sm.open(engine, ledger_range(r.ledger_id));
                slot->sessions = std::make_unique<ledger_session_manager<ledger_sm>>(slot->sm);
            }
            h = slot.get();
        }
        ledger_handle<ledger_sm> out;
        out.sm = &h->sm;
        out.sessions = h->sessions.get();
        out.commit_mutex = &h->commit_mutex;
        out.propose = [this, h](std::vector<std::byte> cmd) {
            if (before_propose) {
                before_propose();
            }
            const std::lock_guard lock(h->apply_mutex);
            return h->sm.apply(cmd, ++h->index);
        };
        out.start_export = [](const export_job&) {};
        return out;
    }

    [[nodiscard]] auto term() const -> std::uint64_t { return 1; }
    auto now_ms() -> std::int64_t { return clock += 10; }
};

using api_t = ledger_api<local_backend, ledger_sm>;

auto roles() -> ledger_roles {
    return ledger_roles{}
        .grant(std::string(admin_id), "*", ledger_role::admin)
        .grant(std::string(app_id), "bank", ledger_role::write)
        .grant(std::string(auditor_id), "bank", ledger_role::read);
}

/// One member: a backend, its API and an HTTPS server on a free port.
struct member {
    local_backend backend;
    api_t api{backend, roles()};
    std::unique_ptr<kythira::https::server> server;

    explicit member(std::uint64_t max_body = 0) {
        const auto& p = shared_pki();
        auto cfg = ledger_http_limits();
        cfg.bind_address = "127.0.0.1";
        cfg.port = 0;  // the server picks a free one
        cfg.tls_cert_path = p.server_cert;
        cfg.tls_key_path = p.server_key;
        cfg.client_ca_path = p.ca_path;
        if (max_body != 0) {
            cfg.max_body_bytes = max_body;
        }
        server = std::make_unique<kythira::https::server>(
            cfg, make_ledger_handler([this](const api_request& r) { return api.handle(r); }));
        server->start();
    }

    [[nodiscard]] auto url() const -> std::string {
        return "https://localhost:" + std::to_string(server->port());
    }
};

auto transport_for(const pki::pair& who) -> ledger_transport {
    https_transport_options o;
    o.ca_path = shared_pki().ca_path;
    o.cert_path = who.cert;
    o.key_path = who.key;
    o.timeout = std::chrono::seconds(10);
    return https_transport(o);
}

auto client_for(const pki::pair& who, std::vector<std::string> endpoints) -> ledger_client {
    ledger_client_options o;
    o.endpoints = std::move(endpoints);
    o.base_backoff = std::chrono::milliseconds(1);
    o.max_backoff = std::chrono::milliseconds(5);
    return {o, transport_for(who)};
}

auto text(const ion_value& v, std::string_view field) -> std::string {
    const ion_value* f = v.find(field);
    return f != nullptr ? f->as_text().value_or("") : "";
}

auto int_field(const ion_value& v, std::string_view field) -> std::int64_t {
    const ion_value* f = v.find(field);
    return f != nullptr ? f->as_int64().value_or(-1) : -1;
}

auto account(std::string_view owner, std::int64_t balance) -> ion_value {
    ion_value v = ion_value::make_struct();
    v.set("owner", ion_value::string(owner)).set("balance", ion_value::integer(balance));
    return v;
}

auto make_bank(ledger_client& admin) -> void {
    admin.create_ledger("bank", false);
    admin.execute("bank", {statements::create_table("acct")});
}

/// Sends a raw request with a given client certificate (or none).
auto raw(const member& m, const std::optional<pki::pair>& who, api_request req) -> api_response {
    https_transport_options o;
    o.ca_path = shared_pki().ca_path;
    if (who) {
        o.cert_path = who->cert;
        o.key_path = who->key;
    }
    return https_transport(o)(m.url(), req);
}

auto get_req(std::string path) -> api_request {
    api_request r;
    r.method = "GET";
    r.path = std::move(path);
    return r;
}

/// The ledger's digest at an earlier block, from a block proof's answer.
auto digest_of_tip(ledger_client& c, std::uint64_t tip) -> saved_digest {
    const auto p = proof_document::from_ion(c.block("bank", 0, tip));
    return saved_digest{p.tip, p.digest};
}

}  // namespace

BOOST_AUTO_TEST_SUITE(identity)

BOOST_AUTO_TEST_CASE(the_principal_is_the_client_certificate_uri_san) {
    member m;
    auto admin = client_for(shared_pki().admin, {m.url()});
    make_bank(admin);

    // No certificate: 401 before the API; health checks need none.
    auto r = raw(m, std::nullopt, get_req("/v1/ledgers"));
    BOOST_TEST(r.status == 401);
    BOOST_TEST(r.body.find("unauthenticated") != std::string::npos);
    BOOST_TEST(raw(m, std::nullopt, get_req("/healthz")).status == 200);

    // A verified certificate without a URI SAN has no identity.
    BOOST_TEST(raw(m, shared_pki().nameless, get_req("/v1/ledgers")).status == 401);

    // A certificate from another CA is refused. Under TLS 1.3 the client
    // finishes its side of the handshake before the server checks the
    // certificate, so the refusal can arrive after the request was written:
    // the client cannot tell it from a lost reply, and reports it as sent.
    BOOST_CHECK_THROW((void)raw(m, shared_pki().stranger, get_req("/v1/ledgers")),
                      ledger_transport_error);

    // Roles follow the URI SAN.
    auto app = client_for(shared_pki().app, {m.url()});
    auto auditor = client_for(shared_pki().auditor, {m.url()});
    BOOST_CHECK_NO_THROW(
        app.execute("bank", {statements::insert("acct", new_id(), account("alice", 10))}));
    try {
        app.create_ledger("other");
        BOOST_FAIL("app created a ledger");
    } catch (const ledger_client_error& e) {
        BOOST_TEST(e.status() == 403);
        BOOST_TEST(e.code() == "forbidden");
        BOOST_TEST(!e.retryable());
    }
    BOOST_TEST(auditor.tables("bank").find("tables") != nullptr);
    try {
        auditor.execute("bank", {statements::insert("acct", new_id(), account("eve", 1))});
        BOOST_FAIL("auditor wrote");
    } catch (const ledger_client_error& e) {
        BOOST_TEST(e.status() == 403);
    }
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(client)

BOOST_AUTO_TEST_CASE(documents_keep_their_ion_types_and_proofs_verify) {
    member m;
    auto admin = client_for(shared_pki().admin, {m.url()});
    make_bank(admin);

    // A decimal, a timestamp with an offset and a blob survive the round
    // trip because the client speaks Ion binary.
    const auto doc = ion_value::parse(std::string_view(
        R"({owner: "alice", balance: 12.50d0, opened: 2025-03-04T05:06:07.890-08:00,
            key: {{AAECAwQ=}}})"));
    const auto id = new_id();
    const auto result = admin.execute("bank", {statements::insert("acct", id, doc)});
    BOOST_TEST(text(result, "status") == "committed");

    const auto got = admin.get_document("bank", "acct", id);
    BOOST_REQUIRE(got.has_value());
    const ion_value* data = got->find("data");
    BOOST_REQUIRE(data != nullptr);
    BOOST_TEST(data->to_binary() == doc.to_binary());
    BOOST_TEST(!admin.get_document("bank", "acct", new_id()).has_value());

    // Digest and a revision proof over HTTPS check offline.
    const auto digest = admin.digest("bank");
    const auto tip = static_cast<std::uint64_t>(int_field(digest, "tip"));
    const auto proof = admin.revision("bank", "acct", id, 0, tip);
    const auto checked =
        check_proof(proof_document::from_ion(proof), saved_digest::from_ion(digest));
    BOOST_TEST(!checked.has_value(), checked.value_or("verified"));

    // History and scans go through query strings.
    const auto history = admin.history("bank", "acct", id);
    BOOST_TEST(history.find("revisions") != nullptr);
    const auto page = admin.scan("bank", "acct", std::nullopt, 10);
    BOOST_REQUIRE(page.find("documents") != nullptr);
    BOOST_TEST(page.find("documents")->size() == 1U);
}

BOOST_AUTO_TEST_CASE(a_transaction_retries_on_an_occ_conflict) {
    member m;
    auto admin = client_for(shared_pki().admin, {m.url()});
    make_bank(admin);
    const auto id = new_id();
    admin.execute("bank", {statements::insert("acct", id, account("alice", 100))});

    auto app = client_for(shared_pki().app, {m.url()});
    auto rival = client_for(shared_pki().app, {m.url()});
    int runs = 0;
    const auto result = app.transact("bank", [&](ledger_client::transaction& t) {
        ++runs;
        const auto cur = t.get("acct", id);
        BOOST_REQUIRE(cur.has_value());
        const auto balance = int_field(*cur->find("data"), "balance");
        if (runs == 1) {
            // Someone else commits between our read and our commit.
            rival.execute("bank", {statements::replace("acct", id, account("alice", balance + 1))});
        }
        t.add(statements::replace("acct", id, account("alice", balance - 30)));
    });
    BOOST_TEST(text(result, "status") == "committed");
    BOOST_TEST(runs == 2);
    BOOST_TEST(app.last_attempts() == 2);
    const auto final_doc = admin.get_document("bank", "acct", id);
    BOOST_REQUIRE(final_doc.has_value());
    BOOST_TEST(int_field(*final_doc->find("data"), "balance") == 71);  // 100 + 1 - 30
}

BOOST_AUTO_TEST_CASE(a_transaction_gives_up_after_max_attempts) {
    member m;
    auto admin = client_for(shared_pki().admin, {m.url()});
    make_bank(admin);
    const auto id = new_id();
    admin.execute("bank", {statements::insert("acct", id, account("alice", 0))});

    ledger_client_options o;
    o.endpoints = {m.url()};
    o.max_attempts = 3;
    o.base_backoff = std::chrono::milliseconds(1);
    ledger_client app(o, transport_for(shared_pki().app));
    auto rival = client_for(shared_pki().app, {m.url()});
    int runs = 0;
    try {
        app.transact("bank", [&](ledger_client::transaction& t) {
            ++runs;
            const auto cur = t.get("acct", id);
            const auto balance = int_field(*cur->find("data"), "balance");
            rival.execute("bank", {statements::replace("acct", id, account("alice", balance + 1))});
            t.add(statements::replace("acct", id, account("alice", -1)));
        });
        BOOST_FAIL("a transaction that always conflicts committed");
    } catch (const ledger_client_error& e) {
        BOOST_TEST(e.code() == "occ_conflict");
        BOOST_TEST(e.retryable());
    }
    BOOST_TEST(runs == 3);
    BOOST_TEST(app.last_attempts() == 3);
}

BOOST_AUTO_TEST_CASE(sessions_insert_and_lookup_through_the_client) {
    member m;
    auto admin = client_for(shared_pki().admin, {m.url()});
    make_bank(admin);
    admin.execute("bank", {statements::create_index("acct", "by_owner",
                                                    document_path{std::string("owner")})});
    std::string id;
    admin.transact(
        "bank", [&](ledger_client::transaction& t) { id = t.insert("acct", account("bob", 5)); });
    BOOST_TEST(id.size() == 22U);
    admin.transact("bank", [&](ledger_client::transaction& t) {
        const auto page = t.lookup("acct", "by_owner", ion_value::string("bob"));
        BOOST_REQUIRE(page.find("documents") != nullptr);
        BOOST_TEST(page.find("documents")->size() == 1U);
        t.add(statements::replace("acct", id, account("bob", 6)));
    });
    BOOST_TEST(int_field(*admin.get_document("bank", "acct", id)->find("data"), "balance") == 6);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(routing)

BOOST_AUTO_TEST_CASE(a_follower_redirects_to_the_leader) {
    member leader;
    member follower;
    follower.backend.leader_url = leader.url();
    auto admin = client_for(shared_pki().admin, {follower.url()});
    BOOST_CHECK_NO_THROW(make_bank(admin));
    BOOST_TEST(admin.endpoint() == leader.url());
    BOOST_TEST(leader.backend.catalog().get("bank").has_value());
}

BOOST_AUTO_TEST_CASE(an_unreachable_member_is_skipped) {
    member m;
    // A port nothing listens on: bind one and let it go.
    std::string dead;
    {
        member gone;
        dead = gone.url();
    }
    auto admin = client_for(shared_pki().admin, {dead, m.url()});
    BOOST_CHECK_NO_THROW(make_bank(admin));
    BOOST_TEST(admin.endpoint() == m.url());
}

BOOST_AUTO_TEST_CASE(targets_are_percent_decoded_and_limits_apply) {
    member m(64 * 1024);
    auto admin = client_for(shared_pki().admin, {m.url()});
    make_bank(admin);

    // The transport encodes the query; JSON comes back unless Ion is asked
    // for.
    api_request req = get_req("/v1/ledgers/bank/tables/acct/documents");
    req.query["limit"] = "5";
    req.query["after"] = "";
    req.want_ion = false;
    auto r = raw(m, shared_pki().admin, req);
    BOOST_TEST(r.status == 200);
    BOOST_TEST(r.content_type == "application/json");

    // A body over the server's limit is refused before the API.
    std::string big(100 * 1024, 'x');
    try {
        admin.execute("bank", {statements::insert("acct", new_id(), account(big, 1))});
        BOOST_FAIL("an oversized body was accepted");
    } catch (const ledger_client_error& e) {
        BOOST_TEST(e.status() == 413);
    } catch (const ledger_transport_error&) {
        // The server may close the connection instead of answering.
    }
}

BOOST_AUTO_TEST_CASE(the_handler_decodes_targets_and_checks_identity) {
    namespace http = boost::beast::http;
    std::vector<api_request> seen;
    ion_worker_pool pool(2);
    auto handler = make_ledger_handler(
        [&seen](const api_request& r) {
            seen.push_back(r);
            return api_response{200, "application/json", "{}"};
        },
        &pool);
    kythira::https::request_context ctx;
    ctx.client_verified = true;
    ctx.client_uri_sans = {std::string(app_id), "spiffe://kythira/other"};

    kythira::https::request req{http::verb::post,
                                "/v1/ledgers/b%61nk/execute?limit=5&after=a%2Bb+c&flag", 11};
    req.set(http::field::content_type, "application/ion; charset=binary");
    req.set(http::field::accept, "application/ion");
    req.body() = "body";
    auto res = handler(req, ctx);
    BOOST_TEST(res.result_int() == 200U);
    BOOST_REQUIRE_EQUAL(seen.size(), 1U);
    BOOST_TEST(seen[0].method == "POST");
    BOOST_TEST(seen[0].path == "/v1/ledgers/bank/execute");
    BOOST_TEST(seen[0].query.at("limit") == "5");
    BOOST_TEST(seen[0].query.at("after") == "a+b c");
    BOOST_TEST(seen[0].query.at("flag").empty());
    BOOST_TEST(seen[0].body == "body");
    BOOST_TEST(seen[0].body_is_ion);
    BOOST_TEST(seen[0].want_ion);
    BOOST_TEST(seen[0].principal == app_id);  // the first URI SAN

    // A malformed escape never reaches the API.
    kythira::https::request bad{http::verb::get, "/v1/ledgers/b%6", 11};
    BOOST_TEST(handler(bad, ctx).result_int() == 400U);
    // Neither does a request without a verified certificate.
    kythira::https::request_context anon;
    BOOST_TEST(handler(req, anon).result_int() == 401U);
    kythira::https::request_context unverified = ctx;
    unverified.client_verified = false;
    BOOST_TEST(handler(req, unverified).result_int() == 401U);
    BOOST_TEST(seen.size() == 1U);
    // Health checks need no identity.
    kythira::https::request health{http::verb::get, "/healthz", 11};
    BOOST_TEST(handler(health, anon).result_int() == 200U);
}

BOOST_AUTO_TEST_CASE(percent_coding_round_trips) {
    const std::string s = "a b/c%d+e~f_g.h-i\xC3\xA9";
    const auto enc = percent_encode(s);
    BOOST_TEST(enc.find('/') == std::string::npos);
    BOOST_TEST(percent_decode(enc, false).value_or("") == s);
    BOOST_TEST(percent_decode("a+b", true).value_or("") == "a b");
    BOOST_TEST(percent_decode("a+b", false).value_or("") == "a+b");
    BOOST_TEST(!percent_decode("%4", false).has_value());
    BOOST_TEST(!percent_decode("%zz", false).has_value());
    BOOST_TEST(is_ion_media_type("application/ion; charset=binary"));
    BOOST_TEST(!is_ion_media_type("application/json"));
}

BOOST_AUTO_TEST_SUITE_END()

#ifdef KYTHIRA_LEDGER_CTL

// Task 10, Requirement 9.1: `ledger_ctl verify --api` re-hashes a running
// node's journal through GetDigest and GetBlock.
BOOST_AUTO_TEST_SUITE(ledger_ctl_api)

namespace {

auto ledger_ctl(const std::string& args, const std::filesystem::path& log) -> int {
    const std::string cmd =
        std::string(KYTHIRA_LEDGER_CTL) + " " + args + " >" + log.string() + " 2>&1";
    const int rc = std::system(cmd.c_str());  // NOLINT(cert-env33-c,concurrency-mt-unsafe)
    return WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
}

auto output(const std::filesystem::path& log) -> std::string {
    return read_file(log).value_or("");
}

}  // namespace

BOOST_AUTO_TEST_CASE(verify_through_the_api_re_hashes_every_block) {
    member m;
    auto admin = client_for(shared_pki().admin, {m.url()});
    make_bank(admin);
    for (int i = 0; i < 5; ++i) {
        admin.execute("bank", {statements::insert("acct", new_id(), account("o", i))});
    }
    const auto& p = shared_pki();
    const auto dir = raft::make_private_temp_directory(std::filesystem::temp_directory_path(),
                                                       "ledger-ctl-api-");
    const auto log = dir / "out.txt";
    const std::string base = "verify --api " + m.url() + " --ledger bank --tls-ca " + p.ca_path +
                             " --cert " + p.auditor.cert + " --key " + p.auditor.key;

    BOOST_TEST(ledger_ctl(base, log) == 0, output(log));
    BOOST_TEST(output(log).find("verified 6 blocks of ledger bank") != std::string::npos,
               output(log));

    // A digest saved at block 3 is checked on the way to the tip.
    const auto saved = digest_of_tip(admin, 3);
    const auto digest_file = dir / "digest.ion";
    write_file_durably(digest_file, saved.to_ion().to_text());
    BOOST_TEST(ledger_ctl(base + " --digest " + digest_file.string(), log) == 0, output(log));
    BOOST_TEST(output(log).find("saved digest at block 3") != std::string::npos, output(log));

    // A saved digest that does not match fails without naming a block.
    auto wrong = saved;
    wrong.digest[0] ^= 0x01U;
    write_file_durably(digest_file, wrong.to_ion().to_text());
    BOOST_TEST(ledger_ctl(base + " --digest " + digest_file.string(), log) == 1, output(log));
    BOOST_TEST(output(log).find("FAILED: digest at block 3") != std::string::npos, output(log));

    // A revision altered in the node's engine: the failure names block 2.
    {
        const auto id = m.backend.cat.get("bank")->ledger_id;
        const std::string key = ledger_range(id).start + "journal/00000000000000000002";
        const auto stored = m.backend.engine.get(kythira::column_family::data, key);
        BOOST_REQUIRE(stored.has_value());
        auto b = block::from_ion(ion_value::parse(std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(stored->data()), stored->size())));
        BOOST_REQUIRE(!b.revisions.empty());
        b.revisions.at(0).data->set("balance", ion_value::integer(1'000'000));
        const auto bytes = b.to_ion().to_binary();
        m.backend.engine.write(kythira::write_batch{}.put(
            kythira::column_family::data, key,
            std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size())));
    }
    BOOST_TEST(ledger_ctl(base, log) == 1, output(log));
    BOOST_TEST(output(log).find("FAILED at block 2") != std::string::npos, output(log));

    // A client the node does not accept, or a ledger it does not have, is
    // an error (2), not a verdict.
    BOOST_TEST(ledger_ctl("verify --api " + m.url() + " --ledger bank --tls-ca " + p.ca_path +
                              " --cert " + p.stranger.cert + " --key " + p.stranger.key,
                          log) == 2,
               output(log));
    BOOST_TEST(ledger_ctl("verify --api " + m.url() + " --ledger nope --tls-ca " + p.ca_path +
                              " --cert " + p.auditor.cert + " --key " + p.auditor.key,
                          log) == 2,
               output(log));
    BOOST_TEST(ledger_ctl("verify --api " + m.url() + " --ledger bank", log) == 2, output(log));

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

BOOST_AUTO_TEST_SUITE_END()

#endif
