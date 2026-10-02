// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// DNS peer discovery and ACME dns-01 against a real BIND 9 server.
//
// dns_peer_discovery_unit_test only ever points the ldns-backed classes at a
// closed port, so it proves they fail closed and nothing else. Here every
// UPDATE goes to a real authoritative server that accepts only TSIG-signed
// updates, and every assertion is made by querying that server directly, so
// a record that was never written, never removed, or written to the wrong
// name fails the test rather than passing as "no crash".
//
// The server is started by scripts/dns-test-server.sh as a CTest fixture
// (see tests/CMakeLists.txt), which passes its port and state directory in
// KYTHIRA_DNS_TEST_PORT and KYTHIRA_DNS_TEST_STATE_DIR. The state directory
// holds the TSIG key name and secret the script generated for this run.
// Running the binary without them is an error, not a skip: the only way to
// reach this file is a build that found both libldns and named.

#define BOOST_TEST_MODULE dns_discovery_bind_integration_test
#include <boost/test/unit_test.hpp>

#include "acme_test_server.hpp"

#include <raft/acme_certificate_provider.hpp>
#include <raft/acme_certificate_provider_impl.hpp>
#include <raft/rfc1035_peer_discovery.hpp>
#include <raft/rfc2136_dns_sd_discovery.hpp>
#include <raft/rfc2136_ldns_discovery.hpp>
#include <raft/rfc6763_ldns_peer_discovery.hpp>
#include <raft/rfc6763_peer_discovery.hpp>

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
#include <folly/init/Init.h>
#endif

#include <openssl/bio.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

using namespace std::chrono_literals;

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
struct FollyInitFixture {
    FollyInitFixture() {
        int argc = 1;
        char* argv0[] = {const_cast<char*>("dns_discovery_bind_integration_test"), nullptr};
        char** argv = argv0;
        _init = std::make_unique<folly::Init>(&argc, &argv);
    }
    std::unique_ptr<folly::Init> _init;
};
BOOST_GLOBAL_FIXTURE(FollyInitFixture);
#endif

namespace {

constexpr const char* k_server = "127.0.0.1";
constexpr const char* k_zone = "example.test.";
constexpr const char* k_cluster_zone = "cluster.example.test.";

auto read_file(const std::string& path) -> std::string {
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error("cannot read " + path);
    }
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

auto required_env(const char* name) -> std::string {
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        throw std::runtime_error(std::string(name) +
                                 " is not set; run this test through ctest, which starts the "
                                 "BIND fixture (scripts/dns-test-server.sh) first");
    }
    return value;
}

struct bind_server {
    std::uint16_t port;
    std::string tsig_name;
    std::string tsig_secret;
};

auto server() -> const bind_server& {
    static const bind_server s = [] {
        const std::string dir = required_env("KYTHIRA_DNS_TEST_STATE_DIR");
        return bind_server{
            static_cast<std::uint16_t>(std::stoi(required_env("KYTHIRA_DNS_TEST_PORT"))),
            read_file(dir + "/tsig.name"), read_file(dir + "/tsig.secret")};
    }();
    return s;
}

// The rdata of every record of `type` at `name`, as ldns prints it, asked of
// the BIND fixture directly. An empty result means the name has no such
// record (NXDOMAIN and NODATA alike); a failed query throws, so a dead server
// cannot masquerade as an absent record.
auto query(const std::string& name, ldns_rr_type type) -> std::vector<std::string> {
    std::unique_ptr<ldns_resolver, void (*)(ldns_resolver*)> res{ldns_resolver_new(),
                                                                 ldns_resolver_free};
    BOOST_REQUIRE(res);
    ldns_rdf* ns = nullptr;
    BOOST_REQUIRE(ldns_str2rdf_a(&ns, k_server) == LDNS_STATUS_OK);
    ldns_resolver_push_nameserver(res.get(), ns);
    ldns_rdf_deep_free(ns);
    ldns_resolver_set_port(res.get(), server().port);
    ldns_resolver_set_recursive(res.get(), false);

    std::unique_ptr<ldns_rdf, void (*)(ldns_rdf*)> qname{ldns_dname_new_frm_str(name.c_str()),
                                                         ldns_rdf_deep_free};
    BOOST_REQUIRE(qname);
    ldns_pkt* raw = nullptr;
    if (ldns_resolver_query_status(&raw, res.get(), qname.get(), type, LDNS_RR_CLASS_IN, 0) !=
            LDNS_STATUS_OK ||
        raw == nullptr) {
        throw std::runtime_error("query for " + name + " got no answer from the BIND fixture");
    }
    std::unique_ptr<ldns_pkt, void (*)(ldns_pkt*)> pkt{raw, ldns_pkt_free};
    const auto rcode = ldns_pkt_get_rcode(pkt.get());
    if (rcode != LDNS_RCODE_NOERROR && rcode != LDNS_RCODE_NXDOMAIN) {
        throw std::runtime_error("query for " + name + " failed with rcode " +
                                 std::to_string(static_cast<int>(rcode)));
    }

    std::vector<std::string> out;
    ldns_rr_list* answer = ldns_pkt_answer(pkt.get());
    for (std::size_t i = 0; answer != nullptr && i < ldns_rr_list_rr_count(answer); ++i) {
        ldns_rr* rr = ldns_rr_list_rr(answer, i);
        if (ldns_rr_get_type(rr) != type) {
            continue;
        }
        std::string rdata;
        for (std::size_t f = 0; f < ldns_rr_rd_count(rr); ++f) {
            char* s = ldns_rdf2str(ldns_rr_rdf(rr, f));
            if (!rdata.empty()) {
                rdata += ' ';
            }
            rdata += s;
            free(s);  // NOLINT(cppcoreguidelines-no-malloc) — ldns allocates with malloc
        }
        out.push_back(std::move(rdata));
    }
    std::sort(out.begin(), out.end());
    return out;
}

auto sorted_addresses(const std::vector<kythira::peer_info<std::string, std::string>>& peers)
    -> std::vector<std::string> {
    std::vector<std::string> out;
    out.reserve(peers.size());
    for (const auto& p : peers) {
        out.push_back(p.address);
    }
    std::sort(out.begin(), out.end());
    return out;
}

auto rfc2136_cfg(const std::string& shared_name) -> kythira::rfc2136_ldns_discovery::config {
    kythira::rfc2136_ldns_discovery::config cfg;
    cfg.query = {k_server, server().port, shared_name};
    cfg.zone = k_zone;
    cfg.tsig_key_name = server().tsig_name;
    cfg.tsig_key_base64 = server().tsig_secret;
    return cfg;
}

auto rfc6763_ldns_cfg() -> kythira::rfc6763_ldns_peer_discovery::config {
    kythira::rfc6763_ldns_peer_discovery::config cfg;
    cfg.query = {k_server, server().port, "_raft._tcp.cluster.example.test."};
    cfg.zone = k_cluster_zone;
    cfg.domain_service_name = "_raft._tcp.example.test.";
    cfg.domain_zone = k_zone;
    cfg.tsig_key_name = server().tsig_name;
    cfg.tsig_key_base64 = server().tsig_secret;
    return cfg;
}

auto dns_sd_cfg() -> kythira::rfc2136_dns_sd_discovery::config {
    kythira::rfc2136_dns_sd_discovery::config cfg;
    cfg.server = k_server;
    cfg.port = server().port;
    cfg.zone = k_zone;
    cfg.service_domain = k_zone;
    cfg.service_type = "_kythira-it._tcp";
    cfg.tsig_key_name = server().tsig_name;
    cfg.tsig_key_base64 = server().tsig_secret;
    return cfg;
}

}  // namespace

// ── rfc2136_ldns_discovery ──────────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(rfc2136_ldns)

// Two nodes share one name. Each sees the other but not itself, and when one
// deregisters the other's record survives: the RFC 2136 §2.5.4 single-RR
// delete, not a §2.5.2 RRset delete that would wipe every node at once.
BOOST_AUTO_TEST_CASE(tsig_signed_register_and_deregister_per_node, *boost::unit_test::timeout(30)) {
    const std::string name = "raft-a.example.test.";
    kythira::rfc1035_peer_discovery reader({k_server, server().port, name});

    auto a = std::make_unique<kythira::rfc2136_ldns_discovery>(rfc2136_cfg(name));
    auto b = std::make_unique<kythira::rfc2136_ldns_discovery>(rfc2136_cfg(name));
    std::move(a->register_node("a", "10.1.0.1")).get();
    std::move(b->register_node("b", "10.1.0.2")).get();

    BOOST_TEST(query(name, LDNS_RR_TYPE_A) == (std::vector<std::string>{"10.1.0.1", "10.1.0.2"}),
               boost::test_tools::per_element());
    BOOST_TEST(sorted_addresses(std::move(reader.find_peers(2s)).get()) ==
                   (std::vector<std::string>{"10.1.0.1", "10.1.0.2"}),
               boost::test_tools::per_element());
    BOOST_TEST(sorted_addresses(std::move(a->find_peers(2s)).get()) ==
                   std::vector<std::string>{"10.1.0.2"},
               boost::test_tools::per_element());

    a.reset();
    BOOST_TEST(query(name, LDNS_RR_TYPE_A) == std::vector<std::string>{"10.1.0.2"},
               boost::test_tools::per_element());
    b.reset();
    BOOST_TEST(query(name, LDNS_RR_TYPE_A).empty());
}

BOOST_AUTO_TEST_CASE(ipv6_address_registers_as_aaaa, *boost::unit_test::timeout(30)) {
    const std::string name = "raft-v6.example.test.";
    {
        kythira::rfc2136_ldns_discovery node(rfc2136_cfg(name));
        std::move(node.register_node("v6", "fd00::1")).get();
        BOOST_TEST(query(name, LDNS_RR_TYPE_AAAA) == std::vector<std::string>{"fd00::1"},
                   boost::test_tools::per_element());
        BOOST_TEST(query(name, LDNS_RR_TYPE_A).empty());
    }
    BOOST_TEST(query(name, LDNS_RR_TYPE_AAAA).empty());
}

// The zone accepts only updates signed with the fixture's key, so these two
// prove the TSIG path is what made the tests above succeed.
BOOST_AUTO_TEST_CASE(update_signed_with_wrong_secret_is_refused, *boost::unit_test::timeout(30)) {
    const std::string name = "raft-badkey.example.test.";
    auto cfg = rfc2136_cfg(name);
    cfg.tsig_key_base64 = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=";
    kythira::rfc2136_ldns_discovery node(cfg);
    BOOST_CHECK_THROW(std::move(node.register_node("x", "10.1.0.9")).get(), std::runtime_error);
    BOOST_TEST(query(name, LDNS_RR_TYPE_A).empty());
}

BOOST_AUTO_TEST_CASE(unsigned_update_is_refused, *boost::unit_test::timeout(30)) {
    const std::string name = "raft-nokey.example.test.";
    auto cfg = rfc2136_cfg(name);
    cfg.tsig_key_name.clear();
    cfg.tsig_key_base64.clear();
    kythira::rfc2136_ldns_discovery node(cfg);
    BOOST_CHECK_THROW(std::move(node.register_node("x", "10.1.0.9")).get(), std::runtime_error);
    BOOST_TEST(query(name, LDNS_RR_TYPE_A).empty());
}

BOOST_AUTO_TEST_SUITE_END()

// ── rfc6763_ldns_peer_discovery ─────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(rfc6763_ldns)

// Requirement 5: one registration writes four records across two zones (a
// PTR, the instance SRV and the cluster-level SRV in the cluster zone, and the
// domain-level SRV in the parent zone), each read back from the server, and
// deregistration removes all four.
BOOST_AUTO_TEST_CASE(register_writes_four_records_across_two_zones,
                     *boost::unit_test::timeout(30)) {
    const auto cfg = rfc6763_ldns_cfg();
    const std::string instance = "n1." + cfg.query.service_name;
    const std::string srv = "10 0 7001 n1-host.example.test.";
    {
        kythira::rfc6763_ldns_peer_discovery node(cfg);
        std::move(node.register_node("n1", "n1-host.example.test.:7001")).get();

        BOOST_TEST(
            query(cfg.query.service_name, LDNS_RR_TYPE_PTR) == std::vector<std::string>{instance},
            boost::test_tools::per_element());
        BOOST_TEST(query(instance, LDNS_RR_TYPE_SRV) == std::vector<std::string>{srv},
                   boost::test_tools::per_element());
        BOOST_TEST(query(cfg.query.service_name, LDNS_RR_TYPE_SRV) == std::vector<std::string>{srv},
                   boost::test_tools::per_element());
        BOOST_TEST(
            query(cfg.domain_service_name, LDNS_RR_TYPE_SRV) == std::vector<std::string>{srv},
            boost::test_tools::per_element());
    }
    BOOST_TEST(query(cfg.query.service_name, LDNS_RR_TYPE_PTR).empty());
    BOOST_TEST(query(instance, LDNS_RR_TYPE_SRV).empty());
    BOOST_TEST(query(cfg.query.service_name, LDNS_RR_TYPE_SRV).empty());
    BOOST_TEST(query(cfg.domain_service_name, LDNS_RR_TYPE_SRV).empty());
}

BOOST_AUTO_TEST_CASE(peers_see_each_other_but_not_themselves, *boost::unit_test::timeout(30)) {
    const auto cfg = rfc6763_ldns_cfg();
    kythira::rfc6763_ldns_peer_discovery n1(cfg);
    kythira::rfc6763_ldns_peer_discovery n2(cfg);
    std::move(n1.register_node("p1", "p1-host.example.test.:7001")).get();
    std::move(n2.register_node("p2", "p2-host.example.test.:7002")).get();

    const auto from_n1 = sorted_addresses(std::move(n1.find_peers(2s)).get());
    BOOST_REQUIRE_EQUAL(from_n1.size(), 1U);
    BOOST_TEST(from_n1.front().find("p2-host.example.test") == 0U);
    BOOST_TEST(from_n1.front().ends_with(":7002"));

    kythira::rfc6763_peer_discovery domain_reader(
        {k_server, server().port, cfg.domain_service_name});
    BOOST_TEST(std::move(domain_reader.find_peers(2s)).get().size() == 2U);
}

BOOST_AUTO_TEST_SUITE_END()

// ── rfc2136_dns_sd_discovery ────────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(rfc2136_dns_sd)

BOOST_AUTO_TEST_CASE(nodes_browse_each_other_and_deregister, *boost::unit_test::timeout(30)) {
    const auto cfg = dns_sd_cfg();
    const std::string browse = cfg.service_type + "." + cfg.service_domain;

    auto a = std::make_unique<kythira::rfc2136_dns_sd_discovery>(cfg);
    kythira::rfc2136_dns_sd_discovery b(cfg);
    std::move(a->register_node("sd-a", "sd-a-host:7101")).get();
    std::move(b.register_node("sd-b", "sd-b-host:7102")).get();

    BOOST_TEST(query(browse, LDNS_RR_TYPE_PTR).size() == 2U);

    const auto from_b = std::move(b.find_peers(2s)).get();
    BOOST_REQUIRE_EQUAL(from_b.size(), 1U);
    BOOST_TEST(from_b.front().node_id == "sd-a");
    BOOST_TEST(from_b.front().address.ends_with(":7101"));

    a.reset();
    const std::string a_instance = "sd-a." + browse;
    BOOST_TEST(query(a_instance, LDNS_RR_TYPE_SRV).empty());
    BOOST_TEST(query(a_instance, LDNS_RR_TYPE_TXT).empty());
    BOOST_TEST(query(browse, LDNS_RR_TYPE_PTR) == std::vector<std::string>{"sd-b." + browse},
               boost::test_tools::per_element());
    BOOST_TEST(std::move(b.find_peers(2s)).get().empty());
}

BOOST_AUTO_TEST_SUITE_END()

// ── ACME dns-01 ─────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(acme_dns01)

// The full dns-01 round trip: the provider publishes the TXT record by
// TSIG-signed UPDATE, acme_test_server validates it by querying the same
// server, the certificate is issued, and the challenge record is gone again
// afterwards (Requirement 18.4).
BOOST_AUTO_TEST_CASE(sign_csr_via_dns01_against_bind, *boost::unit_test::timeout(60)) {
    using namespace raft::testing;
    const std::string identifier = "acme-node.example.test";

    acme_test_server::options server_opts;
    server_opts.dns01_resolver_address = k_server;
    server_opts.dns01_resolver_port = server().port;
    acme_test_server acme{server_opts};

    acme_certificate_provider_config config;
    config.directory_url = acme.directory_url();
    config.challenge = acme_certificate_provider_config::challenge_type::dns_01;
    config.dns01.server = k_server;
    config.dns01.port = server().port;
    config.dns01.zone = k_zone;
    config.dns01.tsig_key_name = server().tsig_name;
    config.dns01.tsig_key_base64 = server().tsig_secret;
    config.poll_timeout = std::chrono::seconds(20);
    config.poll_interval = std::chrono::milliseconds(200);
    acme_certificate_provider provider(config);

    leaf_certificate_options leaf_opts;
    leaf_opts.subject.common_name = identifier;
    leaf_opts.dns_names = {identifier};
    auto csr = generate_key_and_csr(leaf_opts);
    csr_signing_options sign_opts;
    sign_opts.dns_names = {identifier};
    sign_opts.server_auth = true;

    auto material = provider.sign_csr(csr.csr_pem, sign_opts).get();
    BOOST_TEST(!material.certificate_pem.empty());

    BIO* bio = BIO_new_mem_buf(material.certificate_pem.data(),
                               static_cast<int>(material.certificate_pem.size()));
    X509* leaf = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    BOOST_REQUIRE(leaf != nullptr);
    BOOST_TEST(X509_check_host(leaf, identifier.data(), identifier.size(), 0, nullptr) == 1);
    X509_free(leaf);

    BOOST_TEST(query("_acme-challenge." + identifier + ".", LDNS_RR_TYPE_TXT).empty());
}

BOOST_AUTO_TEST_SUITE_END()
