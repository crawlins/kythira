// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file oci_http_client_unit_test.cpp
/// @brief `oci_http_client` against a real local HTTP server
///        (`.kiro/specs/oci-cloud-provider/`, Requirements 1.7-1.9).
///
/// A hand-rolled `httplib::Server` rather than a fake injected in place of the
/// HTTP layer, because most of what this class does *is* the HTTP layer: which
/// host it dials, which headers it puts on the wire, how it reads a status and
/// a `retry-after`. A test that stubbed out `httplib` would be asserting
/// against the stub's idea of those, which is the same idea the test author
/// had — so it would agree with itself and pin nothing.
///
/// This is a down-payment on Task 4's `oci_mock_server.hpp` rather than that
/// file itself: it covers the transport's own behaviour, not the OCI API
/// surface the quorum manager and certificate provider will need modelled.
///
/// The 429 case is the one worth reading carefully. It asserts an **attempt
/// count**, not just that the call eventually succeeded: a client that never
/// retried and a client that retried forever would both be visible only in how
/// many requests the server saw.

#define BOOST_TEST_MODULE oci_http_client_unit_test
#include <boost/test/unit_test.hpp>

#include <raft/oci_http_client.hpp>

#include "test_timeout_scale.hpp"

#include <openssl/evp.h>
#include <openssl/pem.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using kythira::oci_client_config;
using kythira::oci_http_client;

/// Below the ephemeral range (32768-60999), like every other hand-allocated
/// port in this tree — the kernel is free to hand out anything inside it as a
/// source port, which is how `grpc_transport_integration_test` failed on main.
/// Port 0 is not usable here: the config needs the port in `endpoint_override`
/// before the server is asked what it bound.
constexpr std::uint16_t test_port = 18320;
constexpr const char* bind_address = "127.0.0.1";

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

auto make_config(const std::string& pem) -> oci_client_config {
    oci_client_config cfg;
    cfg.region = "us-phoenix-1";
    cfg.tenancy_id = "ocid1.tenancy.oc1..aaaa";
    cfg.user_id = "ocid1.user.oc1..bbbb";
    cfg.fingerprint = "aa:bb:cc:dd";
    cfg.private_key_pem = pem;
    cfg.endpoint_override = std::string("http://") + bind_address + ":" + std::to_string(test_port);
    cfg.api_timeout = std::chrono::seconds{5};
    return cfg;
}

/// A server that answers whatever the case installs, and records what arrived.
class recording_server {
public:
    using responder = std::function<void(const httplib::Request&, httplib::Response&)>;

    explicit recording_server(responder handler) : _handler(std::move(handler)) {
        auto dispatch = [this](const httplib::Request& req, httplib::Response& resp) {
            {
                const std::lock_guard<std::mutex> lock(_mutex);
                _requests.push_back(req);
            }
            _handler(req, resp);
        };
        _server.Get(".*", dispatch);
        _server.Post(".*", dispatch);
        _server.Put(".*", dispatch);
        _server.Delete(".*", dispatch);
    }

    auto start() -> void {
        _thread = std::thread([this] { _server.listen(bind_address, test_port); });
        _server.wait_until_ready();
    }

    auto stop() -> void {
        _server.stop();
        if (_thread.joinable()) {
            _thread.join();
        }
    }

    [[nodiscard]] auto count() const -> std::size_t {
        const std::lock_guard<std::mutex> lock(_mutex);
        return _requests.size();
    }

    [[nodiscard]] auto at(std::size_t i) const -> httplib::Request {
        const std::lock_guard<std::mutex> lock(_mutex);
        return _requests.at(i);
    }

private:
    httplib::Server _server;
    std::thread _thread;
    responder _handler;
    mutable std::mutex _mutex;
    std::vector<httplib::Request> _requests;
};

}  // namespace

BOOST_AUTO_TEST_SUITE(oci_http_client_tests)

/// Requirement 1.7's host derivation, asserted without issuing a request.
///
/// The override replaces the host for *every* service rather than per service.
/// That asymmetry is deliberate and worth pinning: it is what lets one mock
/// server stand in for `iaas` and `certificatesmanagement` at once.
BOOST_AUTO_TEST_CASE(the_service_host_is_derived_from_the_region_unless_overridden) {
    const auto pem = generate_rsa_key_pem();
    oci_client_config cfg = make_config(pem);
    cfg.endpoint_override.clear();
    const oci_http_client client(cfg);

    // Per-service, not one template — established by probing live OCI. Core
    // Services predates the `oci` label and 404s `GetVnic` under it; the
    // certificate services have no DNS record without it. Getting this wrong
    // is invisible to every other test here, since `endpoint_override` replaces
    // the whole host.
    BOOST_CHECK_EQUAL(client.host_for("iaas"), "iaas.us-phoenix-1.oraclecloud.com");
    BOOST_CHECK_EQUAL(client.host_for("certificatesmanagement"),
                      "certificatesmanagement.us-phoenix-1.oci.oraclecloud.com");
    BOOST_CHECK_EQUAL(client.host_for("certificates"),
                      "certificates.us-phoenix-1.oci.oraclecloud.com");

    oci_client_config overridden = make_config(pem);
    overridden.endpoint_override = "http://127.0.0.1:9999";
    const oci_http_client mocked(overridden);
    BOOST_CHECK_EQUAL(mocked.host_for("iaas"), "http://127.0.0.1:9999");
    BOOST_CHECK_EQUAL(mocked.host_for("certificatesmanagement"), "http://127.0.0.1:9999");
}

/// The request sets `Host` **itself**, to exactly the string it signed.
///
/// This is the regression test for a defect that reached live OCI: `Host` is a
/// signed header, and leaving cpp-httplib to supply it does not guarantee the
/// two agree. httplib computes it in `ClientImpl`'s constructor initializer list
/// via `make_host_and_port_string(host_, port, is_ssl())`, and `is_ssl()` is
/// virtual — during base-class construction it resolves to the non-SSL override
/// and returns `false`, so port 443 is not treated as default and `:443` is
/// appended. The client signed the bare hostname, Oracle verified against
/// `…:443`, and every real call came back `NotAuthenticated: Failed to verify
/// the HTTP(S) Signature`.
///
/// Asserted against an https origin with no explicit port, because that is the
/// only shape where httplib's default-port handling is in play — and note that
/// no local server can stand in here: a mock necessarily listens on a
/// non-default port, where both spellings agree and the bug is invisible. That
/// is exactly why 31 green tests missed it. What makes this non-vacuous is the
/// `Host` **key being present at all**: before the fix the map had none.
BOOST_AUTO_TEST_CASE(the_request_sets_host_itself_to_the_string_it_signed) {
    const auto pem = generate_rsa_key_pem();
    oci_client_config cfg = make_config(pem);
    cfg.endpoint_override.clear();
    const oci_http_client client(cfg);

    const auto headers =
        client.prepared_headers(client.host_for("iaas"), "GET", "/20160918/instancePools/abc", "");

    BOOST_REQUIRE_MESSAGE(headers.contains("Host"),
                          "no explicit Host header — httplib would choose it, and for a default "
                          "port it appends ':443' to a host the signature does not cover");
    BOOST_CHECK_EQUAL(headers.at("Host"), "iaas.us-phoenix-1.oraclecloud.com");
    // ...and the signature must be over that same spelling, not the origin.
    BOOST_CHECK(headers.at("authorization").find("headers=\"date (request-target) host\"") !=
                std::string::npos);
}

/// Every request carries the signed headers, and the `host` that was signed is
/// the bare hostname rather than the origin. Signing the origin would sign a
/// string the service never sees, so this checks the header the *server*
/// received rather than anything the client reports about itself.
BOOST_AUTO_TEST_CASE(every_request_is_signed,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    recording_server server([](const httplib::Request&, httplib::Response& resp) {
        resp.status = 200;
        resp.set_content(R"({"id":"ocid1.instancepool.oc1..x"})", "application/json");
    });
    server.start();

    const auto pem = generate_rsa_key_pem();
    oci_http_client client(make_config(pem));
    const auto body = client.request("iaas", "GET", "/20160918/instancePools/abc");

    BOOST_REQUIRE_EQUAL(server.count(), 1U);
    const auto req = server.at(0);
    BOOST_CHECK(req.has_header("authorization"));
    BOOST_CHECK(req.has_header("date"));
    BOOST_CHECK(req.get_header_value("authorization").starts_with(R"(Signature version="1",)"));
    // The signed `host` is the bare hostname:port, with the scheme stripped.
    BOOST_CHECK_EQUAL(req.get_header_value("Host"),
                      std::string(bind_address) + ":" + std::to_string(test_port));
    // A bodyless GET must not carry the body-derived headers.
    BOOST_CHECK(!req.has_header("x-content-sha256"));

    BOOST_CHECK_EQUAL(std::string(body.at("id").as_string()), "ocid1.instancepool.oc1..x");

    server.stop();
}

/// A POST signs and sends the body-derived headers too.
BOOST_AUTO_TEST_CASE(a_post_carries_the_body_derived_signed_headers,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    recording_server server([](const httplib::Request&, httplib::Response& resp) {
        resp.status = 200;
        resp.set_content(R"({"ok":true})", "application/json");
    });
    server.start();

    const auto pem = generate_rsa_key_pem();
    oci_http_client client(make_config(pem));
    const std::string payload = R"({"size":5})";
    (void)client.request("iaas", "POST", "/20160918/instancePools/abc/actions/softreset", payload);

    BOOST_REQUIRE_EQUAL(server.count(), 1U);
    const auto req = server.at(0);
    BOOST_CHECK(req.has_header("x-content-sha256"));
    BOOST_CHECK_EQUAL(req.get_header_value("content-type"), "application/json");
    BOOST_CHECK_EQUAL(req.body, payload);
    BOOST_CHECK(req.get_header_value("authorization").find("x-content-sha256") !=
                std::string::npos);

    server.stop();
}

/// Requirement 1.8: OCI's own `code` and `message` reach the caller.
///
/// Both are checked, plus the status. A message alone would not tell a caller
/// whether to retry; a status alone discards what OCI actually said.
BOOST_AUTO_TEST_CASE(a_non_2xx_surfaces_ocis_code_and_message,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    recording_server server([](const httplib::Request&, httplib::Response& resp) {
        resp.status = 404;
        resp.set_content(R"({"code":"NotAuthorizedOrNotFound","message":"Authorization failed"})",
                         "application/json");
    });
    server.start();

    const auto pem = generate_rsa_key_pem();
    oci_http_client client(make_config(pem));

    std::string what;
    try {
        (void)client.request("iaas", "GET", "/20160918/instancePools/missing");
        BOOST_FAIL("expected a non-2xx to throw");
    } catch (const std::runtime_error& e) {
        what = e.what();
    }
    BOOST_CHECK_MESSAGE(what.find("NotAuthorizedOrNotFound") != std::string::npos,
                        "error did not carry OCI's code: " << what);
    BOOST_CHECK_MESSAGE(what.find("Authorization failed") != std::string::npos,
                        "error did not carry OCI's message: " << what);
    BOOST_CHECK_MESSAGE(what.find("404") != std::string::npos,
                        "error did not carry the status: " << what);

    server.stop();
}

/// An error body that is not the documented shape still yields something
/// usable. This is the case where a caller most needs the raw text — an HTML
/// error page from a proxy, say — and where "could not parse the error" would
/// be the least helpful possible response.
BOOST_AUTO_TEST_CASE(a_non_json_error_body_is_not_swallowed,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    recording_server server([](const httplib::Request&, httplib::Response& resp) {
        resp.status = 502;
        resp.set_content("<html>gateway blew up</html>", "text/html");
    });
    server.start();

    const auto pem = generate_rsa_key_pem();
    oci_http_client client(make_config(pem));
    std::string what;
    try {
        (void)client.request("iaas", "GET", "/20160918/instancePools/abc");
        BOOST_FAIL("expected a 502 to throw");
    } catch (const std::runtime_error& e) {
        what = e.what();
    }
    BOOST_CHECK(what.find("502") != std::string::npos);
    BOOST_CHECK_MESSAGE(what.find("gateway blew up") != std::string::npos,
                        "the raw error body was discarded: " << what);

    server.stop();
}

/// Requirement 1.9: retried exactly once, **and the server's `retry-after` is
/// actually honoured**.
///
/// Two separate claims, and the second is easy to test vacuously. The default
/// delay is 1 second, so a `retry-after: 1` would elapse identically whether
/// the header was read or ignored entirely — the obvious version of this test
/// proves only the first claim. The header therefore says **3**, and the
/// elapsed time has to reflect it: under 2.5s means the value was ignored and
/// the default used.
///
/// The count is the other half. A client that never retried and one that
/// retried without bound both return successfully here; only the number of
/// requests the server saw tells them apart.
BOOST_AUTO_TEST_CASE(a_429_is_retried_exactly_once_after_the_servers_own_delay,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {
    constexpr int server_delay_seconds = 3;
    std::atomic<int> seen{0};
    recording_server server([&seen](const httplib::Request&, httplib::Response& resp) {
        if (seen.fetch_add(1) == 0) {
            resp.status = 429;
            resp.set_header("retry-after", std::to_string(server_delay_seconds));
            resp.set_content(R"({"code":"TooManyRequests","message":"slow down"})",
                             "application/json");
            return;
        }
        resp.status = 200;
        resp.set_content(R"({"ok":true})", "application/json");
    });
    server.start();

    const auto pem = generate_rsa_key_pem();
    oci_http_client client(make_config(pem));
    const auto start = std::chrono::steady_clock::now();
    const auto body = client.request("iaas", "GET", "/20160918/instancePools/abc");
    const auto elapsed = std::chrono::steady_clock::now() - start;

    BOOST_CHECK(body.at("ok").as_bool());
    BOOST_CHECK_EQUAL(server.count(), 2U);
    // Lower bound only — an upper bound would be asserting the runner's
    // scheduling, which is not this class's behaviour.
    BOOST_CHECK_MESSAGE(
        elapsed >= std::chrono::milliseconds{2500},
        "waited " << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()
                  << "ms, so retry-after: " << server_delay_seconds
                  << " was ignored in favour of the 1s default");

    server.stop();
}

/// A second 429 propagates — the retry is bounded, not a loop. Without this, a
/// throttled control plane could turn one quorum poll into an unbounded stall,
/// and the case above cannot tell the difference.
BOOST_AUTO_TEST_CASE(a_second_429_propagates_as_an_error,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    recording_server server([](const httplib::Request&, httplib::Response& resp) {
        resp.status = 429;
        resp.set_header("retry-after", "1");
        resp.set_content(R"({"code":"TooManyRequests","message":"still throttled"})",
                         "application/json");
    });
    server.start();

    const auto pem = generate_rsa_key_pem();
    oci_http_client client(make_config(pem));
    std::string what;
    try {
        (void)client.request("iaas", "GET", "/20160918/instancePools/abc");
        BOOST_FAIL("expected the second 429 to throw");
    } catch (const std::runtime_error& e) {
        what = e.what();
    }
    BOOST_CHECK(what.find("429") != std::string::npos);
    BOOST_CHECK_MESSAGE(what.find("still throttled") != std::string::npos,
                        "the second 429's message was lost: " << what);
    // Exactly two: the original and one retry. Three would mean the bound
    // moved; one would mean the retry never happened.
    BOOST_CHECK_EQUAL(server.count(), 2U);

    server.stop();
}

/// A `retry-after` this class will not trust falls back to the default rather
/// than being honoured. The header is remote input: an HTTP-date form needs a
/// clock and a skew argument this class should not be making, and an absurd
/// value would convert throttling into an outage. The observable consequence
/// is that the call still completes promptly — asserted as elapsed time, since
/// honouring `"Wed, 21 Oct 2026 07:28:00 GMT"` as a number is the bug.
BOOST_AUTO_TEST_CASE(an_unusable_retry_after_does_not_stall_the_call,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    std::atomic<int> seen{0};
    recording_server server([&seen](const httplib::Request&, httplib::Response& resp) {
        if (seen.fetch_add(1) == 0) {
            resp.status = 429;
            resp.set_header("retry-after", "Wed, 21 Oct 2026 07:28:00 GMT");
            resp.set_content(R"({"code":"TooManyRequests","message":"slow down"})",
                             "application/json");
            return;
        }
        resp.status = 200;
        resp.set_content(R"({"ok":true})", "application/json");
    });
    server.start();

    const auto pem = generate_rsa_key_pem();
    oci_http_client client(make_config(pem));
    const auto start = std::chrono::steady_clock::now();
    const auto body = client.request("iaas", "GET", "/20160918/instancePools/abc");
    const auto elapsed = std::chrono::steady_clock::now() - start;

    BOOST_CHECK(body.at("ok").as_bool());
    BOOST_CHECK_EQUAL(server.count(), 2U);
    // Under 2.5s is the 1s default rather than anything derived from the
    // header — the same threshold the honouring case above uses as its lower
    // bound, so the two cases are each other's control: one must exceed it,
    // this one must not. A `21` scraped out of that date string would land
    // above it, and a full date parse further still.
    BOOST_CHECK_MESSAGE(
        elapsed < std::chrono::milliseconds{2500},
        "waited " << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()
                  << "ms, so something was parsed out of an unusable retry-after");

    server.stop();
}

/// A 2xx with no body is a success with nothing to say, which several OCI
/// delete/terminate operations are. Returning a JSON null keeps that distinct
/// from a body that was present and unparseable — the case below.
BOOST_AUTO_TEST_CASE(an_empty_2xx_body_yields_null_rather_than_throwing,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    recording_server server(
        [](const httplib::Request&, httplib::Response& resp) { resp.status = 204; });
    server.start();

    const auto pem = generate_rsa_key_pem();
    oci_http_client client(make_config(pem));
    const auto body = client.request("iaas", "DELETE", "/20160918/instancePools/abc");
    BOOST_CHECK(body.is_null());

    server.stop();
}

/// ...and a 2xx whose body is present but not JSON throws, rather than
/// returning something the caller would then have to re-check. The two cases
/// are stated together because it is the *distinction* that matters.
/// Every OCI endpoint is https, and cpp-httplib only has an https client when
/// it was compiled with `CPPHTTPLIB_OPENSSL_SUPPORT` — which in this tree only
/// `CONFIG_HTTP_TRANSPORT_TLS` defines, via `network_simulator`'s interface.
///
/// This target deliberately does not link `network_simulator`, so it is one of
/// the few places that can observe the TLS-less build at all. That matters:
/// every *other* OCI test points `endpoint_override` at a plain-http local
/// server, so the entire suite stays green in a configuration where no real OCI
/// call could ever succeed. `configs/minimal_defconfig` sets
/// `CONFIG_HTTP_TRANSPORT_TLS=n`, so this is a reachable configuration, not a
/// hypothetical one.
///
/// The two branches are each other's control. Without TLS the failure must name
/// the flag; with it, the same call must fail as an ordinary transport error and
/// not trip the guard — a guard that fired in both builds would be worse than
/// none, since it would send a reader to a flag that was already set.
#ifndef CPPHTTPLIB_OPENSSL_SUPPORT
BOOST_AUTO_TEST_CASE(an_https_endpoint_without_tls_support_names_the_flag_to_set,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    const auto pem = generate_rsa_key_pem();
    oci_client_config cfg = make_config(pem);
    cfg.endpoint_override = "https://iaas.us-phoenix-1.oraclecloud.com";
    const oci_http_client client(cfg);

    std::string what;
    try {
        (void)client.request("iaas", "GET", "/20160918/instancePools/abc");
        BOOST_FAIL("expected an https origin to fail in a build without TLS support");
    } catch (const std::exception& e) {
        what = e.what();
    }
    BOOST_CHECK_MESSAGE(what.find("CONFIG_HTTP_TRANSPORT_TLS") != std::string::npos,
                        "the error did not name the flag to set: " << what);
    BOOST_CHECK_MESSAGE(what.find("oci_http_client") != std::string::npos,
                        "the error did not say which component failed: " << what);
}
#else
BOOST_AUTO_TEST_CASE(an_unreachable_https_endpoint_fails_as_transport_not_as_the_tls_guard,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    const auto pem = generate_rsa_key_pem();
    oci_client_config cfg = make_config(pem);
    // Port 1 refuses immediately; nothing here depends on network access.
    cfg.endpoint_override = "https://127.0.0.1:1";
    const oci_http_client client(cfg);

    std::string what;
    try {
        (void)client.request("iaas", "GET", "/20160918/instancePools/abc");
        BOOST_FAIL("expected a connection to port 1 to fail");
    } catch (const std::exception& e) {
        what = e.what();
    }
    BOOST_CHECK_MESSAGE(what.find("CONFIG_HTTP_TRANSPORT_TLS") == std::string::npos,
                        "the TLS guard fired in a build that has TLS support: " << what);
}
#endif

BOOST_AUTO_TEST_CASE(an_unparseable_2xx_body_throws,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    recording_server server([](const httplib::Request&, httplib::Response& resp) {
        resp.status = 200;
        resp.set_content("not json at all", "text/plain");
    });
    server.start();

    const auto pem = generate_rsa_key_pem();
    oci_http_client client(make_config(pem));
    BOOST_CHECK_THROW((void)client.request("iaas", "GET", "/20160918/instancePools/abc"),
                      std::runtime_error);

    server.stop();
}

BOOST_AUTO_TEST_SUITE_END()
