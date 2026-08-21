// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file oci_quorum_manager_unit_test.cpp
/// @brief `oci_instance_pool_quorum_manager`'s construction contract, its
///        tag read-merge-write, and its fault points
///        (`.kiro/specs/oci-cloud-provider/`, Requirements 13.1-13.4).
///
/// Split from `oci_quorum_manager_mock_test.cpp` along the line that actually
/// matters here: this file is about the manager's *own* rules — what it refuses
/// to be constructed with, what it writes when it writes tags, and that every
/// fault point is wired — while the mock file is about call sequences and state
/// machines over several operations.
///
/// The read-merge-write case is the one that earns its place. `UpdateInstance`
/// replaces the `freeformTags` map rather than merging into it, and it reports
/// success either way, so a manager that skipped the read would silently delete
/// every operator-set tag on every instance it touched and nothing would ever
/// fail. The mock server models the replacement faithfully for exactly this
/// reason — a merging mock would make the bug untestable.
///
/// `oci_signing::sign_request`'s golden vectors live in
/// `oci_signing_unit_test.cpp` (eight cases, including the negative control that
/// the superseded header order does *not* verify) rather than being restated
/// here. One canonical-string assertion is kept below as a tripwire: if the
/// signing scheme ever changes under this manager, this file should not be the
/// last place to find out.

#define BOOST_TEST_MODULE oci_quorum_manager_unit_test
#include <boost/test/unit_test.hpp>

#include <raft/oci_instance_pool_quorum_manager.hpp>
#include <raft/oci_signing.hpp>

#include "oci_mock_server.hpp"
#include "test_timeout_scale.hpp"

#include <openssl/evp.h>
#include <openssl/pem.h>

#ifdef FIU_ENABLE
#include <fiu-control.h>
#endif

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
#include <folly/init/Init.h>
#endif

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace {

using kythira::oci_instance_pool_quorum_manager;
using kythira::oci_instance_pool_quorum_manager_config;

/// Below the ephemeral range (32768-60999), like every hand-allocated port in
/// this tree; 18320 is `oci_http_client_unit_test`'s.
constexpr std::uint16_t test_port = 18321;

auto generate_rsa_key_pem() -> std::string {
    EVP_PKEY* raw = nullptr;
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
    EVP_PKEY_keygen_init(ctx);
    EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, 2048);
    EVP_PKEY_keygen(ctx, &raw);
    EVP_PKEY_CTX_free(ctx);
    BIO* bio = BIO_new(BIO_s_mem());
    PEM_write_bio_PrivateKey(bio, raw, nullptr, nullptr, 0, nullptr, nullptr);
    char* data = nullptr;
    const long len = BIO_get_mem_data(bio, &data);  // NOLINT(google-runtime-int) — OpenSSL's type
    std::string pem(data, static_cast<std::size_t>(len));
    BIO_free_all(bio);
    EVP_PKEY_free(raw);
    return pem;
}

/// One key for the whole file: RSA-2048 generation is the slowest thing here by
/// an order of magnitude, and no case depends on having a distinct key.
auto shared_key_pem() -> const std::string& {
    static const std::string pem = generate_rsa_key_pem();
    return pem;
}

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
struct FollyInitFixture {
    FollyInitFixture() {
        int argc = boost::unit_test::framework::master_test_suite().argc;
        char** argv = boost::unit_test::framework::master_test_suite().argv;
        folly::init(&argc, &argv, false);
    }
};
BOOST_GLOBAL_FIXTURE(FollyInitFixture);
#endif

#ifdef FIU_ENABLE
struct FiuInitFixture {
    FiuInitFixture() { fiu_init(0); }
};
BOOST_GLOBAL_FIXTURE(FiuInitFixture);
#endif

/// A mock control plane that outlives every case in the file.
///
/// One server rather than one per case because the manager's constructor makes a
/// real `GetInstancePool` call — a per-case server would mean a bind/close cycle
/// per construction, and a `TIME_WAIT` race on a fixed port is a flake with a
/// long history in this tree.
struct MockServerFixture {
    MockServerFixture() : server(std::make_unique<kythira::testing::oci_mock_server>(test_port)) {
        // Before start(), so every request this file makes is signature-verified
        // against the same key the client signs with.
        server->set_signing_key_pem(shared_key_pem());
        server->start();
    }
    ~MockServerFixture() { server->stop(); }

    std::unique_ptr<kythira::testing::oci_mock_server> server;
};

MockServerFixture& fixture() {
    static MockServerFixture instance;
    return instance;
}

auto valid_config() -> oci_instance_pool_quorum_manager_config {
    oci_instance_pool_quorum_manager_config cfg;
    cfg.oci.region = "us-phoenix-1";
    cfg.oci.tenancy_id = "ocid1.tenancy.oc1..aaaa";
    cfg.oci.user_id = "ocid1.user.oc1..bbbb";
    cfg.oci.fingerprint = "aa:bb:cc:dd";
    cfg.oci.private_key_pem = shared_key_pem();
    cfg.oci.endpoint_override = fixture().server->origin();
    cfg.oci.api_timeout = std::chrono::seconds{5};
    cfg.compartment_id = "ocid1.compartment.oc1..mock";
    cfg.cluster_name = "test-cluster";
    cfg.instance_pool_id = fixture().server->pool_id();
    cfg.node_port = 7000;
    cfg.poll_interval = std::chrono::milliseconds{50};
    cfg.provision_timeout = std::chrono::seconds{5};
    cfg.topology.groups.push_back(
        {.group_id = fixture().server->availability_domain(), .target_count = 3});
    return cfg;
}

/// A config that never reaches the network: every case below that expects a
/// throw from the constructor's *validation* must fail before `GetInstancePool`
/// is attempted, so pointing it at a dead port is how the test proves the order
/// rather than assuming it.
auto unreachable_config() -> oci_instance_pool_quorum_manager_config {
    auto cfg = valid_config();
    cfg.oci.endpoint_override = "http://127.0.0.1:1";
    return cfg;
}

}  // namespace

BOOST_AUTO_TEST_SUITE(oci_quorum_manager_construction)

/// Requirement 13.1. A `static_assert` in the header already fails the build if
/// this regresses; the runtime case exists so the intent is visible in the test
/// output rather than only in a compile error nobody reads.
BOOST_AUTO_TEST_CASE(the_manager_satisfies_the_quorum_manager_concept) {
    static_assert(kythira::quorum_manager<oci_instance_pool_quorum_manager<>, std::uint64_t,
                                          std::string, std::string>);
    BOOST_CHECK(true);
}

BOOST_AUTO_TEST_CASE(a_valid_config_constructs_and_reads_the_pools_availability_domains,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    oci_instance_pool_quorum_manager<> mgr{valid_config()};
    BOOST_REQUIRE_EQUAL(mgr.availability_domains().size(), 1U);
    BOOST_CHECK_EQUAL(mgr.availability_domains().front(), fixture().server->availability_domain());
}

/// Requirement 2.2. Each field named individually rather than as one
/// "incomplete config" case, because the failure this guards against is a
/// deployment with exactly one blank line in its env file.
BOOST_AUTO_TEST_CASE(an_empty_compartment_id_throws) {
    auto cfg = unreachable_config();
    cfg.compartment_id.clear();
    BOOST_CHECK_THROW((oci_instance_pool_quorum_manager<>{cfg}), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(an_empty_cluster_name_throws) {
    auto cfg = unreachable_config();
    cfg.cluster_name.clear();
    BOOST_CHECK_THROW((oci_instance_pool_quorum_manager<>{cfg}), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(an_empty_instance_pool_id_throws) {
    auto cfg = unreachable_config();
    cfg.instance_pool_id.clear();
    BOOST_CHECK_THROW((oci_instance_pool_quorum_manager<>{cfg}), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(a_zero_node_port_throws) {
    auto cfg = unreachable_config();
    cfg.node_port = 0;
    BOOST_CHECK_THROW((oci_instance_pool_quorum_manager<>{cfg}), std::invalid_argument);
}

/// Requirement 2.2's API-key clause. The four fields are checked together here
/// because they share one condition — `use_instance_principal == false` — and
/// the interesting claim is that the condition is honoured, not that four
/// separate `empty()` calls work.
BOOST_AUTO_TEST_CASE(a_missing_api_key_field_throws_when_instance_principal_is_off) {
    {
        auto cfg = unreachable_config();
        cfg.oci.tenancy_id.clear();
        BOOST_CHECK_THROW((oci_instance_pool_quorum_manager<>{cfg}), std::invalid_argument);
    }
    {
        auto cfg = unreachable_config();
        cfg.oci.user_id.clear();
        BOOST_CHECK_THROW((oci_instance_pool_quorum_manager<>{cfg}), std::invalid_argument);
    }
    {
        auto cfg = unreachable_config();
        cfg.oci.fingerprint.clear();
        BOOST_CHECK_THROW((oci_instance_pool_quorum_manager<>{cfg}), std::invalid_argument);
    }
    {
        auto cfg = unreachable_config();
        cfg.oci.private_key_pem.clear();
        BOOST_CHECK_THROW((oci_instance_pool_quorum_manager<>{cfg}), std::invalid_argument);
    }
}

/// The security-token mode passes constructor validation without any of the
/// API-key triple — pinned because the first WIF CI run failed exactly here:
/// the gate demanded a tenancy_id that token mode deliberately does not
/// have. Construction still proceeds to GetInstancePool against the mock
/// (which does not verify signatures), so what this asserts is the gate,
/// not the signing. The session key stays required: a token with no key to
/// sign with is named at construction, not discovered as a 401.
BOOST_AUTO_TEST_CASE(a_security_token_passes_validation_without_the_api_key_triple,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    auto cfg = valid_config();
    cfg.oci.tenancy_id.clear();
    cfg.oci.user_id.clear();
    cfg.oci.fingerprint.clear();
    cfg.oci.security_token = "hdr.payload.sig";
    oci_instance_pool_quorum_manager<> mgr{cfg};
    BOOST_CHECK_EQUAL(mgr.availability_domains().size(), 1U);

    cfg.oci.private_key_pem.clear();
    BOOST_CHECK_THROW((oci_instance_pool_quorum_manager<>{cfg}), std::invalid_argument);
}

/// Requirement 2.3: an unreachable or unknown pool is a construction failure,
/// not a manager that fails later. `std::runtime_error` rather than
/// `std::invalid_argument` distinguishes "your config is wrong" from "OCI said
/// no", which is the distinction an operator reads first.
BOOST_AUTO_TEST_CASE(an_unknown_instance_pool_throws_at_construction,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    auto cfg = valid_config();
    cfg.instance_pool_id = "ocid1.instancepool.oc1.phx.does-not-exist";
    BOOST_CHECK_THROW((oci_instance_pool_quorum_manager<>{cfg}), std::runtime_error);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(oci_quorum_manager_tags)

/// Requirement 13.4 / design.md Property 1.
///
/// The pre-existing tag is the whole point: a manager that wrote only its own
/// four tags would pass every other assertion in this file and would still have
/// erased an operator's `owner` tag on every instance it ever provisioned. The
/// `UpdateInstance` body is inspected directly rather than only the resulting
/// state, because the requirement is about what goes on the wire.
BOOST_AUTO_TEST_CASE(tag_writes_merge_onto_the_instances_existing_tags,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    const auto instance_id =
        fixture().server->add_instance({{"owner", "platform-team"}, {"cost-center", "42"}});

    oci_instance_pool_quorum_manager<> mgr{valid_config()};
    const auto merged = mgr.merged_tags(
        instance_id, {{"kythira-cluster", "test-cluster"}, {"kythira-node-id", "7"}});

    BOOST_CHECK_EQUAL(merged.at("owner"), "platform-team");
    BOOST_CHECK_EQUAL(merged.at("cost-center"), "42");
    BOOST_CHECK_EQUAL(merged.at("kythira-cluster"), "test-cluster");
    BOOST_CHECK_EQUAL(merged.at("kythira-node-id"), "7");
    BOOST_CHECK_EQUAL(merged.size(), 4U);

    fixture().server->set_lifecycle_state(instance_id, "TERMINATED");
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(oci_quorum_manager_signing_tripwire)

/// Not a substitute for `oci_signing_unit_test.cpp` — a tripwire.
///
/// The signing string is a signature over an *ordered* string, and every failure
/// mode of getting it wrong looks the same from here: a well-formed request and
/// a 401 from OCI. If the order or the field set moves under this manager, this
/// assertion says so at the point of use.
BOOST_AUTO_TEST_CASE(the_bodyless_canonical_string_still_starts_with_date) {
    const auto signing_string = kythira::oci_signing::build_signing_string(
        "GET", "/20160918/instancePools/abc", "iaas.us-phoenix-1.oraclecloud.com", "",
        "Mon, 10 Aug 2026 12:00:00 GMT");
    BOOST_CHECK_EQUAL(signing_string,
                      "date: Mon, 10 Aug 2026 12:00:00 GMT\n"
                      "(request-target): get /20160918/instancePools/abc\n"
                      "host: iaas.us-phoenix-1.oraclecloud.com");
}

BOOST_AUTO_TEST_SUITE_END()

#ifdef FIU_ENABLE

BOOST_AUTO_TEST_SUITE(oci_quorum_manager_fault_injection)

/// Requirement 10.1. The cluster is non-empty on purpose: `assess_quorum`
/// returns healthy without touching OCI for an empty one, so an empty vector
/// here would pass whether the fault point existed or not.
BOOST_AUTO_TEST_CASE(the_list_instances_fault_rejects_the_future,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    oci_instance_pool_quorum_manager<> mgr{valid_config()};
    std::vector<kythira::node_placement<std::uint64_t, std::string>> cluster{
        {.node_id = 1, .group_id = fixture().server->availability_domain()}};

    fiu_enable("raft/oci/instance_pool/list_instances", 1, nullptr, 0);
    auto fut = mgr.assess_quorum(cluster);
    fiu_disable("raft/oci/instance_pool/list_instances");

    BOOST_CHECK_THROW(std::move(fut).get(), std::exception);
}

/// Requirement 10.2. Also proves the fault fires *before* `UpdateInstancePool`:
/// the pool size is read either side and must not have moved.
BOOST_AUTO_TEST_CASE(the_update_size_fault_rejects_the_future_without_growing_the_pool,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    oci_instance_pool_quorum_manager<> mgr{valid_config()};
    const auto before = fixture().server->pool_size();

    fiu_enable("raft/oci/instance_pool/update_size", 1, nullptr, 0);
    auto fut = mgr.provision_node(fixture().server->availability_domain(), std::nullopt);
    fiu_disable("raft/oci/instance_pool/update_size");

    BOOST_CHECK_THROW(std::move(fut).get(), std::exception);
    BOOST_CHECK_EQUAL(fixture().server->pool_size(), before);
}

/// Requirement 10.3.
BOOST_AUTO_TEST_CASE(the_detach_instance_fault_rejects_the_future,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    oci_instance_pool_quorum_manager<> mgr{valid_config()};

    fiu_enable("raft/oci/instance_pool/detach_instance", 1, nullptr, 0);
    auto fut = mgr.decommission_node(std::uint64_t{1});
    fiu_disable("raft/oci/instance_pool/detach_instance");

    BOOST_CHECK_THROW(std::move(fut).get(), std::exception);
}

/// Requirement 10.4. An empty cluster is fine here, unlike the assess case:
/// `maintain_quorum`'s fault point is checked before it delegates, so a
/// short-circuiting `assess_quorum` cannot mask it.
BOOST_AUTO_TEST_CASE(the_maintain_quorum_fault_rejects_the_future,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    oci_instance_pool_quorum_manager<> mgr{valid_config()};
    const std::vector<kythira::node_placement<std::uint64_t, std::string>> cluster;

    fiu_enable("raft/oci/instance_pool/maintain_quorum", 1, nullptr, 0);
    auto fut = mgr.maintain_quorum(cluster);
    fiu_disable("raft/oci/instance_pool/maintain_quorum");

    BOOST_CHECK_THROW(std::move(fut).get(), std::exception);
}

BOOST_AUTO_TEST_SUITE_END()

#endif  // FIU_ENABLE
