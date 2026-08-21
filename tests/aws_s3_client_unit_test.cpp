// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#define BOOST_TEST_MODULE aws_s3_client_unit_test
#include <boost/test/unit_test.hpp>

#include <raft/aws_s3_client.hpp>

// Unit coverage for `aws_s3_client` (cloud-object-persistence Requirement 12,
// task 7). Every assertion here is made against the bytes that **actually
// arrived** at a local server rather than against the client's own constants:
// the whole value of this client is what it puts on the wire, and a test that
// reads the client's fields back would pass just as happily if nothing were
// sent at all.
//
// The three load-bearing cases, in order of what they would cost if wrong:
//
//  1. **412 latches, 409 does not.** S3 answers 412 `PreconditionFailed` when a
//     precondition is rejected and 409 `ConditionalRequestConflict` when two
//     conditional requests race and the caller should retry. Mapping the second
//     onto `object_precondition_failed` would turn a transient race into a
//     permanent, unclearable fence latch on a healthy engine. The 409 path was
//     *not* elicited by task 0's live probe (53 racing losers, all 412), so this
//     mapping rests on documentation and is pinned here rather than deleted as
//     unreachable.
//  2. **Exactly one request per call.** The SDK's retry strategy is forced to
//     zero; the engine's single retry must be the only retry or its idempotency
//     argument is unverifiable. Counted against a failing endpoint.
//  3. **A truncated listing is never silently short.** Recovery reads the log
//     from one prefixed LIST and cannot tell "40 entries" from "60 entries, 40
//     shown".

#ifdef KYTHIRA_HAS_AWS_SDK

#include <aws/core/Aws.h>
#include <aws/core/auth/AWSCredentialsProvider.h>
#include <aws/core/auth/AWSCredentialsProviderChain.h>

#include <httplib.h>

#include <chrono>
#include <functional>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace kythira;
using namespace std::chrono_literals;

namespace {

struct AwsSdkFixture {
    AwsSdkFixture() {
        Aws::SDKOptions opts;
        Aws::InitAPI(opts);
    }
    ~AwsSdkFixture() {
        Aws::SDKOptions opts;
        Aws::ShutdownAPI(opts);
    }
    AwsSdkFixture(const AwsSdkFixture&) = delete;
    auto operator=(const AwsSdkFixture&) -> AwsSdkFixture& = delete;
    AwsSdkFixture(AwsSdkFixture&&) = delete;
    auto operator=(AwsSdkFixture&&) -> AwsSdkFixture& = delete;
};

// The SDK must outlive every suite.
BOOST_GLOBAL_FIXTURE(AwsSdkFixture);

/// Static credentials, so nothing in this binary ever reaches for the instance
/// metadata service or `~/.aws`. `AWSCredentialsProviderChain::AddProvider` is
/// protected, which is why this is a subclass rather than an instance.
struct static_credentials_chain : Aws::Auth::AWSCredentialsProviderChain {
    static_credentials_chain() {
        AddProvider(Aws::MakeShared<Aws::Auth::SimpleAWSCredentialsProvider>(
            "aws_s3_client_unit_test", "AKIDEXAMPLE", "wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY"));
    }
};

/// What the server saw. Captured rather than asserted in the handler: a failed
/// `BOOST_TEST` on the server thread reports against the wrong test, and the
/// interesting assertions want to run after the call has returned anyway.
struct recorded_request {
    std::string method;
    std::string path;
    std::string body;
    httplib::Headers headers;
    httplib::Params params;

    [[nodiscard]] auto header(const std::string& name) const -> std::string {
        const auto it = headers.find(name);
        return it == headers.end() ? std::string{} : it->second;
    }

    [[nodiscard]] auto has_header(const std::string& name) const -> bool {
        return headers.find(name) != headers.end();
    }

    [[nodiscard]] auto param(const std::string& name) const -> std::string {
        const auto it = params.find(name);
        return it == params.end() ? std::string{} : it->second;
    }
};

/// A local stand-in for S3: records every request and answers with whatever the
/// test programmed. It performs no signature verification — the SDK's SigV4 is
/// not what this suite is testing, and the repo's signature-verifying mocks
/// exist for the two hand-rolled clients.
struct MockS3 {
    httplib::Server server;
    std::thread thread;
    int port{0};

    mutable std::mutex mu;
    std::vector<recorded_request> log;

    /// Answers the Nth request (0-based); the last entry answers every
    /// subsequent one, so a single-element vector is "always this".
    std::vector<std::function<void(const recorded_request&, httplib::Response&)>> responders;

    MockS3() {
        auto handler = [this](const httplib::Request& req, httplib::Response& res) {
            recorded_request rec{req.method, req.path, req.body, req.headers, req.params};
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
        server.Put("/.*", handler);
        server.Get("/.*", handler);
        server.Delete("/.*", handler);
    }

    void start() {
        port = server.bind_to_any_port("127.0.0.1");
        thread = std::thread([this] { server.listen_after_bind(); });
        for (int i = 0; i < 200 && !server.is_running(); ++i) {
            std::this_thread::sleep_for(5ms);
        }
    }

    ~MockS3() {
        server.stop();
        if (thread.joinable()) {
            thread.join();
        }
    }

    MockS3(const MockS3&) = delete;
    auto operator=(const MockS3&) -> MockS3& = delete;
    MockS3(MockS3&&) = delete;
    auto operator=(MockS3&&) -> MockS3& = delete;

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

auto config_for(const MockS3& mock) -> aws_client_config {
    aws_client_config cfg;
    cfg.region = "us-east-1";
    cfg.endpoint_override = mock.origin();
    cfg.api_timeout = 5s;
    cfg.credentials_provider = std::make_shared<static_credentials_chain>();
    return cfg;
}

/// S3's error bodies, in the shape the SDK's marshaller reads: it is the
/// `<Code>` element, not the status, that decides every verdict in this client.
auto s3_error_xml(const std::string& code, const std::string& message) -> std::string {
    return "<?xml version=\"1.0\" encoding=\"UTF-8\"?><Error><Code>" + code + "</Code><Message>" +
           message + "</Message><RequestId>test-request-id</RequestId></Error>";
}

auto listing_xml(const std::string& contents_xml, bool truncated,
                 const std::string& next_token = {}) -> std::string {
    std::string body =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<ListBucketResult xmlns=\"http://s3.amazonaws.com/doc/2006-03-01/\">"
        "<Name>b</Name><MaxKeys>1000</MaxKeys>";
    body += contents_xml;
    body += std::string("<IsTruncated>") + (truncated ? "true" : "false") + "</IsTruncated>";
    if (!next_token.empty()) {
        body += "<NextContinuationToken>" + next_token + "</NextContinuationToken>";
    }
    body += "</ListBucketResult>";
    return body;
}

auto contents_entry(const std::string& key) -> std::string {
    return "<Contents><Key>" + key +
           "</Key><LastModified>2026-08-17T00:00:00.000Z</LastModified>"
           "<ETag>&quot;d41d8cd98f00b204e9800998ecf8427e&quot;</ETag><Size>0</Size>"
           "<StorageClass>STANDARD</StorageClass></Contents>";
}

}  // namespace

// ── The concept obligations, over the real type ──────────────────────────────
//
// The header asserts these beside the definition too. Repeated here because a
// concept that is only satisfied in the header's own translation unit is not
// the property callers depend on.

BOOST_AUTO_TEST_SUITE(aws_s3_concepts)

BOOST_AUTO_TEST_CASE(satisfies_the_store_concepts) {
    static_assert(key_object_store<aws_s3_client>);
    static_assert(conditional_key_object_store<aws_s3_client>);
    static_assert(content_md5_versioned_store<aws_s3_client>);
    BOOST_TEST(aws_s3_client::provider_name() == "s3");
}

BOOST_AUTO_TEST_SUITE_END()

// ── Request shape ────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(aws_s3_request_shape)

// Path-style under `endpoint_override`, so a local server needs no wildcard
// DNS for `<bucket>.<host>`.
BOOST_AUTO_TEST_CASE(endpoint_override_uses_path_style_addressing) {
    MockS3 mock;
    mock.responders.push_back([](const recorded_request&, httplib::Response& res) {
        res.set_header("ETag", "\"5d41402abc4b2a76b9719d911017c592\"");
        res.status = 200;
    });
    mock.start();

    const aws_s3_client client{config_for(mock)};
    client.put_object("mybucket", "raft/term", "hello");

    const auto reqs = mock.requests();
    BOOST_REQUIRE(reqs.size() == 1U);
    BOOST_TEST(reqs[0].method == "PUT");
    BOOST_TEST(reqs[0].path == "/mybucket/raft/term");
    BOOST_TEST(!reqs[0].header("Authorization").empty());
}

// The case above passes with `useVirtualAddressing = false` **deleted**, so on
// its own it is not evidence for that line: the SDK's endpoint rules already
// fall back to path style when the endpoint host is a literal IP. What the
// setting actually buys is the *hostname* endpoint — LocalStack at
// `http://localstack:4566` (task 15.1), or any named mock — where virtual
// addressing asks for `<bucket>.<host>` and needs wildcard DNS that does not
// exist. `localhost` is the hostname available everywhere, so it is the one
// that can pin this.
BOOST_AUTO_TEST_CASE(a_hostname_endpoint_is_also_addressed_path_style) {
    MockS3 mock;
    mock.responders.push_back([](const recorded_request&, httplib::Response& res) {
        res.set_header("ETag", "\"e\"");
        res.status = 200;
    });
    mock.start();

    aws_client_config cfg = config_for(mock);
    cfg.endpoint_override = "http://localhost:" + std::to_string(mock.port);
    const aws_s3_client client{cfg};
    client.put_object("mybucket", "raft/term", "hello");

    const auto reqs = mock.requests();
    BOOST_REQUIRE(reqs.size() == 1U);
    // Virtual addressing would have made this "/raft/term" against a host of
    // "mybucket.localhost" — which either fails to resolve or reaches the wrong
    // place, and in neither case is it this.
    BOOST_TEST(reqs[0].path == "/mybucket/raft/term");
}

// Requirement 7.1's service-side check, in the encoding task 0.5 verified:
// `Content-MD5` is **base64 of the raw 16 digest bytes**, where the ETag is
// hex. The two encodings of one digest are the obvious thing to confuse, so the
// known answer is written out rather than recomputed by the same code under
// test.
BOOST_AUTO_TEST_CASE(every_put_carries_content_md5_as_base64_not_hex) {
    MockS3 mock;
    mock.responders.push_back([](const recorded_request&, httplib::Response& res) {
        res.set_header("ETag", "\"5d41402abc4b2a76b9719d911017c592\"");
        res.status = 200;
    });
    mock.start();

    const aws_s3_client client{config_for(mock)};
    client.put_object("b", "k", "hello");

    const auto reqs = mock.requests();
    BOOST_REQUIRE(reqs.size() == 1U);
    // md5("hello") = 5d41402abc4b2a76b9719d911017c592
    BOOST_TEST(reqs[0].header("Content-MD5") == "XUFAKrxLKna5cZ2REBfFkg==");
    BOOST_TEST(reqs[0].header("Content-MD5") != "5d41402abc4b2a76b9719d911017c592");
}

// The SDK's `requestChecksumCalculation` default (`WHEN_SUPPORTED`) adds a
// CRC32 and can reframe the body onto `aws-chunked` trailers to carry it. It is
// switched off in the client so the integrity check on the wire is the one the
// header names — and so the body arrives as the bytes that were written, which
// the round-trip cases below silently depend on.
BOOST_AUTO_TEST_CASE(the_sdks_own_checksum_framing_is_not_used) {
    MockS3 mock;
    mock.responders.push_back([](const recorded_request&, httplib::Response& res) {
        res.set_header("ETag", "\"x\"");
        res.status = 200;
    });
    mock.start();

    const aws_s3_client client{config_for(mock)};
    client.put_object("b", "k", "hello");

    const auto reqs = mock.requests();
    BOOST_REQUIRE(reqs.size() == 1U);
    BOOST_TEST(!reqs[0].has_header("x-amz-sdk-checksum-algorithm"));
    BOOST_TEST(!reqs[0].has_header("x-amz-trailer"));
    BOOST_TEST(reqs[0].body == "hello");
}

BOOST_AUTO_TEST_CASE(create_only_sends_if_none_match_star) {
    MockS3 mock;
    mock.responders.push_back([](const recorded_request&, httplib::Response& res) {
        res.set_header("ETag", "\"e1\"");
        res.status = 200;
    });
    mock.start();

    const aws_s3_client client{config_for(mock)};
    const auto result = client.put_object_if("b", "k", "v", precondition{if_absent{}});

    const auto reqs = mock.requests();
    BOOST_REQUIRE(reqs.size() == 1U);
    BOOST_TEST(reqs[0].header("If-None-Match") == "*");
    BOOST_TEST(!reqs[0].has_header("If-Match"));
    BOOST_TEST(result.version == "\"e1\"");
}

BOOST_AUTO_TEST_CASE(overwrite_precondition_sends_if_match) {
    MockS3 mock;
    mock.responders.push_back([](const recorded_request&, httplib::Response& res) {
        res.set_header("ETag", "\"e2\"");
        res.status = 200;
    });
    mock.start();

    const aws_s3_client client{config_for(mock)};
    client.put_object_if("b", "k", "v", precondition{if_version{"\"e1\""}});

    const auto reqs = mock.requests();
    BOOST_REQUIRE(reqs.size() == 1U);
    BOOST_TEST(reqs[0].header("If-Match") == "\"e1\"");
    BOOST_TEST(!reqs[0].has_header("If-None-Match"));
}

BOOST_AUTO_TEST_CASE(conditional_delete_sends_if_match) {
    MockS3 mock;
    mock.responders.push_back(
        [](const recorded_request&, httplib::Response& res) { res.status = 204; });
    mock.start();

    const aws_s3_client client{config_for(mock)};
    client.delete_object_if("b", "k", precondition{if_version{"\"e1\""}});

    const auto reqs = mock.requests();
    BOOST_REQUIRE(reqs.size() == 1U);
    BOOST_TEST(reqs[0].method == "DELETE");
    BOOST_TEST(reqs[0].header("If-Match") == "\"e1\"");
}

// S3's DeleteObject models `If-Match` and not `If-None-Match`, so a create-only
// precondition has no wire spelling. Refusing loudly is the point: the silent
// alternative is an unconditional DELETE issued by a caller who believes a
// guard is in place.
BOOST_AUTO_TEST_CASE(create_only_delete_is_refused_rather_than_dropped) {
    MockS3 mock;
    mock.start();

    const aws_s3_client client{config_for(mock)};
    BOOST_CHECK_THROW(client.delete_object_if("b", "k", precondition{if_absent{}}),
                      std::invalid_argument);
    // And nothing reached the service — the refusal happens before the request.
    BOOST_TEST(mock.request_count() == 0U);
}

BOOST_AUTO_TEST_SUITE_END()

// ── Round trips and absence ──────────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(aws_s3_operations)

BOOST_AUTO_TEST_CASE(put_get_delete_round_trip) {
    MockS3 mock;
    std::string stored;
    bool present = false;
    mock.responders.push_back([&](const recorded_request& req, httplib::Response& res) {
        if (req.method == "PUT") {
            stored = req.body;
            present = true;
            res.set_header("ETag", "\"etag-1\"");
            res.status = 200;
        } else if (req.method == "GET") {
            if (!present) {
                res.status = 404;
                res.set_content(s3_error_xml("NoSuchKey", "The specified key does not exist."),
                                "application/xml");
                return;
            }
            res.set_header("ETag", "\"etag-1\"");
            res.set_content(stored, "application/octet-stream");
        } else {
            present = false;
            res.status = 204;
        }
    });
    mock.start();

    const aws_s3_client client{config_for(mock)};
    const auto put = client.put_object("b", "raft/term", "42");
    BOOST_TEST(put.version == "\"etag-1\"");

    const auto got = client.get_object("b", "raft/term");
    BOOST_REQUIRE(got.has_value());
    BOOST_TEST(got->body == "42");
    BOOST_TEST(got->version == "\"etag-1\"");

    client.delete_object("b", "raft/term");
    BOOST_TEST(!client.get_object("b", "raft/term").has_value());
}

BOOST_AUTO_TEST_CASE(binary_bytes_survive_a_round_trip) {
    MockS3 mock;
    std::string stored;
    mock.responders.push_back([&](const recorded_request& req, httplib::Response& res) {
        if (req.method == "PUT") {
            stored = req.body;
            res.set_header("ETag", "\"etag\"");
            res.status = 200;
        } else {
            res.set_header("ETag", "\"etag\"");
            res.set_content(stored, "application/octet-stream");
        }
    });
    mock.start();

    // Split literal, deliberately: a hex escape is greedy and 'b' is a hex
    // digit, so "\xffbinary" parses as the single escape \xffb — out of range
    // for char, and clang++-18 (what CI builds with) rejects it outright.
    const std::string payload(
        "\x00\x01\xff"
        "binary\x00",
        11);

    const aws_s3_client client{config_for(mock)};
    client.put_object("b", "blob", payload);
    const auto got = client.get_object("b", "blob");
    BOOST_REQUIRE(got.has_value());
    BOOST_TEST(got->body == payload);
}

// A zero-byte object is a legitimate value, and reading one goes through the
// `<< rdbuf()` path that sets failbit on an empty stream.
BOOST_AUTO_TEST_CASE(an_empty_object_reads_back_as_empty_not_absent) {
    MockS3 mock;
    mock.responders.push_back([](const recorded_request&, httplib::Response& res) {
        res.set_header("ETag", "\"d41d8cd98f00b204e9800998ecf8427e\"");
        res.set_content("", "application/octet-stream");
        res.status = 200;
    });
    mock.start();

    const aws_s3_client client{config_for(mock)};
    const auto got = client.get_object("b", "empty");
    BOOST_REQUIRE(got.has_value());
    BOOST_TEST(got->body.empty());
}

// Absence is an answer, not an error — a persistence load path must tell "no
// such object" from "the store is broken".
BOOST_AUTO_TEST_CASE(no_such_key_is_nullopt) {
    MockS3 mock;
    mock.responders.push_back([](const recorded_request&, httplib::Response& res) {
        res.status = 404;
        res.set_content(s3_error_xml("NoSuchKey", "The specified key does not exist."),
                        "application/xml");
    });
    mock.start();

    const aws_s3_client client{config_for(mock)};
    BOOST_TEST(!client.get_object("b", "absent").has_value());
}

// The other 404. A missing bucket is a misconfiguration that must surface —
// reading it as "the object is absent" would let an engine pointed at a
// non-existent bucket start up as though its prefix were merely empty.
BOOST_AUTO_TEST_CASE(no_such_bucket_is_an_error_not_an_absent_object) {
    MockS3 mock;
    mock.responders.push_back([](const recorded_request&, httplib::Response& res) {
        res.status = 404;
        res.set_content(s3_error_xml("NoSuchBucket", "The specified bucket does not exist."),
                        "application/xml");
    });
    mock.start();

    const aws_s3_client client{config_for(mock)};
    try {
        (void)client.get_object("b", "k");
        BOOST_FAIL("expected a runtime_error for NoSuchBucket");
    } catch (const std::runtime_error& e) {
        BOOST_TEST(std::string(e.what()).find("NoSuchBucket") != std::string::npos);
    }
}

BOOST_AUTO_TEST_CASE(the_services_code_and_message_reach_the_exception) {
    MockS3 mock;
    mock.responders.push_back([](const recorded_request&, httplib::Response& res) {
        res.status = 403;
        res.set_content(s3_error_xml("AccessDenied", "Access Denied"), "application/xml");
    });
    mock.start();

    const aws_s3_client client{config_for(mock)};
    try {
        client.put_object("b", "k", "v");
        BOOST_FAIL("expected a runtime_error");
    } catch (const std::runtime_error& e) {
        const std::string what = e.what();
        BOOST_TEST(what.find("AccessDenied") != std::string::npos);
        BOOST_TEST(what.find("403") != std::string::npos);
        BOOST_TEST(what.find("PutObject") != std::string::npos);
        BOOST_TEST(what.find("k") != std::string::npos);
    }
}

// A checksum the service rejects: 400 BadDigest, confirmed live (Finding 10).
// It is an ordinary error, not a precondition failure — re-sending identical
// bytes is what repairs a corrupted transfer, and latching a fence on it would
// be a permanent response to a transient fault.
BOOST_AUTO_TEST_CASE(a_rejected_digest_is_an_ordinary_error) {
    MockS3 mock;
    mock.responders.push_back([](const recorded_request&, httplib::Response& res) {
        res.status = 400;
        res.set_content(s3_error_xml("BadDigest", "The Content-MD5 you specified did not match"),
                        "application/xml");
    });
    mock.start();

    const aws_s3_client client{config_for(mock)};
    BOOST_CHECK_THROW(client.put_object_if("b", "k", "v", precondition{if_absent{}}),
                      std::runtime_error);
    try {
        client.put_object_if("b", "k", "v", precondition{if_absent{}});
    } catch (const object_precondition_failed&) {
        BOOST_FAIL("BadDigest must not be reported as a precondition failure");
    } catch (const std::runtime_error& e) {
        BOOST_TEST(std::string(e.what()).find("BadDigest") != std::string::npos);
    }
}

BOOST_AUTO_TEST_SUITE_END()

// ── The 412/409 split: the mapping this whole client turns on ────────────────

BOOST_AUTO_TEST_SUITE(aws_s3_conditional_verdicts)

BOOST_AUTO_TEST_CASE(a_412_on_create_only_is_a_precondition_failure) {
    MockS3 mock;
    mock.responders.push_back([](const recorded_request&, httplib::Response& res) {
        res.status = 412;
        res.set_content(
            s3_error_xml("PreconditionFailed",
                         "At least one of the pre-conditions you specified did not hold"),
            "application/xml");
    });
    mock.start();

    const aws_s3_client client{config_for(mock)};
    try {
        client.put_object_if("b", "raft/log/7", "v", precondition{if_absent{}});
        BOOST_FAIL("expected object_precondition_failed");
    } catch (const object_precondition_failed& e) {
        BOOST_TEST(e.provider() == "s3");
        BOOST_TEST(e.key() == "raft/log/7");
        // Empty expected version is how "must not exist" is carried.
        BOOST_TEST(e.expected_version().empty());
        BOOST_TEST(std::string(e.what()).find("expected: absent") != std::string::npos);
    }
}

BOOST_AUTO_TEST_CASE(a_412_on_if_match_carries_the_version_it_lost_on) {
    MockS3 mock;
    mock.responders.push_back([](const recorded_request&, httplib::Response& res) {
        res.status = 412;
        res.set_content(s3_error_xml("PreconditionFailed", "did not hold"), "application/xml");
    });
    mock.start();

    const aws_s3_client client{config_for(mock)};
    try {
        client.put_object_if("b", "raft/term", "v", precondition{if_version{"\"stale\""}});
        BOOST_FAIL("expected object_precondition_failed");
    } catch (const object_precondition_failed& e) {
        BOOST_TEST(e.expected_version() == "\"stale\"");
    }
}

BOOST_AUTO_TEST_CASE(a_412_on_conditional_delete_is_a_precondition_failure) {
    MockS3 mock;
    mock.responders.push_back([](const recorded_request&, httplib::Response& res) {
        res.status = 412;
        res.set_content(s3_error_xml("PreconditionFailed", "did not hold"), "application/xml");
    });
    mock.start();

    const aws_s3_client client{config_for(mock)};
    BOOST_CHECK_THROW(client.delete_object_if("b", "k", precondition{if_version{"\"stale\""}}),
                      object_precondition_failed);
}

// The case that costs the most if it is wrong, and the one no live run could
// produce (spike-notes.md Finding 10: 53 racing losers, zero 409). AWS
// documents 409 `ConditionalRequestConflict` as "retry"; reporting it as a
// precondition failure would latch a healthy engine's fence permanently, and no
// operator action clears that latch.
BOOST_AUTO_TEST_CASE(a_409_conditional_conflict_is_retryable_not_a_fence_latch) {
    MockS3 mock;
    mock.responders.push_back([](const recorded_request&, httplib::Response& res) {
        res.status = 409;
        res.set_content(
            s3_error_xml("ConditionalRequestConflict",
                         "Request failed because of a conflicting operation against this resource"),
            "application/xml");
    });
    mock.start();

    const aws_s3_client client{config_for(mock)};
    try {
        client.put_object_if("b", "k", "v", precondition{if_absent{}});
        BOOST_FAIL("expected a throw for 409");
    } catch (const object_precondition_failed&) {
        BOOST_FAIL(
            "409 ConditionalRequestConflict must NOT be reported as a precondition "
            "failure — that turns a retryable race into an unclearable fence latch");
    } catch (const std::runtime_error& e) {
        BOOST_TEST(std::string(e.what()).find("ConditionalRequestConflict") != std::string::npos);
    }
}

// The same 409 on a conditional DELETE, for the same reason.
BOOST_AUTO_TEST_CASE(a_409_on_conditional_delete_is_also_not_a_fence_latch) {
    MockS3 mock;
    mock.responders.push_back([](const recorded_request&, httplib::Response& res) {
        res.status = 409;
        res.set_content(s3_error_xml("ConditionalRequestConflict", "conflicting operation"),
                        "application/xml");
    });
    mock.start();

    const aws_s3_client client{config_for(mock)};
    try {
        client.delete_object_if("b", "k", precondition{if_version{"\"e1\""}});
        BOOST_FAIL("expected a throw for 409");
    } catch (const object_precondition_failed&) {
        BOOST_FAIL("409 ConditionalRequestConflict must NOT latch on DELETE either");
    } catch (const std::runtime_error& e) {
        BOOST_TEST(std::string(e.what()).find("ConditionalRequestConflict") != std::string::npos);
    }
}

// An unconditional PUT that somehow draws a 412 is not a precondition failure:
// there was no precondition to fail. The engine's fence latches on
// `object_precondition_failed` alone, so this is the difference between a
// confusing error and a permanently wedged node.
BOOST_AUTO_TEST_CASE(an_unconditional_put_never_reports_a_precondition_failure) {
    MockS3 mock;
    mock.responders.push_back([](const recorded_request&, httplib::Response& res) {
        res.status = 412;
        res.set_content(s3_error_xml("PreconditionFailed", "did not hold"), "application/xml");
    });
    mock.start();

    const aws_s3_client client{config_for(mock)};
    try {
        client.put_object("b", "k", "v");
        BOOST_FAIL("expected a throw");
    } catch (const object_precondition_failed&) {
        BOOST_FAIL("an unconditional PUT carries no precondition to fail");
    } catch (const std::runtime_error& e) {
        BOOST_TEST(std::string(e.what()).find("PreconditionFailed") != std::string::npos);
    }
}

BOOST_AUTO_TEST_SUITE_END()

// ── Listing ──────────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(aws_s3_listing)

BOOST_AUTO_TEST_CASE(a_single_page_listing_returns_every_key) {
    MockS3 mock;
    mock.responders.push_back([](const recorded_request&, httplib::Response& res) {
        res.set_content(
            listing_xml(contents_entry("raft/term") + contents_entry("raft/log/001"), false),
            "application/xml");
    });
    mock.start();

    const aws_s3_client client{config_for(mock)};
    const auto keys = client.list_keys("b", "raft/");
    BOOST_REQUIRE(keys.size() == 2U);
    BOOST_TEST(keys[0] == "raft/term");
    BOOST_TEST(keys[1] == "raft/log/001");

    const auto reqs = mock.requests();
    BOOST_REQUIRE(reqs.size() == 1U);
    BOOST_TEST(reqs[0].param("list-type") == "2");
    BOOST_TEST(reqs[0].param("prefix") == "raft/");
}

// A persistence engine that read only the first page would silently lose the
// tail of its log, and would look healthy doing it.
BOOST_AUTO_TEST_CASE(pagination_is_followed_to_exhaustion) {
    MockS3 mock;
    mock.responders.push_back([](const recorded_request& req, httplib::Response& res) {
        BOOST_TEST(req.param("continuation-token").empty());
        res.set_content(listing_xml(contents_entry("a"), true, "TOKEN-2"), "application/xml");
    });
    mock.responders.push_back([](const recorded_request&, httplib::Response& res) {
        res.set_content(listing_xml(contents_entry("b"), false), "application/xml");
    });
    mock.start();

    const aws_s3_client client{config_for(mock)};
    const auto keys = client.list_keys("b", "");
    BOOST_REQUIRE(keys.size() == 2U);
    BOOST_TEST(keys[0] == "a");
    BOOST_TEST(keys[1] == "b");

    const auto reqs = mock.requests();
    BOOST_REQUIRE(reqs.size() == 2U);
    BOOST_TEST(reqs[1].param("continuation-token") == "TOKEN-2");
}

// A page that says "there is more" and then does not say where is a listing
// this client cannot complete. Returning what it has would be a short answer
// indistinguishable from a complete one.
BOOST_AUTO_TEST_CASE(a_truncated_page_without_a_continuation_token_throws) {
    MockS3 mock;
    mock.responders.push_back([](const recorded_request&, httplib::Response& res) {
        res.set_content(listing_xml(contents_entry("a"), true), "application/xml");
    });
    mock.start();

    const aws_s3_client client{config_for(mock)};
    BOOST_CHECK_THROW((void)client.list_keys("b", ""), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(a_failed_page_throws_rather_than_truncating) {
    MockS3 mock;
    mock.responders.push_back([](const recorded_request&, httplib::Response& res) {
        res.set_content(listing_xml(contents_entry("a"), true, "TOKEN-2"), "application/xml");
    });
    mock.responders.push_back([](const recorded_request&, httplib::Response& res) {
        res.status = 500;
        res.set_content(s3_error_xml("InternalError", "We encountered an internal error"),
                        "application/xml");
    });
    mock.start();

    const aws_s3_client client{config_for(mock)};
    BOOST_CHECK_THROW((void)client.list_keys("b", ""), std::runtime_error);
}

BOOST_AUTO_TEST_SUITE_END()

// ── Zero retries, counted rather than assumed ────────────────────────────────

BOOST_AUTO_TEST_SUITE(aws_s3_retry_policy)

// Requirement 12.2. The SDK's default strategy would retry a 500 up to ten
// times; the engine's single PUT retry has to be the only retry, or its
// idempotency argument is unverifiable from the outside. Counted from the
// server's own log, which is the only place the SDK's behaviour is visible.
BOOST_AUTO_TEST_CASE(a_failing_put_is_issued_exactly_once) {
    MockS3 mock;
    mock.responders.push_back([](const recorded_request&, httplib::Response& res) {
        res.status = 500;
        res.set_content(s3_error_xml("InternalError", "We encountered an internal error"),
                        "application/xml");
    });
    mock.start();

    const aws_s3_client client{config_for(mock)};
    BOOST_CHECK_THROW(client.put_object("b", "k", "v"), std::runtime_error);
    BOOST_TEST(mock.request_count() == 1U);
}

// 503 SlowDown is the SDK's canonical retryable error, so it is the sharper
// test of the zeroed strategy than a bare 500.
BOOST_AUTO_TEST_CASE(a_throttled_put_is_also_issued_exactly_once) {
    MockS3 mock;
    mock.responders.push_back([](const recorded_request&, httplib::Response& res) {
        res.status = 503;
        res.set_content(s3_error_xml("SlowDown", "Please reduce your request rate"),
                        "application/xml");
    });
    mock.start();

    const aws_s3_client client{config_for(mock)};
    BOOST_CHECK_THROW(client.put_object("b", "k", "v"), std::runtime_error);
    BOOST_TEST(mock.request_count() == 1U);
}

BOOST_AUTO_TEST_CASE(a_failing_get_is_issued_exactly_once) {
    MockS3 mock;
    mock.responders.push_back([](const recorded_request&, httplib::Response& res) {
        res.status = 503;
        res.set_content(s3_error_xml("SlowDown", "Please reduce your request rate"),
                        "application/xml");
    });
    mock.start();

    const aws_s3_client client{config_for(mock)};
    BOOST_CHECK_THROW((void)client.get_object("b", "k"), std::runtime_error);
    BOOST_TEST(mock.request_count() == 1U);
}

BOOST_AUTO_TEST_CASE(a_failing_list_is_issued_exactly_once) {
    MockS3 mock;
    mock.responders.push_back([](const recorded_request&, httplib::Response& res) {
        res.status = 503;
        res.set_content(s3_error_xml("SlowDown", "Please reduce your request rate"),
                        "application/xml");
    });
    mock.start();

    const aws_s3_client client{config_for(mock)};
    BOOST_CHECK_THROW((void)client.list_keys("b", "raft/"), std::runtime_error);
    BOOST_TEST(mock.request_count() == 1U);
}

BOOST_AUTO_TEST_SUITE_END()

#else  // KYTHIRA_HAS_AWS_SDK

BOOST_AUTO_TEST_CASE(aws_sdk_not_available) {
    BOOST_TEST_MESSAGE("AWS SDK not available — aws_s3_client tests skipped");
    BOOST_TEST(true);
}

#endif  // KYTHIRA_HAS_AWS_SDK
