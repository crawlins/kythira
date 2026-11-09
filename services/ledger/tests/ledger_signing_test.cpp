// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// CA-signed digests (.kiro/specs/verifiable-ledger/ task 10,
// Requirement 4.5): signing with keys the test CA issues, verification
// against the CA bundle, every field bound, the wrong CA and a certificate
// not valid at signing time refused, the Ion form, GetDigest through the
// API with a signer, and ledger_ctl's --ca.

#define BOOST_TEST_MODULE ledger_signing_test
#include <boost/test/unit_test.hpp>

#include <kythira/ledger/ion_value.hpp>
#include <kythira/ledger/journal_segments.hpp>
#include <kythira/ledger/journal_signing.hpp>
#include <kythira/ledger/journal_types.hpp>
#include <kythira/ledger/journal_verifier.hpp>
#include <kythira/ledger/ledger_api.hpp>
#include <kythira/ledger/ledger_catalog.hpp>
#include <kythira/ledger/ledger_sessions.hpp>
#include <kythira/ledger/ledger_state_machine.hpp>

#include <raft/certificate_authority.hpp>
#include <raft/memory_kv_engine.hpp>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <sys/wait.h>

using namespace kythira::ledger;
using kythira::memory_kv_engine;
namespace ca = raft::testing;

using ledger_sm = basic_ledger_state_machine<memory_kv_engine>;
using catalog_sm = basic_ledger_catalog<memory_kv_engine>;

namespace {

constexpr std::int64_t t0 = 1'762'689'600'000;
constexpr std::string_view node_uri = "spiffe://kythira/ledger/node-1";

auto now_ms() -> std::int64_t {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

struct issued {
    std::string key;
    std::string chain;
};

auto issue(ca::certificate_authority& from, ca::key_algorithm alg = ca::key_algorithm::ecdsa_p256,
           std::chrono::system_clock::time_point not_before = std::chrono::system_clock::now() -
                                                              std::chrono::minutes(1)) -> issued {
    ca::leaf_certificate_options o;
    o.subject.common_name = "ledger node";
    o.uri_names = {std::string(node_uri)};
    o.algorithm = alg;
    o.server_auth = true;
    o.client_auth = true;
    o.not_before = not_before;
    auto m = from.issue(o);
    return {m.private_key_pem, m.chain_pem};
}

auto some_digest() -> saved_digest {
    saved_digest d;
    d.tip = 41;
    for (std::size_t i = 0; i < d.digest.size(); ++i) {
        d.digest[i] = static_cast<std::uint8_t>(i * 7 + 3);
    }
    return d;
}

struct temp_dir {
    std::filesystem::path path;
    temp_dir() {
        path = std::filesystem::temp_directory_path() / ("ledger_signing_" + new_id());
        std::filesystem::create_directories(path);
    }
    ~temp_dir() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
    temp_dir(const temp_dir&) = delete;
    temp_dir(temp_dir&&) = delete;
    auto operator=(const temp_dir&) -> temp_dir& = delete;
    auto operator=(temp_dir&&) -> temp_dir& = delete;
};

/// One ledger in one engine, entries applied at once.
struct local_backend {
    memory_kv_engine engine;
    catalog_sm cat;
    std::uint64_t catalog_index = 0;
    ledger_sm sm;
    std::unique_ptr<ledger_session_manager<ledger_sm>> sessions;
    std::mutex commit_mutex;
    std::uint64_t index = 0;
    bool opened = false;
    std::int64_t clock = t0;

    local_backend() { cat.open(engine, catalog_range()); }
    [[nodiscard]] auto catalog() const -> const catalog_sm& { return cat; }
    auto propose_catalog(std::vector<std::byte> entry) -> std::vector<std::byte> {
        return cat.apply(entry, ++catalog_index);
    }
    auto open(const ledger_record& r) -> std::optional<ledger_handle<ledger_sm>> {
        if (!opened) {
            sm.open(engine, ledger_range(r.ledger_id));
            sessions = std::make_unique<ledger_session_manager<ledger_sm>>(sm);
            opened = true;
        }
        ledger_handle<ledger_sm> h;
        h.sm = &sm;
        h.sessions = sessions.get();
        h.commit_mutex = &commit_mutex;
        h.propose = [this](std::vector<std::byte> cmd) { return sm.apply(cmd, ++index); };
        h.start_export = [](const export_job&) {};
        return h;
    }
    [[nodiscard]] auto term() const -> std::uint64_t { return 1; }
    auto now_ms() -> std::int64_t { return clock += 10; }
};

}  // namespace

BOOST_AUTO_TEST_SUITE(signing)

BOOST_AUTO_TEST_CASE(a_signed_digest_verifies_against_the_ca_for_each_key_type) {
    ca::certificate_authority authority;
    for (const auto alg : {ca::key_algorithm::ecdsa_p256, ca::key_algorithm::ecdsa_p384,
                           ca::key_algorithm::rsa_2048}) {
        const auto cert = issue(authority, alg);
        const digest_signer signer(cert.key, cert.chain);
        const auto sd = signer.sign("bank", some_digest(), now_ms());
        const auto check = check_signed_digest(sd, authority.root_certificate_pem());
        BOOST_TEST(!check.failure.has_value(), check.failure.value_or(""));
        BOOST_REQUIRE_EQUAL(check.signer.size(), 1U);
        BOOST_TEST(check.signer.front() == node_uri);
    }
}

BOOST_AUTO_TEST_CASE(every_signed_field_is_bound) {
    ca::certificate_authority authority;
    const auto cert = issue(authority);
    const digest_signer signer(cert.key, cert.chain);
    const auto good = signer.sign("bank", some_digest(), now_ms());
    const auto root = authority.root_certificate_pem();
    BOOST_REQUIRE(!check_signed_digest(good, root).failure);

    auto changed = [&](auto mutate) {
        auto sd = good;
        mutate(sd);
        return check_signed_digest(sd, root).failure.has_value();
    };
    BOOST_TEST(changed([](signed_digest& s) { s.ledger = "other"; }));
    BOOST_TEST(changed([](signed_digest& s) { s.digest.tip += 1; }));
    BOOST_TEST(changed([](signed_digest& s) { s.digest.digest[5] ^= 0x01; }));
    BOOST_TEST(changed([](signed_digest& s) { s.signed_at += 1; }));
    BOOST_TEST(changed([](signed_digest& s) { s.signature.back() ^= 0x01; }));
    BOOST_TEST(changed([](signed_digest& s) { s.signature.clear(); }));
    BOOST_TEST(changed([](signed_digest& s) { s.certificate_chain.clear(); }));
}

BOOST_AUTO_TEST_CASE(another_ca_or_another_key_is_refused) {
    ca::certificate_authority authority;
    ca::certificate_authority stranger;
    const auto ours = issue(authority);
    const auto theirs = issue(stranger);

    // Signed by a certificate from a CA the checker does not trust.
    const auto sd = digest_signer(theirs.key, theirs.chain).sign("bank", some_digest(), now_ms());
    const auto check = check_signed_digest(sd, authority.root_certificate_pem());
    BOOST_REQUIRE(check.failure.has_value());
    BOOST_TEST(check.failure->find("does not chain") != std::string::npos);

    // A trusted chain pasted onto a signature from another key.
    auto forged = sd;
    forged.certificate_chain = ours.chain;
    BOOST_TEST(check_signed_digest(forged, authority.root_certificate_pem()).failure.has_value());

    // A signer whose key is not the leaf's refuses to start.
    BOOST_CHECK_THROW(digest_signer(theirs.key, ours.chain), digest_signing_error);
    BOOST_CHECK_THROW(digest_signer("not a key", ours.chain), digest_signing_error);
    BOOST_CHECK_THROW(digest_signer(ours.key, ""), digest_signing_error);
    BOOST_TEST(check_signed_digest(sd, "").failure.has_value());
}

BOOST_AUTO_TEST_CASE(validity_is_judged_at_signing_time) {
    ca::certificate_authority authority;
    // Valid from an hour from now: a digest signed now was signed with a
    // certificate that was not yet valid.
    const auto future = issue(authority, ca::key_algorithm::ecdsa_p256,
                              std::chrono::system_clock::now() + std::chrono::hours(1));
    const digest_signer early(future.key, future.chain);
    BOOST_TEST(check_signed_digest(early.sign("bank", some_digest(), now_ms()),
                                   authority.root_certificate_pem())
                   .failure.has_value());
    // The same certificate, at a time inside its validity, verifies: what
    // counts is signedAt, not the checker's clock.
    const auto later = now_ms() + std::int64_t{2} * 3600 * 1000;
    BOOST_TEST(!check_signed_digest(early.sign("bank", some_digest(), later),
                                    authority.root_certificate_pem())
                    .failure.has_value());
}

BOOST_AUTO_TEST_CASE(the_ion_form_round_trips_and_reads_as_a_saved_digest) {
    ca::certificate_authority authority;
    const auto cert = issue(authority);
    const auto sd = digest_signer(cert.key, cert.chain).sign("bank", some_digest(), now_ms());
    const auto ion = sd.to_ion();
    BOOST_TEST(signed_digest::is_signed(ion));
    BOOST_TEST(!signed_digest::is_signed(some_digest().to_ion()));

    for (const auto& reparsed :
         {ion_value::parse(ion.to_binary()), ion_value::parse(std::string_view(ion.to_text()))}) {
        const auto back = signed_digest::from_ion(reparsed);
        BOOST_TEST(back.ledger == "bank");
        BOOST_TEST(back.signed_at == sd.signed_at);
        BOOST_TEST(back.signature == sd.signature);
        BOOST_TEST(!check_signed_digest(back, authority.root_certificate_pem()).failure);
        const auto plain = saved_digest::from_ion(reparsed);
        BOOST_TEST(plain.tip == sd.digest.tip);
        BOOST_TEST(plain.digest == sd.digest.digest);
    }
    BOOST_CHECK_THROW((void)signed_digest::from_ion(some_digest().to_ion()), std::exception);
}

BOOST_AUTO_TEST_CASE(get_digest_is_signed_when_the_api_has_a_signer) {
    ca::certificate_authority authority;
    const auto cert = issue(authority);
    local_backend backend;
    // The signature is judged at signedAt, the backend's clock: it must be
    // inside the certificate's validity.
    backend.clock = now_ms();
    ledger_api<local_backend, ledger_sm> api(
        backend, ledger_roles{}.grant("admin", "*", ledger_role::admin));
    auto call = [&api](std::string method, std::string path, std::string body = "") {
        api_request r;
        r.method = std::move(method);
        r.path = std::move(path);
        r.body = std::move(body);
        r.principal = "admin";
        r.want_ion = true;
        return api.handle(r);
    };
    BOOST_REQUIRE_EQUAL(call("POST", "/v1/ledgers", R"({name: "bank"})").status, 201);
    BOOST_REQUIRE_EQUAL(call("POST", "/v1/ledgers/bank/execute",
                             R"({statements: [{op: "create_table", table: "t"}]})")
                            .status,
                        200);
    auto parse = [](const api_response& r) {
        return ion_value::parse(
            std::span(reinterpret_cast<const std::uint8_t*>(r.body.data()), r.body.size()));
    };
    // Off by default.
    BOOST_TEST(!signed_digest::is_signed(parse(call("GET", "/v1/ledgers/bank/digest"))));

    api.set_digest_signer(std::make_shared<const digest_signer>(cert.key, cert.chain));
    const auto v = parse(call("GET", "/v1/ledgers/bank/digest"));
    BOOST_REQUIRE(signed_digest::is_signed(v));
    const auto sd = signed_digest::from_ion(v);
    BOOST_TEST(sd.ledger == "bank");
    BOOST_TEST(sd.digest.tip == 0U);
    BOOST_TEST(sd.digest.digest == backend.sm.digest_at(0));
    BOOST_TEST(!check_signed_digest(sd, authority.root_certificate_pem()).failure);
}

BOOST_AUTO_TEST_SUITE_END()

#ifdef KYTHIRA_LEDGER_CTL

BOOST_AUTO_TEST_SUITE(ledger_ctl_tool)

namespace {

auto ledger_ctl(const std::string& args, const std::filesystem::path& log) -> int {
    const std::string cmd =
        std::string(KYTHIRA_LEDGER_CTL) + " " + args + " >" + log.string() + " 2>&1";
    const int rc = std::system(cmd.c_str());  // NOLINT(cert-env33-c,concurrency-mt-unsafe)
    return WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
}

auto contains(const std::filesystem::path& log, std::string_view what) -> bool {
    const auto text = read_file(log);
    return text && text->find(what) != std::string::npos;
}

}  // namespace

BOOST_AUTO_TEST_CASE(prove_checks_the_digest_signature_with_ca) {
    ca::certificate_authority authority;
    ca::certificate_authority stranger;
    const auto cert = issue(authority);

    memory_kv_engine engine;
    ledger_sm sm;
    sm.open(engine, ledger_range("audit"));
    std::uint64_t index = 0;
    for (int i = 0; i < 3; ++i) {
        std::vector<ion_value> st;
        if (i == 0) {
            st.push_back(statements::create_table("t"));
        }
        ion_value doc = ion_value::make_struct();
        doc.set("n", ion_value::integer(i));
        st.push_back(statements::insert("t", new_id(), doc));
        const auto p = sm.prepare(new_id(), t0 + i, st);
        BOOST_REQUIRE(p.entry);
        BOOST_REQUIRE(apply_result::decode(sm.apply(p.command, ++index)).committed);
    }
    const saved_digest saved{2, sm.digest_at(2)};
    const auto sd = digest_signer(cert.key, cert.chain).sign("audit", saved, now_ms());

    temp_dir work;
    const auto log = work.path / "out.txt";
    const auto proof = work.path / "proof.ion";
    const auto signed_file = work.path / "signed.ion";
    const auto plain_file = work.path / "plain.ion";
    const auto ca_file = work.path / "ca.pem";
    const auto other_ca = work.path / "other.pem";
    const auto p = sm.get_block_proof(1, 2);
    write_file_durably(
        proof, proof_document{p.blk, std::nullopt, p.proof, p.digest, 2}.to_ion().to_text());
    write_file_durably(signed_file, sd.to_ion().to_text());
    write_file_durably(plain_file, saved.to_ion().to_text());
    write_file_durably(ca_file, authority.root_certificate_pem());
    write_file_durably(other_ca, stranger.root_certificate_pem());
    const std::string prove = "prove --proof " + proof.string() + " --digest ";

    BOOST_TEST(ledger_ctl(prove + signed_file.string() + " --ca " + ca_file.string(), log) == 0);
    BOOST_TEST(contains(log, "signed by spiffe://kythira/ledger/node-1"));
    BOOST_TEST(contains(log, "block 1 is in the ledger"));

    BOOST_TEST(ledger_ctl(prove + signed_file.string() + " --ca " + other_ca.string(), log) == 1);
    BOOST_TEST(contains(log, "FAILED: digest signature"));

    BOOST_TEST(ledger_ctl(prove + plain_file.string() + " --ca " + ca_file.string(), log) == 1);
    BOOST_TEST(contains(log, "not signed"));

    // Without --ca a signed digest still works as a digest, with a note.
    BOOST_TEST(ledger_ctl(prove + signed_file.string(), log) == 0);
    BOOST_TEST(contains(log, "pass --ca"));
}

BOOST_AUTO_TEST_SUITE_END()

#endif
