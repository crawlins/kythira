// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file oci_heartbeat_writer_unit_test.cpp
/// @brief `oci_heartbeat_writer` against `oci_mock_server` (Requirement 4.4).
///
/// What this tier pins: the read-merge-write cycle (Requirement 4.2 — a
/// heartbeat write must not clobber the manager's identity tags or an
/// operator's), the written value's format (decimal Unix timestamp,
/// Requirement 4.4), instance-id discovery from the metadata service, and the
/// background loop's survival of API failures. What it deliberately does not
/// re-test: Instance Principal signing and routing — the writer talks through
/// `oci_http_client`, whose auth-mode selection is `oci_federation_unit_test`'s
/// contract; every request here is still signature-verified, just under the
/// API-key mode the mock understands.
///
/// Ports 18325 (mock control plane) and 18326 (metadata stub) — next free
/// slots in the OCI block (18320-18324: http client, signing, quorum mock,
/// certificates, federation).

#define BOOST_TEST_MODULE oci_heartbeat_writer_unit_test
#include <boost/test/unit_test.hpp>

#include <raft/oci_heartbeat_writer.hpp>

#include "oci_mock_server.hpp"
#include "test_timeout_scale.hpp"

#include <openssl/evp.h>
#include <openssl/pem.h>

#include <httplib.h>

#include <chrono>
#include <cstdint>
#include <ctime>
#include <map>
#include <string>
#include <thread>

namespace {

using kythira::oci_heartbeat_writer;
using kythira::oci_heartbeat_writer_config;

constexpr std::uint16_t test_port = 18325;
constexpr std::uint16_t metadata_port = 18326;

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

auto shared_key_pem() -> const std::string& {
    static const std::string pem = generate_rsa_key_pem();
    return pem;
}

/// A fresh mock control plane plus a writer config pointed at it.
struct MockFixture {
    MockFixture() : server(test_port) {
        server.set_signing_key_pem(shared_key_pem());
        server.start();
    }
    ~MockFixture() { server.stop(); }

    MockFixture(const MockFixture&) = delete;
    auto operator=(const MockFixture&) -> MockFixture& = delete;
    MockFixture(MockFixture&&) = delete;
    auto operator=(MockFixture&&) -> MockFixture& = delete;

    [[nodiscard]] auto config(std::string instance_id) const -> oci_heartbeat_writer_config {
        oci_heartbeat_writer_config cfg;
        cfg.client.region = "us-phoenix-1";
        cfg.client.tenancy_id = "ocid1.tenancy.oc1..aaaa";
        cfg.client.user_id = "ocid1.user.oc1..bbbb";
        cfg.client.fingerprint = "aa:bb:cc:dd";
        cfg.client.private_key_pem = shared_key_pem();
        cfg.client.endpoint_override = server.origin();
        cfg.client.api_timeout = std::chrono::seconds{5};
        cfg.instance_id = std::move(instance_id);
        return cfg;
    }

    kythira::testing::oci_mock_server server;
};

}  // namespace

BOOST_AUTO_TEST_SUITE(oci_heartbeat_writer_tests)

/// Requirements 4.2 + 4.4: the write is read-merge-write, so every tag that
/// was on the instance before the heartbeat — the manager's identity tags, an
/// operator's own — survives it, and the heartbeat value is the decimal Unix
/// timestamp `assess_quorum` parses.
BOOST_FIXTURE_TEST_CASE(write_once_stamps_heartbeat_and_preserves_tags, MockFixture,
                        *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {
    const auto id = server.add_instance(
        {
            {"kythira-cluster", "mock-cluster"},
            {"kythira-node-id", "7"},
            {"operator-set", "keep-me"},
        },
        "RUNNING");

    oci_heartbeat_writer writer(config(id));
    const std::time_t stamped = writer.write_once(1'770'000'000);
    BOOST_CHECK_EQUAL(stamped, 1'770'000'000);
    BOOST_CHECK_EQUAL(writer.writes(), 1);

    const auto state = server.instance(id);
    BOOST_REQUIRE(state.has_value());
    BOOST_CHECK_EQUAL(state->freeform_tags.at("kythira-last-heartbeat"), "1770000000");
    BOOST_CHECK_EQUAL(state->freeform_tags.at("kythira-cluster"), "mock-cluster");
    BOOST_CHECK_EQUAL(state->freeform_tags.at("kythira-node-id"), "7");
    BOOST_CHECK_EQUAL(state->freeform_tags.at("operator-set"), "keep-me");
}

/// A second beat replaces the first — the tag is a timestamp, not a history.
BOOST_FIXTURE_TEST_CASE(second_write_advances_the_timestamp, MockFixture,
                        *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {
    const auto id = server.add_instance({{"kythira-node-id", "9"}}, "RUNNING");

    oci_heartbeat_writer writer(config(id));
    (void)writer.write_once(1'770'000'000);
    (void)writer.write_once(1'770'000'010);

    const auto state = server.instance(id);
    BOOST_REQUIRE(state.has_value());
    BOOST_CHECK_EQUAL(state->freeform_tags.at("kythira-last-heartbeat"), "1770000010");
    BOOST_CHECK_EQUAL(writer.writes(), 2);
}

/// An empty `instance_id` means "this instance": the OCID comes from the
/// metadata service's `/instance/id`, with the `Bearer Oracle` header the v2
/// service requires — the same discovery a real node performs under Instance
/// Principal, pointed here at a stub.
BOOST_FIXTURE_TEST_CASE(instance_id_discovered_from_metadata, MockFixture,
                        *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {
    const auto id = server.add_instance({{"kythira-node-id", "11"}}, "RUNNING");

    httplib::Server metadata;
    metadata.Get("/opc/v2/instance/id", [&](const httplib::Request& req, httplib::Response& resp) {
        if (req.get_header_value("Authorization") != "Bearer Oracle") {
            resp.status = 401;
            return;
        }
        resp.set_content(id, "text/plain");
    });
    std::thread metadata_thread([&] { metadata.listen("127.0.0.1", metadata_port); });
    metadata.wait_until_ready();

    auto cfg = config(/*instance_id=*/"");
    cfg.metadata_base_url = "http://127.0.0.1:" + std::to_string(metadata_port) + "/opc/v2";
    oci_heartbeat_writer writer(cfg);
    (void)writer.write_once(1'770'000'000);

    const auto state = server.instance(id);
    BOOST_REQUIRE(state.has_value());
    BOOST_CHECK_EQUAL(state->freeform_tags.at("kythira-last-heartbeat"), "1770000000");

    metadata.stop();
    metadata_thread.join();
}

/// The background loop: an immediate first write, then one per interval —
/// and an API failure is counted and reported, never fatal to the loop
/// (a missed heartbeat is `assess_quorum`'s signal, not this class's crash).
BOOST_FIXTURE_TEST_CASE(background_loop_beats_and_survives_failures, MockFixture,
                        *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {
    const auto id = server.add_instance({{"kythira-node-id", "13"}}, "RUNNING");

    auto cfg = config(id);
    cfg.interval = std::chrono::seconds{1};
    oci_heartbeat_writer writer(cfg);
    writer.start();

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{30};
    while (writer.writes() < 2 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
    }
    writer.stop();
    BOOST_CHECK_GE(writer.writes(), 2);
    BOOST_CHECK_EQUAL(writer.last_error(), "");

    // Now a writer aimed at an instance the control plane has never heard of:
    // the loop keeps running, counting failures rather than dying on the
    // first, and the error is inspectable.
    auto failing_cfg = config("ocid1.instance.oc1.phx.nosuchinstance");
    failing_cfg.interval = std::chrono::seconds{1};
    oci_heartbeat_writer failing(failing_cfg);
    failing.start();
    const auto fail_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{30};
    while (failing.write_failures() < 2 && std::chrono::steady_clock::now() < fail_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
    }
    failing.stop();
    BOOST_CHECK_GE(failing.write_failures(), 2);
    BOOST_CHECK_EQUAL(failing.writes(), 0);
    BOOST_CHECK_NE(failing.last_error(), "");
}

/// A direct `write_once` against a missing instance throws rather than
/// counting silently — the synchronous caller asked and deserves the answer.
BOOST_FIXTURE_TEST_CASE(write_once_throws_on_api_failure, MockFixture,
                        *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {
    oci_heartbeat_writer writer(config("ocid1.instance.oc1.phx.nosuchinstance"));
    BOOST_CHECK_THROW((void)writer.write_once(1'770'000'000), std::exception);
    BOOST_CHECK_EQUAL(writer.writes(), 0);
}

BOOST_AUTO_TEST_SUITE_END()
