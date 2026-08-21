// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#define BOOST_TEST_MODULE gcp_gcs_client_unit_test
#include <boost/test/unit_test.hpp>

#include <raft/gcp_gcs_client.hpp>

// Unit coverage for `gcp_gcs_client` (cloud-object-persistence Requirement 14,
// task 9). Every assertion is made against the bytes that **actually arrived**
// at a local server rather than against the client's own fields: the whole
// value of this client is what it puts on the wire, and a test that read the
// client's configuration back would pass just as happily if nothing were sent.
//
// The four load-bearing cases, in order of what they would cost if wrong:
//
//  1. **A 404 is only absence when it names an absent object.** GCS answers a
//     missing object and a missing *bucket* with the same status and the same
//     machine-readable `reason: "notFound"` — measured against live GCS, not
//     read out of documentation. Only the message differs. Reading a
//     misconfigured bucket as "this key is not written yet" would present to the
//     engine as an empty Raft log, which is the one failure it cannot detect at
//     runtime, so absence is a whitelist and everything else throws.
//  2. **412 latches and nothing else does.** A conditional write that is refused
//     for any other reason must raise an ordinary exception; mapping it to
//     `object_precondition_failed` turns a transient failure into a permanent,
//     unclearable fence latch.
//  3. **Exactly one request per call.** google-cloud-cpp's *default* storage
//     retry policy is a 15-minute `LimitedTimeRetryPolicy`, so this is not
//     "zero instead of three" but "zero instead of a quarter-hour loop". The
//     count is taken against an endpoint answering a status the library treats
//     as retryable (503 → `kUnavailable`); with the policy line removed this
//     case does not fail so much as *hang*, which is why the ctest target
//     carries a TIMEOUT and why that timeout is part of the test.
//  4. **The conditional DELETE answers a 404 two different ways.** `if_absent`
//     succeeds on one (the precondition held) and `if_version` throws (the
//     object the caller expected is gone, so it lost the race). Collapsing the
//     two would silently swallow exactly what the fence exists to detect.
//
// **On credentials, and why this suite does not use the storage emulator
// variable.** `CLOUD_STORAGE_EMULATOR_ENDPOINT` looks like the obvious way to
// point a storage client at a local server, and it is a trap: it *overrides* an
// explicit `RestEndpointOption`, so a suite that set it would exercise the
// environment variable and never the `endpoint_override` field this client
// actually reads. Measured, before this file was written — with the variable
// set to one local port and the client configured for another, every request
// arrived at the variable's port. Instead each case builds a **throwaway
// service account whose RSA key is generated at run time**, which
// google-cloud-cpp turns into a locally-signed JWT with no token-endpoint round
// trip. That exercises the real `credentials_json` branch, needs no network,
// and puts no key material in the repository.

#ifdef KYTHIRA_HAS_GCP_STORAGE

#include <httplib.h>

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>

#include <chrono>
#include <cstddef>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace kythira;
using namespace std::chrono_literals;

namespace {

// ── A throwaway service account, minted in-process ───────────────────────────

/// A fresh 2048-bit RSA private key in PEM form.
///
/// Generated rather than embedded on purpose: a private key checked into the
/// tree is a finding for the repository's secret scanner however clearly it is
/// labelled a test fixture, and this costs one key generation per binary.
auto generate_private_key_pem() -> std::string {
    EVP_PKEY* pkey = EVP_RSA_gen(2048);
    BOOST_REQUIRE(pkey != nullptr);
    BIO* bio = BIO_new(BIO_s_mem());
    BOOST_REQUIRE(bio != nullptr);
    BOOST_REQUIRE(PEM_write_bio_PrivateKey(bio, pkey, nullptr, nullptr, 0, nullptr, nullptr) == 1);
    char* data = nullptr;
    const long length = BIO_get_mem_data(bio, &data);
    std::string pem(data, static_cast<std::size_t>(length));
    BIO_free(bio);
    EVP_PKEY_free(pkey);
    return pem;
}

auto json_escape(std::string_view raw) -> std::string {
    std::string out;
    out.reserve(raw.size() + 32);
    for (const char c : raw) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                out += c;
                break;
        }
    }
    return out;
}

/// One service account for the whole binary — the key generation is the only
/// slow thing in this file, and every case wants the same answer from it.
auto fake_service_account_json() -> const std::string& {
    static const std::string json = [] {
        return std::string(R"({"type":"service_account",)"
                           R"("project_id":"kythira-unit-test",)"
                           R"("private_key_id":"0000000000000000000000000000000000000000",)"
                           R"("private_key":")") +
               json_escape(generate_private_key_pem()) +
               std::string(
                   R"(",)"
                   R"("client_email":"unit-test@kythira-unit-test.iam.gserviceaccount.com",)"
                   R"("client_id":"000000000000000000000",)"
                   R"("token_uri":"https://oauth2.googleapis.com/token"})");
    }();
    return json;
}

// ── The local stand-in for GCS ───────────────────────────────────────────────

/// What the server saw. Captured rather than asserted in the handler: a failed
/// `BOOST_TEST` on the server thread reports against the wrong test, and the
/// interesting assertions want to run after the call has returned anyway.
struct recorded_request {
    std::string method;
    std::string path;
    std::string target;  // path + query, undecoded
    std::string body;
    httplib::Params params;

    [[nodiscard]] auto param(const std::string& name) const -> std::string {
        const auto it = params.find(name);
        return it == params.end() ? std::string{} : it->second;
    }

    [[nodiscard]] auto has_param(const std::string& name) const -> bool {
        return params.find(name) != params.end();
    }

    /// The upload path carries the object metadata as one MIME part, so the
    /// fields this suite checks (`md5Hash`) are looked for in the whole body.
    [[nodiscard]] auto body_contains(std::string_view needle) const -> bool {
        return body.find(needle) != std::string::npos;
    }
};

/// Records every request and answers with whatever the test programmed. It
/// performs no JWT verification — google-cloud-cpp's credential handling is not
/// what this suite is testing.
struct MockGcs {
    httplib::Server server;
    std::thread thread;
    int port{0};

    mutable std::mutex mu;
    std::vector<recorded_request> log;

    /// Answers the Nth request (0-based); the last entry answers every
    /// subsequent one, so a single-element vector is "always this".
    std::vector<std::function<void(const recorded_request&, httplib::Response&)>> responders;

    MockGcs() {
        auto handler = [this](const httplib::Request& req, httplib::Response& res) {
            recorded_request rec{req.method, req.path, req.target, req.body, req.params};
            std::size_t index = 0;
            {
                const std::lock_guard lock(mu);
                index = log.size();
                log.push_back(rec);
            }
            if (responders.empty()) {
                res.status = 200;
                return;
            }
            const auto pick = index < responders.size() ? index : responders.size() - 1;
            responders[pick](rec, res);
        };
        server.Post("/.*", handler);
        server.Get("/.*", handler);
        server.Put("/.*", handler);
        server.Delete("/.*", handler);
    }

    void start() {
        port = server.bind_to_any_port("127.0.0.1");
        thread = std::thread([this] { server.listen_after_bind(); });
        for (int i = 0; i < 200 && !server.is_running(); ++i) {
            std::this_thread::sleep_for(5ms);
        }
    }

    ~MockGcs() {
        server.stop();
        if (thread.joinable()) {
            thread.join();
        }
    }

    MockGcs(const MockGcs&) = delete;
    auto operator=(const MockGcs&) -> MockGcs& = delete;
    MockGcs(MockGcs&&) = delete;
    auto operator=(MockGcs&&) -> MockGcs& = delete;

    [[nodiscard]] auto origin() const -> std::string {
        return "http://127.0.0.1:" + std::to_string(port);
    }

    [[nodiscard]] auto requests() const -> std::vector<recorded_request> {
        const std::lock_guard lock(mu);
        return log;
    }

    [[nodiscard]] auto request_count() const -> std::size_t {
        const std::lock_guard lock(mu);
        return log.size();
    }
};

auto config_for(const MockGcs& mock) -> gcp_client_config {
    gcp_client_config cfg;
    cfg.project_id = "kythira-unit-test";
    cfg.credentials_json = fake_service_account_json();
    cfg.endpoint_override = mock.origin();
    cfg.api_timeout = 5s;
    return cfg;
}

// ── Response bodies, in the shapes GCS actually sends ────────────────────────

/// The error envelope of the GCS JSON API. Copied from live responses (see the
/// file comment): it is the `reason`, not the status class, that this client
/// keys its precondition verdict on, and it is the `message` — uncomfortably —
/// that carries the only object-vs-bucket discriminator GCS offers.
auto gcs_error_json(int code, const std::string& reason, const std::string& message)
    -> std::string {
    return R"({"error":{"code":)" + std::to_string(code) + R"(,"message":")" + message +
           R"(","errors":[{"message":")" + message + R"(","domain":"global","reason":")" + reason +
           R"("}]}})";
}

/// The two 404s, verbatim in shape from live GCS.
auto absent_object_json(const std::string& bucket, const std::string& key) -> std::string {
    return gcs_error_json(404, "notFound", "No such object: " + bucket + "/" + key);
}

auto absent_bucket_json() -> std::string {
    return gcs_error_json(404, "notFound", "The specified bucket does not exist.");
}

auto condition_not_met_json() -> std::string {
    return gcs_error_json(412, "conditionNotMet",
                          "At least one of the pre-conditions you specified did not hold.");
}

/// An object resource. `generation` and `size` are **strings** in the JSON API
/// even though both are integers, which is exactly the sort of thing a
/// hand-written mock gets wrong and then agrees with itself about.
auto object_json(const std::string& name, const std::string& generation,
                 const std::string& size = "5") -> std::string {
    return R"({"kind":"storage#object","bucket":"kythira-bucket","name":")" + name +
           R"(","generation":")" + generation +
           R"(","metageneration":"1",)"
           R"("contentType":"application/octet-stream","size":")" +
           size +
           R"(",)"
           R"("etag":"CL/I06jPqJYDEAE=","md5Hash":"XUFAKrxLKna5cZ2REBfFkg=="})";
}

auto listing_json(const std::vector<std::string>& names, const std::string& next_token = {})
    -> std::string {
    std::string body = R"({"kind":"storage#objects","items":[)";
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (i != 0) {
            body += ',';
        }
        body += object_json(names[i], std::to_string(1000 + i));
    }
    body += ']';
    if (!next_token.empty()) {
        body += R"(,"nextPageToken":")" + next_token + R"(")";
    }
    body += '}';
    return body;
}

/// Programs the mock to answer a media download.
auto media_responder(const std::string& body, const std::string& generation)
    -> std::function<void(const recorded_request&, httplib::Response&)> {
    return [body, generation](const recorded_request&, httplib::Response& res) {
        res.status = 200;
        res.set_header("x-goog-generation", generation);
        res.set_header("x-goog-metageneration", "1");
        res.set_content(body, "application/octet-stream");
    };
}

auto json_responder(int status, const std::string& body)
    -> std::function<void(const recorded_request&, httplib::Response&)> {
    return [status, body](const recorded_request&, httplib::Response& res) {
        res.status = status;
        res.set_content(body, "application/json");
    };
}

/// A response with the status and no payload at all — the shape that leaves
/// google-cloud-cpp with a status code and no `reason` to read.
auto bare_status_responder(int status)
    -> std::function<void(const recorded_request&, httplib::Response&)> {
    return [status](const recorded_request&, httplib::Response& res) { res.status = status; };
}

constexpr const char* k_bucket = "kythira-bucket";

}  // namespace

// ── The concept obligations, over the real type ──────────────────────────────
//
// The header asserts these beside the definition too. Repeated here because a
// concept satisfied only in the header's own translation unit is not the
// property callers depend on.

BOOST_AUTO_TEST_SUITE(gcp_gcs_concepts)

BOOST_AUTO_TEST_CASE(satisfies_the_store_concepts) {
    static_assert(key_object_store<gcp_gcs_client>);
    static_assert(conditional_key_object_store<gcp_gcs_client>);
    BOOST_TEST(gcp_gcs_client::provider_name() == "gcs");
}

BOOST_AUTO_TEST_CASE(is_not_a_content_md5_versioned_store) {
    // GCS's version is a generation counter and its ETag is an opaque token
    // (`CL/I06jPqJYDEAE=`, measured). A client that declared this trait would
    // make the engine compare an opaque token against an MD5 and fail every
    // single write, so the negative is pinned rather than left implicit.
    static_assert(!content_md5_versioned_store<gcp_gcs_client>);
}

BOOST_AUTO_TEST_SUITE_END()

// ── put_object ───────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(gcp_gcs_put)

BOOST_AUTO_TEST_CASE(put_uploads_to_the_upload_path_and_returns_the_generation) {
    MockGcs mock;
    mock.responders = {json_responder(200, object_json("logs/000001", "1786971917733520"))};
    mock.start();

    const gcp_gcs_client client{config_for(mock)};
    const auto result = client.put_object(k_bucket, "logs/000001", "hello");

    // The version is the generation, as a decimal string — not the ETag, which
    // this same response carries and which is a different thing entirely.
    BOOST_TEST(result.version == "1786971917733520");

    const auto requests = mock.requests();
    BOOST_REQUIRE_EQUAL(requests.size(), 1U);
    BOOST_TEST(requests[0].method == "POST");
    BOOST_TEST(requests[0].path.find("/upload/storage/v1/b/kythira-bucket/o") != std::string::npos);
    BOOST_TEST(requests[0].body_contains("hello"));
}

BOOST_AUTO_TEST_CASE(put_sends_the_content_md5_as_base64_not_hex) {
    // Requirement 7.1: the service verifies the digest. The encoding is base64
    // of the raw 16 bytes; confusing it with the hex spelling is the obvious
    // mistake, so the expected value is computed here from the same content by
    // an independent route rather than copied from the client.
    MockGcs mock;
    mock.responders = {json_responder(200, object_json("k", "7"))};
    mock.start();

    const std::string content = "the quick brown fox";
    const auto expected = google::cloud::storage::ComputeMD5Hash(content);
    // Pin the shape too, so a change that made both sides agree on something
    // wrong would still be visible: base64 of 16 bytes is 24 chars ending "==".
    BOOST_TEST(expected.size() == 24U);
    BOOST_TEST(expected.ends_with("=="));

    const gcp_gcs_client client{config_for(mock)};
    client.put_object(k_bucket, "k", content);

    const auto requests = mock.requests();
    BOOST_REQUIRE_EQUAL(requests.size(), 1U);
    BOOST_TEST(requests[0].body_contains("md5Hash"));
    BOOST_TEST(requests[0].body_contains(expected));
}

BOOST_AUTO_TEST_CASE(put_failure_throws_naming_the_operation_and_the_service_message) {
    MockGcs mock;
    mock.responders = {
        json_responder(400, gcs_error_json(400, "invalid", "Provided MD5 hash does not match"))};
    mock.start();

    const gcp_gcs_client client{config_for(mock)};
    try {
        client.put_object(k_bucket, "k", "hello");
        BOOST_FAIL("expected put_object to throw");
    } catch (const object_precondition_failed&) {
        BOOST_FAIL("a 400 must not be reported as a precondition failure");
    } catch (const std::runtime_error& err) {
        const std::string what = err.what();
        BOOST_TEST(what.find("InsertObject") != std::string::npos);
        BOOST_TEST(what.find("invalid") != std::string::npos);
        BOOST_TEST(what.find("Provided MD5 hash does not match") != std::string::npos);
    }
}

BOOST_AUTO_TEST_CASE(put_issues_exactly_one_request_against_a_retryable_failure) {
    // 503 maps to `kUnavailable`, which the library's default retry policy
    // treats as transient — so with `LimitedErrorCountRetryPolicy(0)` removed
    // this does not merely count wrong, it retries for fifteen minutes. The
    // ctest TIMEOUT is what turns that into a failure rather than a hang.
    MockGcs mock;
    mock.responders = {bare_status_responder(503)};
    mock.start();

    const gcp_gcs_client client{config_for(mock)};
    BOOST_CHECK_THROW(client.put_object(k_bucket, "k", "hello"), std::runtime_error);
    BOOST_TEST(mock.request_count() == 1U);
}

BOOST_AUTO_TEST_SUITE_END()

// ── put_object_if ────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(gcp_gcs_conditional_put)

BOOST_AUTO_TEST_CASE(if_absent_sends_if_generation_match_zero) {
    MockGcs mock;
    mock.responders = {json_responder(200, object_json("k", "5"))};
    mock.start();

    const gcp_gcs_client client{config_for(mock)};
    const auto result = client.put_object_if(k_bucket, "k", "hello", precondition{if_absent{}});
    BOOST_TEST(result.version == "5");

    const auto requests = mock.requests();
    BOOST_REQUIRE_EQUAL(requests.size(), 1U);
    BOOST_TEST(requests[0].param("ifGenerationMatch") == "0");
}

BOOST_AUTO_TEST_CASE(if_version_sends_that_generation) {
    MockGcs mock;
    mock.responders = {json_responder(200, object_json("k", "1786971917901538"))};
    mock.start();

    const gcp_gcs_client client{config_for(mock)};
    const auto result =
        client.put_object_if(k_bucket, "k", "hello", precondition{if_version{"1786971917733520"}});
    BOOST_TEST(result.version == "1786971917901538");

    const auto requests = mock.requests();
    BOOST_REQUIRE_EQUAL(requests.size(), 1U);
    BOOST_TEST(requests[0].param("ifGenerationMatch") == "1786971917733520");
}

BOOST_AUTO_TEST_CASE(unconditional_put_sends_no_generation_precondition) {
    // The negative control for the two cases above: an unset `IfGenerationMatch`
    // must not be serialised at all. If it were sent as 0, every ordinary write
    // would silently become create-only and would start failing the moment a
    // key was written twice.
    MockGcs mock;
    mock.responders = {json_responder(200, object_json("k", "5"))};
    mock.start();

    const gcp_gcs_client client{config_for(mock)};
    client.put_object(k_bucket, "k", "hello");

    const auto requests = mock.requests();
    BOOST_REQUIRE_EQUAL(requests.size(), 1U);
    BOOST_TEST(!requests[0].has_param("ifGenerationMatch"));
}

BOOST_AUTO_TEST_CASE(rejected_precondition_throws_object_precondition_failed) {
    MockGcs mock;
    mock.responders = {json_responder(412, condition_not_met_json())};
    mock.start();

    const gcp_gcs_client client{config_for(mock)};
    try {
        client.put_object_if(k_bucket, "logs/000007", "hello",
                             precondition{if_version{"1786971917733520"}});
        BOOST_FAIL("expected put_object_if to throw object_precondition_failed");
    } catch (const object_precondition_failed& err) {
        BOOST_TEST(err.provider() == "gcs");
        BOOST_TEST(err.key() == "logs/000007");
        BOOST_TEST(err.expected_version() == "1786971917733520");
    }
}

BOOST_AUTO_TEST_CASE(rejected_create_only_reports_an_absent_expectation) {
    MockGcs mock;
    mock.responders = {json_responder(412, condition_not_met_json())};
    mock.start();

    const gcp_gcs_client client{config_for(mock)};
    try {
        client.put_object_if(k_bucket, "k", "hello", precondition{if_absent{}});
        BOOST_FAIL("expected put_object_if to throw object_precondition_failed");
    } catch (const object_precondition_failed& err) {
        // Empty is how this type spells "expected: absent"; it must not be the
        // literal 0 that went on the wire.
        BOOST_TEST(err.expected_version() == "");
        BOOST_TEST(std::string(err.what()).find("expected: absent") != std::string::npos);
    }
}

BOOST_AUTO_TEST_CASE(a_412_with_no_payload_still_latches) {
    // The fallback path: no body means no `reason` to read, so the status code
    // has to carry the verdict on its own.
    MockGcs mock;
    mock.responders = {bare_status_responder(412)};
    mock.start();

    const gcp_gcs_client client{config_for(mock)};
    BOOST_CHECK_THROW(client.put_object_if(k_bucket, "k", "hello", precondition{if_absent{}}),
                      object_precondition_failed);
}

BOOST_AUTO_TEST_CASE(a_non_precondition_failure_on_a_conditional_put_does_not_latch) {
    // The single most consequential negative in this file. A fence latch is
    // permanent and no operator action clears it, so anything that is not the
    // service saying "your precondition did not hold" must stay an ordinary
    // exception the engine's own retry can survive.
    MockGcs mock;
    mock.responders = {json_responder(503, gcs_error_json(503, "backendError", "Backend Error"))};
    mock.start();

    const gcp_gcs_client client{config_for(mock)};
    BOOST_CHECK_THROW(client.put_object_if(k_bucket, "k", "hello", precondition{if_absent{}}),
                      std::runtime_error);
    try {
        client.put_object_if(k_bucket, "k", "hello", precondition{if_absent{}});
        BOOST_FAIL("expected put_object_if to throw");
    } catch (const object_precondition_failed&) {
        BOOST_FAIL("a 503 must not latch the fence");
    } catch (const std::runtime_error&) {
    }
}

BOOST_AUTO_TEST_CASE(conditional_put_issues_exactly_one_request) {
    MockGcs mock;
    mock.responders = {bare_status_responder(503)};
    mock.start();

    const gcp_gcs_client client{config_for(mock)};
    BOOST_CHECK_THROW(client.put_object_if(k_bucket, "k", "hello", precondition{if_absent{}}),
                      std::runtime_error);
    BOOST_TEST(mock.request_count() == 1U);
}

BOOST_AUTO_TEST_CASE(an_unparseable_version_is_refused_before_anything_is_sent) {
    // Coercing a bad version to 0 would turn an overwrite CAS into a
    // create-only, which fails safe only by accident. Nothing may reach the
    // service, so the request count is the real assertion here.
    MockGcs mock;
    mock.responders = {json_responder(200, object_json("k", "5"))};
    mock.start();

    const gcp_gcs_client client{config_for(mock)};
    BOOST_CHECK_THROW(
        client.put_object_if(k_bucket, "k", "hello", precondition{if_version{"\"an-etag\""}}),
        std::invalid_argument);
    BOOST_CHECK_THROW(client.put_object_if(k_bucket, "k", "hello", precondition{if_version{""}}),
                      std::invalid_argument);
    BOOST_CHECK_THROW(
        client.put_object_if(k_bucket, "k", "hello", precondition{if_version{"123abc"}}),
        std::invalid_argument);
    BOOST_TEST(mock.request_count() == 0U);
}

BOOST_AUTO_TEST_SUITE_END()

// ── get_object ───────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(gcp_gcs_get)

BOOST_AUTO_TEST_CASE(get_returns_the_body_and_the_generation) {
    MockGcs mock;
    mock.responders = {media_responder("stored bytes", "1786971917733520")};
    mock.start();

    const gcp_gcs_client client{config_for(mock)};
    const auto got = client.get_object(k_bucket, "logs/000001");
    BOOST_REQUIRE(got.has_value());
    BOOST_TEST(got->body == "stored bytes");
    BOOST_TEST(got->version == "1786971917733520");
}

BOOST_AUTO_TEST_CASE(get_of_a_zero_byte_object_falls_back_to_the_metadata_for_its_version) {
    // Measured against live GCS: reading a zero-byte object yields an ok
    // status, an empty body and **no response headers at all**, so there is no
    // `x-goog-generation` to read — while a five-byte read in the same run
    // returned 22 headers including it. The mock reproduces that exactly (the
    // media responder here sends no generation header), and the client is
    // expected to spend a second round trip on `GetObjectMetadata`.
    MockGcs mock;
    mock.responders = {
        [](const recorded_request&, httplib::Response& res) { res.status = 200; },
        json_responder(200, object_json("k", "42", "0")),
    };
    mock.start();

    const gcp_gcs_client client{config_for(mock)};
    const auto got = client.get_object(k_bucket, "k");
    BOOST_REQUIRE(got.has_value());
    BOOST_TEST(got->body == "");
    BOOST_TEST(got->version == "42");
    BOOST_TEST(mock.request_count() == 2U);
}

BOOST_AUTO_TEST_CASE(a_zero_byte_read_whose_metadata_disagrees_about_size_throws) {
    // The window between the download and the metadata fetch. If the object
    // grew in between, that metadata describes bytes this call never read, and
    // returning its generation would name a version for content the caller does
    // not have — which is precisely how a fenced write later succeeds against
    // an object it never saw.
    MockGcs mock;
    mock.responders = {
        [](const recorded_request&, httplib::Response& res) { res.status = 200; },
        json_responder(200, object_json("k", "43", "17")),
    };
    mock.start();

    const gcp_gcs_client client{config_for(mock)};
    try {
        client.get_object(k_bucket, "k");
        BOOST_FAIL("expected a size disagreement to throw");
    } catch (const std::runtime_error& err) {
        BOOST_TEST(std::string(err.what()).find("changed between the download") !=
                   std::string::npos);
    }
}

BOOST_AUTO_TEST_CASE(a_zero_byte_read_of_an_object_deleted_underneath_is_absence) {
    MockGcs mock;
    mock.responders = {
        [](const recorded_request&, httplib::Response& res) { res.status = 200; },
        json_responder(404, absent_object_json(k_bucket, "k")),
    };
    mock.start();

    const gcp_gcs_client client{config_for(mock)};
    BOOST_TEST(!client.get_object(k_bucket, "k").has_value());
}

BOOST_AUTO_TEST_CASE(a_zero_byte_read_whose_metadata_names_a_missing_bucket_throws) {
    MockGcs mock;
    mock.responders = {
        [](const recorded_request&, httplib::Response& res) { res.status = 200; },
        json_responder(404, absent_bucket_json()),
    };
    mock.start();

    const gcp_gcs_client client{config_for(mock)};
    BOOST_CHECK_THROW(client.get_object(k_bucket, "k"), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(an_absent_object_is_nullopt) {
    MockGcs mock;
    mock.responders = {json_responder(404, absent_object_json(k_bucket, "logs/000001"))};
    mock.start();

    const gcp_gcs_client client{config_for(mock)};
    BOOST_TEST(!client.get_object(k_bucket, "logs/000001").has_value());
}

BOOST_AUTO_TEST_CASE(absence_is_recognised_through_the_retry_loops_message_wrapper) {
    // Pins a fact that cost this suite six failures on its first run.
    // google-cloud-cpp's retry loop **rewrites** the message on the way out, so
    // what GCS sent as `No such object: b/k` reaches the client as
    // `Permanent error, with a last message of No such object: b/k`. A prefix
    // match against the service's own wording therefore recognises nothing —
    // which is what the client did first, turning every absent object into a
    // thrown exception. The assertion below is on the wrapper being seen
    // through, so restoring a prefix match fails here rather than somewhere
    // subtler.
    MockGcs mock;
    mock.responders = {json_responder(404, absent_object_json(k_bucket, "logs/000001"))};
    mock.start();

    const gcp_gcs_client client{config_for(mock)};
    BOOST_TEST(!client.get_object(k_bucket, "logs/000001").has_value());

    // …and the wrapper really is there, so this case is not passing for want of
    // one. Same response, read through an operation that reports the message.
    MockGcs echo;
    echo.responders = {json_responder(404, absent_bucket_json())};
    echo.start();

    const gcp_gcs_client echo_client{config_for(echo)};
    try {
        echo_client.get_object(k_bucket, "k");
        BOOST_FAIL("expected the missing-bucket 404 to throw");
    } catch (const std::runtime_error& err) {
        BOOST_TEST(std::string(err.what()).find("with a last message of") != std::string::npos);
    }
}

BOOST_AUTO_TEST_CASE(an_absent_bucket_throws_and_is_never_read_as_an_absent_object) {
    // The case this client's whole 404 handling exists for. GCS gives this the
    // same status and the same `reason` as the case above; reading it as
    // absence would show the engine an empty Raft log, which it cannot detect.
    MockGcs mock;
    mock.responders = {json_responder(404, absent_bucket_json())};
    mock.start();

    const gcp_gcs_client client{config_for(mock)};
    try {
        const auto got = client.get_object(k_bucket, "logs/000001");
        BOOST_FAIL("a missing bucket must not be reported as an absent object");
    } catch (const std::runtime_error& err) {
        BOOST_TEST(std::string(err.what()).find("The specified bucket does not exist.") !=
                   std::string::npos);
    }
}

BOOST_AUTO_TEST_CASE(a_404_with_no_payload_throws_rather_than_reporting_absence) {
    // Absence is a whitelist: an unrecognised 404 is not evidence the object is
    // absent, and guessing in the permissive direction is the one guess this
    // client must never make.
    MockGcs mock;
    mock.responders = {bare_status_responder(404)};
    mock.start();

    const gcp_gcs_client client{config_for(mock)};
    BOOST_CHECK_THROW(client.get_object(k_bucket, "k"), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(a_server_error_on_get_throws) {
    MockGcs mock;
    mock.responders = {json_responder(500, gcs_error_json(500, "backendError", "Backend Error"))};
    mock.start();

    const gcp_gcs_client client{config_for(mock)};
    BOOST_CHECK_THROW(client.get_object(k_bucket, "k"), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(a_read_with_no_generation_header_throws) {
    // The version read here is what a fenced engine later predicates a
    // conditional write on, so a versionless read must not be handed back as a
    // successful one — it would quietly turn the next CAS into a create-only.
    MockGcs mock;
    mock.responders = {[](const recorded_request&, httplib::Response& res) {
        res.status = 200;
        res.set_content("stored bytes", "application/octet-stream");
    }};
    mock.start();

    const gcp_gcs_client client{config_for(mock)};
    try {
        client.get_object(k_bucket, "k");
        BOOST_FAIL("expected a read with no generation to throw");
    } catch (const std::runtime_error& err) {
        // Matched on the *whole* phrase, not on "no generation" alone. The
        // zero-byte fallback's own error message ends with "...reports no
        // generation" too, so the looser match let a mutant that routed this
        // non-empty read into that fallback pass for the wrong reason — it
        // still threw, just from one call later.
        BOOST_TEST(std::string(err.what()).find("bytes but no generation") != std::string::npos);
    }
    // The load-bearing half: a read that returned bytes must never reach the
    // metadata fallback, which exists only for the zero-byte case. One request,
    // not two.
    BOOST_TEST(mock.request_count() == 1U);
}

BOOST_AUTO_TEST_SUITE_END()

// ── delete_object ────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(gcp_gcs_delete)

BOOST_AUTO_TEST_CASE(delete_issues_a_delete_and_succeeds) {
    MockGcs mock;
    mock.responders = {bare_status_responder(204)};
    mock.start();

    const gcp_gcs_client client{config_for(mock)};
    BOOST_CHECK_NO_THROW(client.delete_object(k_bucket, "logs/000001"));

    const auto requests = mock.requests();
    BOOST_REQUIRE_EQUAL(requests.size(), 1U);
    BOOST_TEST(requests[0].method == "DELETE");
    BOOST_TEST(!requests[0].has_param("ifGenerationMatch"));
}

BOOST_AUTO_TEST_CASE(deleting_an_absent_object_succeeds) {
    // GCS answers 404 where S3 and OSS answer 204, so the idempotence the
    // engine's truncation path needs is supplied by this client rather than by
    // the service.
    MockGcs mock;
    mock.responders = {json_responder(404, absent_object_json(k_bucket, "logs/000001"))};
    mock.start();

    const gcp_gcs_client client{config_for(mock)};
    BOOST_CHECK_NO_THROW(client.delete_object(k_bucket, "logs/000001"));
}

BOOST_AUTO_TEST_CASE(deleting_from_an_absent_bucket_throws) {
    // The same whitelist as the read path, and it matters just as much here:
    // swallowing this 404 would let a misconfigured bucket report every
    // truncation as successful.
    MockGcs mock;
    mock.responders = {json_responder(404, absent_bucket_json())};
    mock.start();

    const gcp_gcs_client client{config_for(mock)};
    BOOST_CHECK_THROW(client.delete_object(k_bucket, "k"), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(a_server_error_on_delete_throws) {
    MockGcs mock;
    mock.responders = {json_responder(500, gcs_error_json(500, "backendError", "Backend Error"))};
    mock.start();

    const gcp_gcs_client client{config_for(mock)};
    BOOST_CHECK_THROW(client.delete_object(k_bucket, "k"), std::runtime_error);
}

BOOST_AUTO_TEST_SUITE_END()

// ── delete_object_if ─────────────────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(gcp_gcs_conditional_delete)

BOOST_AUTO_TEST_CASE(if_version_sends_that_generation) {
    MockGcs mock;
    mock.responders = {bare_status_responder(204)};
    mock.start();

    const gcp_gcs_client client{config_for(mock)};
    BOOST_CHECK_NO_THROW(
        client.delete_object_if(k_bucket, "k", precondition{if_version{"1786971917733520"}}));

    const auto requests = mock.requests();
    BOOST_REQUIRE_EQUAL(requests.size(), 1U);
    BOOST_TEST(requests[0].method == "DELETE");
    BOOST_TEST(requests[0].param("ifGenerationMatch") == "1786971917733520");
}

BOOST_AUTO_TEST_CASE(if_absent_is_expressible_and_sends_zero) {
    // Unlike S3, whose DeleteObject models `If-Match` only, GCS accepts
    // `ifGenerationMatch=0` on a delete — measured against live GCS.
    MockGcs mock;
    mock.responders = {json_responder(404, absent_object_json(k_bucket, "k"))};
    mock.start();

    const gcp_gcs_client client{config_for(mock)};
    BOOST_CHECK_NO_THROW(client.delete_object_if(k_bucket, "k", precondition{if_absent{}}));

    const auto requests = mock.requests();
    BOOST_REQUIRE_EQUAL(requests.size(), 1U);
    BOOST_TEST(requests[0].param("ifGenerationMatch") == "0");
}

BOOST_AUTO_TEST_CASE(a_stale_generation_latches) {
    MockGcs mock;
    mock.responders = {json_responder(412, condition_not_met_json())};
    mock.start();

    const gcp_gcs_client client{config_for(mock)};
    try {
        client.delete_object_if(k_bucket, "logs/000007",
                                precondition{if_version{"1786971917733520"}});
        BOOST_FAIL("expected delete_object_if to throw object_precondition_failed");
    } catch (const object_precondition_failed& err) {
        BOOST_TEST(err.key() == "logs/000007");
        BOOST_TEST(err.expected_version() == "1786971917733520");
    }
}

BOOST_AUTO_TEST_CASE(if_absent_on_an_existing_object_latches) {
    MockGcs mock;
    mock.responders = {json_responder(412, condition_not_met_json())};
    mock.start();

    const gcp_gcs_client client{config_for(mock)};
    BOOST_CHECK_THROW(client.delete_object_if(k_bucket, "k", precondition{if_absent{}}),
                      object_precondition_failed);
}

BOOST_AUTO_TEST_CASE(the_two_preconditions_read_a_404_differently) {
    // The asymmetry, asserted as one case because it is one decision.
    //
    //  - `if_absent` + 404 → the precondition held; there was nothing to delete
    //    and the caller asked for exactly that.
    //  - `if_version` + 404 → the object the caller expected is *gone*, so the
    //    precondition did not hold and it lost the race. Reporting that as
    //    success would swallow precisely what the fence exists to detect.
    MockGcs absent_mock;
    absent_mock.responders = {json_responder(404, absent_object_json(k_bucket, "k"))};
    absent_mock.start();

    const gcp_gcs_client absent_client{config_for(absent_mock)};
    BOOST_CHECK_NO_THROW(absent_client.delete_object_if(k_bucket, "k", precondition{if_absent{}}));

    MockGcs version_mock;
    version_mock.responders = {json_responder(404, absent_object_json(k_bucket, "k"))};
    version_mock.start();

    const gcp_gcs_client version_client{config_for(version_mock)};
    BOOST_CHECK_THROW(
        version_client.delete_object_if(k_bucket, "k", precondition{if_version{"999"}}),
        object_precondition_failed);
}

BOOST_AUTO_TEST_CASE(an_absent_bucket_on_a_conditional_delete_still_throws_plainly) {
    // Neither precondition may turn a missing bucket into an answer about the
    // object, and the `if_absent` path is the one that could: it treats a 404
    // as success, so the whitelist is what stops it here.
    MockGcs mock;
    mock.responders = {json_responder(404, absent_bucket_json())};
    mock.start();

    const gcp_gcs_client client{config_for(mock)};
    try {
        client.delete_object_if(k_bucket, "k", precondition{if_absent{}});
        BOOST_FAIL("a missing bucket must not satisfy a create-only delete precondition");
    } catch (const object_precondition_failed&) {
        BOOST_FAIL("a missing bucket is not a precondition failure either");
    } catch (const std::runtime_error& err) {
        BOOST_TEST(std::string(err.what()).find("The specified bucket does not exist.") !=
                   std::string::npos);
    }
}

BOOST_AUTO_TEST_CASE(conditional_delete_issues_exactly_one_request) {
    MockGcs mock;
    mock.responders = {bare_status_responder(503)};
    mock.start();

    const gcp_gcs_client client{config_for(mock)};
    BOOST_CHECK_THROW(client.delete_object_if(k_bucket, "k", precondition{if_absent{}}),
                      std::runtime_error);
    BOOST_TEST(mock.request_count() == 1U);
}

BOOST_AUTO_TEST_SUITE_END()

// ── list_keys ────────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(gcp_gcs_list)

BOOST_AUTO_TEST_CASE(list_sends_the_prefix_and_returns_every_name) {
    MockGcs mock;
    mock.responders = {json_responder(
        200, listing_json({"node-1/logs/000001", "node-1/logs/000002", "node-1/term"}))};
    mock.start();

    const gcp_gcs_client client{config_for(mock)};
    const auto keys = client.list_keys(k_bucket, "node-1/");

    BOOST_REQUIRE_EQUAL(keys.size(), 3U);
    BOOST_TEST(keys[0] == "node-1/logs/000001");
    BOOST_TEST(keys[1] == "node-1/logs/000002");
    BOOST_TEST(keys[2] == "node-1/term");

    const auto requests = mock.requests();
    BOOST_REQUIRE_EQUAL(requests.size(), 1U);
    BOOST_TEST(requests[0].method == "GET");
    BOOST_TEST(requests[0].param("prefix") == "node-1/");
}

BOOST_AUTO_TEST_CASE(list_pages_to_exhaustion) {
    // A short listing is silent data loss here: recovery reads the log from one
    // prefixed LIST and cannot tell "2 entries" from "4 entries, 2 shown".
    MockGcs mock;
    mock.responders = {
        json_responder(200, listing_json({"node-1/logs/000001", "node-1/logs/000002"}, "page-2")),
        json_responder(200, listing_json({"node-1/logs/000003", "node-1/term"})),
    };
    mock.start();

    const gcp_gcs_client client{config_for(mock)};
    const auto keys = client.list_keys(k_bucket, "node-1/");

    BOOST_REQUIRE_EQUAL(keys.size(), 4U);
    BOOST_TEST(keys[3] == "node-1/term");

    const auto requests = mock.requests();
    BOOST_REQUIRE_EQUAL(requests.size(), 2U);
    // The second request must carry the token, or the first page would simply
    // be fetched twice and the listing would loop or double-count.
    BOOST_TEST(requests[1].param("pageToken") == "page-2");
}

BOOST_AUTO_TEST_CASE(an_empty_listing_is_an_empty_vector_not_an_error) {
    MockGcs mock;
    mock.responders = {json_responder(200, R"({"kind":"storage#objects"})")};
    mock.start();

    const gcp_gcs_client client{config_for(mock)};
    BOOST_TEST(client.list_keys(k_bucket, "node-1/").empty());
}

BOOST_AUTO_TEST_CASE(a_failed_first_page_throws) {
    MockGcs mock;
    mock.responders = {json_responder(500, gcs_error_json(500, "backendError", "Backend Error"))};
    mock.start();

    const gcp_gcs_client client{config_for(mock)};
    try {
        client.list_keys(k_bucket, "node-1/");
        BOOST_FAIL("expected list_keys to throw");
    } catch (const std::runtime_error& err) {
        BOOST_TEST(std::string(err.what()).find("ListObjects") != std::string::npos);
    }
}

BOOST_AUTO_TEST_CASE(a_failed_later_page_throws_rather_than_returning_a_short_listing) {
    // The one that matters. The first page succeeded, so a client that stopped
    // at the failure would return a plausible, complete-looking, wrong answer.
    MockGcs mock;
    mock.responders = {
        json_responder(200, listing_json({"node-1/logs/000001", "node-1/logs/000002"}, "page-2")),
        json_responder(500, gcs_error_json(500, "backendError", "Backend Error")),
    };
    mock.start();

    const gcp_gcs_client client{config_for(mock)};
    BOOST_CHECK_THROW(client.list_keys(k_bucket, "node-1/"), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(an_absent_bucket_on_a_listing_throws) {
    MockGcs mock;
    mock.responders = {json_responder(404, absent_bucket_json())};
    mock.start();

    const gcp_gcs_client client{config_for(mock)};
    BOOST_CHECK_THROW(client.list_keys(k_bucket, "node-1/"), std::runtime_error);
}

BOOST_AUTO_TEST_SUITE_END()

// ── Configuration ────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(gcp_gcs_configuration)

BOOST_AUTO_TEST_CASE(an_empty_project_id_is_accepted) {
    // Object operations name a bucket and never a project, so refusing to
    // construct without one would reject a configuration that works. Every
    // other GCP component in this tree does require it, which is exactly why
    // the difference is pinned rather than left to be noticed.
    MockGcs mock;
    mock.responders = {json_responder(200, object_json("k", "5"))};
    mock.start();

    auto cfg = config_for(mock);
    cfg.project_id.clear();

    const gcp_gcs_client client{cfg};
    BOOST_CHECK_NO_THROW(client.put_object(k_bucket, "k", "hello"));
    BOOST_TEST(mock.request_count() == 1U);
}

BOOST_AUTO_TEST_CASE(the_endpoint_override_is_what_the_client_talks_to) {
    // Stated as its own case because everything else in this file depends on
    // it: if `endpoint_override` were ignored, every case here would be talking
    // to real GCS and most would fail for interesting-looking reasons. Two
    // mocks, one configured and one not, and only the configured one is hit.
    MockGcs configured;
    configured.responders = {json_responder(200, object_json("k", "5"))};
    configured.start();

    MockGcs unconfigured;
    unconfigured.responders = {json_responder(200, object_json("k", "9"))};
    unconfigured.start();

    const gcp_gcs_client client{config_for(configured)};
    client.put_object(k_bucket, "k", "hello");

    BOOST_TEST(configured.request_count() == 1U);
    BOOST_TEST(unconfigured.request_count() == 0U);
}

BOOST_AUTO_TEST_SUITE_END()

#else  // KYTHIRA_HAS_GCP_STORAGE

BOOST_AUTO_TEST_CASE(gcp_storage_not_available) {
    BOOST_TEST_MESSAGE("google-cloud-cpp storage component not available — gcp_gcs_client skipped");
    BOOST_TEST(true);
}

#endif  // KYTHIRA_HAS_GCP_STORAGE
