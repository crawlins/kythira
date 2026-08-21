// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#define BOOST_TEST_MODULE oci_object_storage_mock_conformance_test
#include <boost/test/unit_test.hpp>

#include "object_store_conformance.hpp"

#include <raft/oci_object_storage_client.hpp>

#include "oci_mock_server.hpp"

#include <openssl/evp.h>
#include <openssl/pem.h>

// The engine's conformance suite, run over OCI Object Storage's mock tier
// (cloud-object-persistence task 15.4).
//
// The point of this tier is that the SAME suite that proves the engine over an
// in-memory map proves it over a real HTTP round trip, real request signing,
// real JSON error bodies and real listing pagination — so "the engine works"
// stops being a claim about `std::map`.
//
// ## What this tier can and cannot prove
//
// It CAN catch a client that signs the wrong canonical form: `oci_mock_server`
// verifies the signature with the public key, against the bytes that arrived.
//
// It CANNOT catch either of the two defects task 16 found against the real
// service, and that is structural rather than an omission:
//
//   * the endpoint derivation is never exercised, because `endpoint_override`
//     replaces the host outright — so a wrong `objectstorage.` suffix is
//     invisible here;
//   * the signature check verifies what THIS server reconstructs, so a client
//     and mock that encode a request identically agree however wrongly they
//     both encode it — which is exactly how a percent-encoded `/` in a query
//     value passed every local tier and failed live.
//
// Recorded here rather than in the commit message because the next person to
// add a provider mock will otherwise assume this tier covers more than it does.

using namespace kythira;

namespace {

/// One RSA key for the whole binary: the client signs with it and the mock
/// verifies with its public half, which is what makes the verification real
/// rather than a second copy of the client's own arithmetic.
auto shared_key_pem() -> const std::string& {
    static const std::string pem = [] {
        EVP_PKEY* key = EVP_RSA_gen(2048);
        BIO* bio = BIO_new(BIO_s_mem());
        PEM_write_bio_PrivateKey(bio, key, nullptr, nullptr, 0, nullptr, nullptr);
        char* data = nullptr;
        const long length = BIO_get_mem_data(bio, &data);
        std::string out(data, static_cast<std::size_t>(length));
        BIO_free(bio);
        EVP_PKEY_free(key);
        return out;
    }();
    return pem;
}

/// Conformance harness over a live local OCI mock server.
///
/// No failure injection: `oci_mock_server` has no knobs for it, so this
/// instantiates the NO_INJECTION subset. That is honest rather than
/// convenient — a harness that silently skipped the retry cases while claiming
/// the full suite would misreport what had been proved.
/// A fixed port, matching what the other OCI mock suites do. `origin()` echoes
/// whatever the constructor was given, so an ephemeral 0 would produce
/// `http://127.0.0.1:0` — which fails with "could not establish connection" and
/// looks like a client bug rather than a harness one. Distinct from
/// oci_quorum_manager_mock_test's 18322 so the two can run concurrently under
/// `ctest -j`.
constexpr std::uint16_t k_test_port = 18327;

struct oci_mock_harness {
    using engine_t = object_store_persistence_engine<oci_object_storage_client>;

    testing::oci_mock_server server{k_test_port};

    oci_mock_harness() {
        // Before start(), so every request this harness makes is signature-
        // verified against the key the client signs with.
        server.set_signing_key_pem(shared_key_pem());
        server.start();
    }
    ~oci_mock_harness() { server.stop(); }

    oci_mock_harness(const oci_mock_harness&) = delete;
    auto operator=(const oci_mock_harness&) -> oci_mock_harness& = delete;
    oci_mock_harness(oci_mock_harness&&) = delete;
    auto operator=(oci_mock_harness&&) -> oci_mock_harness& = delete;

    [[nodiscard]] auto client() const -> oci_object_storage_client {
        oci_object_storage_config cfg;
        cfg.oci.region = "us-phoenix-1";
        cfg.oci.tenancy_id = "ocid1.tenancy.oc1..mock";
        cfg.oci.user_id = "ocid1.user.oc1..mock";
        cfg.oci.fingerprint = "aa:bb:cc";
        cfg.oci.private_key_pem = shared_key_pem();
        cfg.oci.endpoint_override = server.origin();
        cfg.namespace_name = "axunmw4f0mln";
        return oci_object_storage_client{cfg};
    }

    [[nodiscard]] auto bucket() const -> std::string { return "kythira-bucket"; }
    [[nodiscard]] auto prefix() const -> std::string { return "node-1"; }

    [[nodiscard]] auto make_engine() const -> engine_t {
        return engine_t{client(), bucket(), prefix()};
    }
    [[nodiscard]] auto make_engine(const std::string& b, const std::string& p) const -> engine_t {
        return engine_t{client(), b, p};
    }
    [[nodiscard]] auto make_engine(object_persistence_options opts) const -> engine_t {
        return engine_t{client(), bucket(), prefix(), std::move(opts)};
    }

    // Inspection goes straight into the server's map rather than over HTTP.
    // The suite counts requests around engine operations and calls `keys()`
    // before counting, so an inspecting LIST would be indistinguishable from
    // one the engine made — and would make the engine look like it listed
    // twice.
    auto seed(const std::string& key, std::string_view body) -> void {
        server.seed_object(bucket(), key, body);
    }
    [[nodiscard]] auto keys() const -> std::vector<std::string> {
        return server.object_keys(bucket());
    }
    [[nodiscard]] auto has(const std::string& key) const -> bool {
        return server.object_has(bucket(), key);
    }
    [[nodiscard]] auto body(const std::string& key) const -> std::string {
        return server.object_body(bucket(), key);
    }

    // The request log the conformance suite counts against. It is the mock
    // *server's* log — what actually arrived over HTTP — rather than anything
    // the client recorded about itself, which is the whole reason this tier is
    // worth more than the in-memory one for these particular cases.
    [[nodiscard]] auto request_log() const -> std::vector<std::string> {
        return server.object_requests();
    }
    [[nodiscard]] auto count_requests(std::string_view verb) const -> std::size_t {
        return server.count_object_requests(verb);
    }
    auto clear_requests() -> void { server.clear_object_requests(); }
};

}  // namespace

KYTHIRA_OBJECT_STORE_CONFORMANCE_NO_INJECTION(oci_mock_harness, oci_object_storage_client)
