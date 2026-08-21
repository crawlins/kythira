// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#define BOOST_TEST_MODULE azure_blob_client_unit_test
#include <boost/test/unit_test.hpp>

#include <raft/azure_blob_client.hpp>

// Unit coverage for `azure_blob_client` (cloud-object-persistence Requirement
// 13, task 8), against a local `httplib::Server` standing in for Blob REST.
//
// Every assertion about a request is made from the bytes that **arrived**, not
// from the client's own constants — task 8's verify bar names this explicitly
// for `x-ms-version`, and it applies to the rest for the same reason: a client's
// entire value is what it puts on the wire.
//
// The four cases that would cost the most if wrong:
//
//  1. **Both of Azure's two rejection statuses latch.** 409 `BlobAlreadyExists`
//     for a lost `If-None-Match: *` and 412 `ConditionNotMet` for a lost
//     `If-Match`. Requirement 13.4 originally named only the 412; mapping only
//     that one would let a create-only collision — the exact case that catches a
//     stale leader appending log entries — pass as an ordinary error and fence
//     nothing.
//  2. **201 on PUT and 202 on DELETE are success.** A client that accepts only
//     200 fails every write it makes.
//  3. **A DELETE of an absent blob succeeds**, although Azure answers 404 where
//     S3 and OSS answer 204. The engine's log truncation depends on it.
//  4. **An unrecognised listing body is an error, not an empty container.** Blob
//     has no `IsTruncated` flag, so "zero blobs" and "I could not read this" are
//     otherwise the same answer — and to a persistence engine that answer is an
//     empty log.

#ifdef KYTHIRA_HAS_AZURE_SDK

#include <azure/core/context.hpp>
#include <azure/core/credentials/credentials.hpp>
#include <azure/core/datetime.hpp>

#include <httplib.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace kythira;
using namespace std::chrono_literals;

namespace {

/// A credential that mints a fixed token and counts how often it is asked, so
/// the suite can show the token is cached rather than re-fetched per request.
struct counting_credential : Azure::Core::Credentials::TokenCredential {
    mutable std::atomic<int> calls{0};
    std::chrono::seconds lifetime{3600};
    std::string token{"test-token"};

    counting_credential() : Azure::Core::Credentials::TokenCredential("counting_credential") {}

    auto GetToken(Azure::Core::Credentials::TokenRequestContext const& ctx,
                  Azure::Core::Context const&) const
        -> Azure::Core::Credentials::AccessToken override {
        ++calls;
        last_scope = ctx.Scopes.empty() ? std::string{} : ctx.Scopes.front();
        Azure::Core::Credentials::AccessToken out;
        out.Token = token;
        out.ExpiresOn = Azure::DateTime(std::chrono::system_clock::now() + lifetime);
        return out;
    }

    mutable std::string last_scope;
};

/// What the server saw. Recorded and asserted on the test thread rather than
/// inside the handler, where a failed assertion reports against the wrong case.
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

struct MockBlob {
    httplib::Server server;
    std::thread thread;
    int port{0};

    mutable std::mutex mu;
    std::vector<recorded_request> log;

    /// Answers the Nth request (0-based); the last entry answers every
    /// subsequent one, so a single-element vector is "always this".
    std::vector<std::function<void(const recorded_request&, httplib::Response&)>> responders;

    MockBlob() {
        auto handler = [this](const httplib::Request& req, httplib::Response& res) {
            recorded_request rec{req.method, req.path, req.body, req.headers, req.params};
            std::size_t index = 0;
            {
                const std::lock_guard lock(mu);
                index = log.size();
                log.push_back(rec);
            }
            if (responders.empty()) {
                res.status = 201;
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

    ~MockBlob() {
        server.stop();
        if (thread.joinable()) {
            thread.join();
        }
    }

    MockBlob(const MockBlob&) = delete;
    auto operator=(const MockBlob&) -> MockBlob& = delete;
    MockBlob(MockBlob&&) = delete;
    auto operator=(MockBlob&&) -> MockBlob& = delete;

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

auto config_for(const MockBlob& mock, std::shared_ptr<counting_credential> cred = {})
    -> azure_blob_config {
    azure_blob_config cfg;
    cfg.account = "kythirarealtestobj";
    cfg.endpoint_override = mock.origin();
    cfg.azure.api_timeout = 5s;
    cfg.azure.credential = cred ? std::move(cred) : std::make_shared<counting_credential>();
    return cfg;
}

/// An Azure error body. The client prefers the `x-ms-error-code` header, so a
/// responder that wants to exercise the fallback sets only this.
auto azure_error_xml(const std::string& code, const std::string& message) -> std::string {
    return "<?xml version=\"1.0\" encoding=\"utf-8\"?><Error><Code>" + code + "</Code><Message>" +
           message + "</Message></Error>";
}

auto blob_entry(const std::string& name) -> std::string {
    return "<Blob><Name>" + name +
           "</Name><Properties><Content-Length>0</Content-Length>"
           "<Etag>0x8DEFC5FC0A7294B</Etag></Properties></Blob>";
}

auto listing_xml(const std::string& blobs, const std::string& next_marker = {}) -> std::string {
    return "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
           "<EnumerationResults ContainerName=\"kythira-raft\"><Blobs>" +
           blobs + "</Blobs><NextMarker>" + next_marker + "</NextMarker></EnumerationResults>";
}

}  // namespace

// ── Concept obligations, including the negative one ──────────────────────────

BOOST_AUTO_TEST_SUITE(azure_blob_concepts)

// The negative is the load-bearing one. Azure's ETag is an opaque timestamp
// token, so declaring `version_is_content_md5` would make the engine compare
// that token against an MD5 and reject every write it makes.
BOOST_AUTO_TEST_CASE(the_store_concepts_this_client_satisfies) {
    static_assert(key_object_store<azure_blob_client>);
    static_assert(conditional_key_object_store<azure_blob_client>);
    static_assert(!content_md5_versioned_store<azure_blob_client>);
    BOOST_TEST(azure_blob_client::provider_name() == "azure-blob");
}

BOOST_AUTO_TEST_CASE(an_unpinned_api_version_is_refused_at_construction) {
    azure_blob_config cfg;
    cfg.account = "acct";
    cfg.api_version.clear();
    BOOST_CHECK_THROW(azure_blob_client{cfg}, std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(a_client_that_names_nowhere_to_put_blobs_is_refused) {
    azure_blob_config cfg;  // no account, no endpoint_override
    BOOST_CHECK_THROW(azure_blob_client{cfg}, std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(addressing_is_account_host_by_default_and_the_override_otherwise) {
    azure_blob_config cfg;
    cfg.account = "kythirarealtestobj";
    const azure_blob_client real{cfg};
    const auto [origin, path] = real.origin_and_path("kythira-raft", "raft/term");
    BOOST_TEST(origin == "https://kythirarealtestobj.blob.core.windows.net");
    BOOST_TEST(path == "/kythira-raft/raft/term");

    // Azurite puts the account in the path, so the override carries it.
    cfg.endpoint_override = "http://127.0.0.1:10000/devstoreaccount1";
    const azure_blob_client emulated{cfg};
    const auto [mock_origin, mock_path] = emulated.origin_and_path("kythira-raft", "raft/term");
    BOOST_TEST(mock_origin == "http://127.0.0.1:10000/devstoreaccount1");
    BOOST_TEST(mock_path == "/kythira-raft/raft/term");
}

BOOST_AUTO_TEST_SUITE_END()

// ── Request shape ────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(azure_blob_request_shape)

// Task 8's verify bar, worded as "asserted from received bytes, not from the
// client's own constant" — so this reads the header off the server's log.
BOOST_AUTO_TEST_CASE(the_pinned_api_version_appears_on_every_request) {
    MockBlob mock;
    mock.responders.push_back([](const recorded_request& req, httplib::Response& res) {
        if (req.method == "GET" && req.path == "/c") {
            res.set_content(listing_xml(blob_entry("raft/term")), "application/xml");
            return;
        }
        res.set_header("ETag", "\"0x8DEF\"");
        res.status = req.method == "PUT" ? 201 : (req.method == "DELETE" ? 202 : 200);
    });
    mock.start();

    const azure_blob_client client{config_for(mock)};
    client.put_object("c", "raft/term", "42");
    (void)client.get_object("c", "raft/term");
    client.delete_object("c", "raft/term");
    (void)client.list_keys("c", "raft/");

    const auto reqs = mock.requests();
    BOOST_REQUIRE(reqs.size() == 4U);
    for (const auto& req : reqs) {
        BOOST_TEST(req.header("x-ms-version") == "2023-11-03");
        BOOST_TEST(req.header("Authorization") == "Bearer test-token");
    }
}

// `x-ms-blob-type: BlockBlob` is required on PUT and meaningless elsewhere.
BOOST_AUTO_TEST_CASE(blob_type_is_sent_on_put_and_only_on_put) {
    MockBlob mock;
    mock.responders.push_back([](const recorded_request& req, httplib::Response& res) {
        res.set_header("ETag", "\"e\"");
        res.status = req.method == "PUT" ? 201 : 200;
    });
    mock.start();

    const azure_blob_client client{config_for(mock)};
    client.put_object("c", "k", "v");
    (void)client.get_object("c", "k");

    const auto reqs = mock.requests();
    BOOST_REQUIRE(reqs.size() == 2U);
    BOOST_TEST(reqs[0].header("x-ms-blob-type") == "BlockBlob");
    BOOST_TEST(!reqs[1].has_header("x-ms-blob-type"));
}

// Requirement 7.1's service-side half: base64 of the raw digest, which Azure
// rejects with 400 Md5Mismatch. Known answer written out rather than
// recomputed, because the ETag spelling of a digest is hex and the two are the
// obvious thing to confuse — even though on Azure the ETag is not a digest at
// all.
BOOST_AUTO_TEST_CASE(every_put_carries_content_md5_as_base64) {
    MockBlob mock;
    mock.responders.push_back([](const recorded_request&, httplib::Response& res) {
        res.set_header("ETag", "\"e\"");
        res.status = 201;
    });
    mock.start();

    const azure_blob_client client{config_for(mock)};
    client.put_object("c", "k", "hello");

    const auto reqs = mock.requests();
    BOOST_REQUIRE(reqs.size() == 1U);
    // md5("hello") = 5d41402abc4b2a76b9719d911017c592
    BOOST_TEST(reqs[0].header("Content-MD5") == "XUFAKrxLKna5cZ2REBfFkg==");
}

// The token is fetched once and reused: a credential call per request would put
// an interactive CLI invocation on the write path of a consensus algorithm.
BOOST_AUTO_TEST_CASE(the_bearer_token_is_fetched_once_and_cached) {
    MockBlob mock;
    mock.responders.push_back([](const recorded_request&, httplib::Response& res) {
        res.set_header("ETag", "\"e\"");
        res.status = 201;
    });
    mock.start();

    auto cred = std::make_shared<counting_credential>();
    const azure_blob_client client{config_for(mock, cred)};
    client.put_object("c", "k", "v");
    client.put_object("c", "k", "v");
    client.put_object("c", "k", "v");

    BOOST_TEST(cred->calls.load() == 1);
    BOOST_TEST(cred->last_scope == "https://storage.azure.com/.default");
}

// ...and a token already inside the refresh margin is replaced rather than
// sent. A token that is valid when the header is built can still be expired
// when the service evaluates it, and a failed write here is not free.
BOOST_AUTO_TEST_CASE(a_token_near_expiry_is_refetched) {
    MockBlob mock;
    mock.responders.push_back([](const recorded_request&, httplib::Response& res) {
        res.set_header("ETag", "\"e\"");
        res.status = 201;
    });
    mock.start();

    auto cred = std::make_shared<counting_credential>();
    cred->lifetime = 60s;  // inside the 300 s refresh margin
    const azure_blob_client client{config_for(mock, cred)};
    client.put_object("c", "k", "v");
    client.put_object("c", "k", "v");

    BOOST_TEST(cred->calls.load() == 2);
}

BOOST_AUTO_TEST_CASE(create_only_sends_if_none_match_star) {
    MockBlob mock;
    mock.responders.push_back([](const recorded_request&, httplib::Response& res) {
        res.set_header("ETag", "\"0x8DEF\"");
        res.status = 201;
    });
    mock.start();

    const azure_blob_client client{config_for(mock)};
    const auto result = client.put_object_if("c", "k", "v", precondition{if_absent{}});

    const auto reqs = mock.requests();
    BOOST_REQUIRE(reqs.size() == 1U);
    BOOST_TEST(reqs[0].header("If-None-Match") == "*");
    BOOST_TEST(!reqs[0].has_header("If-Match"));
    BOOST_TEST(result.version == "\"0x8DEF\"");
}

BOOST_AUTO_TEST_CASE(overwrite_precondition_sends_if_match) {
    MockBlob mock;
    mock.responders.push_back([](const recorded_request&, httplib::Response& res) {
        res.set_header("ETag", "\"0x8DF0\"");
        res.status = 201;
    });
    mock.start();

    const azure_blob_client client{config_for(mock)};
    client.put_object_if("c", "k", "v", precondition{if_version{"\"0x8DEF\""}});

    const auto reqs = mock.requests();
    BOOST_REQUIRE(reqs.size() == 1U);
    BOOST_TEST(reqs[0].header("If-Match") == "\"0x8DEF\"");
    BOOST_TEST(!reqs[0].has_header("If-None-Match"));
}

BOOST_AUTO_TEST_SUITE_END()

// ── Status codes, which are not the ones a reader would guess ────────────────

BOOST_AUTO_TEST_SUITE(azure_blob_status_handling)

// 201, not 200. A client that accepted only 200 would fail every write.
BOOST_AUTO_TEST_CASE(a_201_is_a_successful_put) {
    MockBlob mock;
    mock.responders.push_back([](const recorded_request&, httplib::Response& res) {
        res.set_header("ETag", "\"0x8DEF\"");
        res.status = 201;
    });
    mock.start();

    const azure_blob_client client{config_for(mock)};
    BOOST_TEST(client.put_object("c", "k", "v").version == "\"0x8DEF\"");
}

// 202, not 204.
BOOST_AUTO_TEST_CASE(a_202_is_a_successful_delete) {
    MockBlob mock;
    mock.responders.push_back(
        [](const recorded_request&, httplib::Response& res) { res.status = 202; });
    mock.start();

    const azure_blob_client client{config_for(mock)};
    BOOST_CHECK_NO_THROW(client.delete_object("c", "k"));
}

// Azure answers 404 where S3 and OSS answer 204. The engine's truncation path
// deletes keys it is not certain exist, so a non-idempotent delete would turn
// ordinary recovery into an exception.
BOOST_AUTO_TEST_CASE(deleting_an_absent_blob_succeeds) {
    MockBlob mock;
    mock.responders.push_back([](const recorded_request&, httplib::Response& res) {
        res.status = 404;
        res.set_header("x-ms-error-code", "BlobNotFound");
        res.set_content(azure_error_xml("BlobNotFound", "The specified blob does not exist."),
                        "application/xml");
    });
    mock.start();

    const azure_blob_client client{config_for(mock)};
    BOOST_CHECK_NO_THROW(client.delete_object("c", "absent"));
}

// The other 404. A missing container is a misconfiguration that must surface —
// swallowing it would let an engine pointed at a non-existent container start
// up as though its prefix were merely empty, and then delete "successfully".
BOOST_AUTO_TEST_CASE(a_missing_container_is_an_error_on_both_get_and_delete) {
    MockBlob mock;
    mock.responders.push_back([](const recorded_request&, httplib::Response& res) {
        res.status = 404;
        res.set_header("x-ms-error-code", "ContainerNotFound");
        res.set_content(azure_error_xml("ContainerNotFound",
                                        "The specified container does not "
                                        "exist."),
                        "application/xml");
    });
    mock.start();

    const azure_blob_client client{config_for(mock)};
    BOOST_CHECK_THROW((void)client.get_object("c", "k"), std::runtime_error);
    BOOST_CHECK_THROW(client.delete_object("c", "k"), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(an_absent_blob_reads_back_as_nullopt) {
    MockBlob mock;
    mock.responders.push_back([](const recorded_request&, httplib::Response& res) {
        res.status = 404;
        res.set_header("x-ms-error-code", "BlobNotFound");
        res.set_content(azure_error_xml("BlobNotFound", "not here"), "application/xml");
    });
    mock.start();

    const azure_blob_client client{config_for(mock)};
    BOOST_TEST(!client.get_object("c", "absent").has_value());
}

BOOST_AUTO_TEST_CASE(the_error_code_header_and_message_reach_the_exception) {
    MockBlob mock;
    mock.responders.push_back([](const recorded_request&, httplib::Response& res) {
        res.status = 403;
        res.set_header("x-ms-error-code", "AuthorizationPermissionMismatch");
        res.set_content(azure_error_xml("AuthorizationPermissionMismatch",
                                        "This request is not authorized to perform this "
                                        "operation using this permission."),
                        "application/xml");
    });
    mock.start();

    const azure_blob_client client{config_for(mock)};
    try {
        client.put_object("c", "k", "v");
        BOOST_FAIL("expected a runtime_error");
    } catch (const std::runtime_error& e) {
        const std::string what = e.what();
        BOOST_TEST(what.find("AuthorizationPermissionMismatch") != std::string::npos);
        BOOST_TEST(what.find("403") != std::string::npos);
        BOOST_TEST(what.find("not authorized") != std::string::npos);
    }
}

// The header is the documented channel, but a proxy or emulator can drop it,
// and an error whose code is unreadable must not become a different verdict.
BOOST_AUTO_TEST_CASE(the_error_code_falls_back_to_the_xml_body) {
    MockBlob mock;
    mock.responders.push_back([](const recorded_request&, httplib::Response& res) {
        res.status = 404;  // no x-ms-error-code header at all
        res.set_content(azure_error_xml("BlobNotFound", "not here"), "application/xml");
    });
    mock.start();

    const azure_blob_client client{config_for(mock)};
    BOOST_CHECK_NO_THROW(client.delete_object("c", "absent"));
}

BOOST_AUTO_TEST_SUITE_END()

// ── One provider, two rejection statuses, both meaning "you lost" ────────────

BOOST_AUTO_TEST_SUITE(azure_blob_conditional_verdicts)

// The case Requirement 13.4 was CORRECTED for. Azure answers 409 — not 412 — to
// a lost create-only race, and this is the race that catches a stale leader
// appending log entries. Mapping only the 412 would fence nothing.
BOOST_AUTO_TEST_CASE(a_409_blob_already_exists_is_a_precondition_failure) {
    MockBlob mock;
    mock.responders.push_back([](const recorded_request&, httplib::Response& res) {
        res.status = 409;
        res.set_header("x-ms-error-code", "BlobAlreadyExists");
        res.set_content(azure_error_xml("BlobAlreadyExists", "The specified blob already exists."),
                        "application/xml");
    });
    mock.start();

    const azure_blob_client client{config_for(mock)};
    try {
        client.put_object_if("c", "raft/log/7", "v", precondition{if_absent{}});
        BOOST_FAIL("expected object_precondition_failed for 409 BlobAlreadyExists");
    } catch (const object_precondition_failed& e) {
        BOOST_TEST(e.provider() == "azure-blob");
        BOOST_TEST(e.key() == "raft/log/7");
        BOOST_TEST(e.expected_version().empty());
        BOOST_TEST(std::string(e.what()).find("expected: absent") != std::string::npos);
    }
}

BOOST_AUTO_TEST_CASE(a_412_condition_not_met_is_a_precondition_failure) {
    MockBlob mock;
    mock.responders.push_back([](const recorded_request&, httplib::Response& res) {
        res.status = 412;
        res.set_header("x-ms-error-code", "ConditionNotMet");
        res.set_content(azure_error_xml("ConditionNotMet",
                                        "The condition specified using HTTP "
                                        "conditional header(s) is not met."),
                        "application/xml");
    });
    mock.start();

    const azure_blob_client client{config_for(mock)};
    try {
        client.put_object_if("c", "raft/term", "v", precondition{if_version{"\"0x8DEF\""}});
        BOOST_FAIL("expected object_precondition_failed for 412 ConditionNotMet");
    } catch (const object_precondition_failed& e) {
        BOOST_TEST(e.expected_version() == "\"0x8DEF\"");
    }
}

BOOST_AUTO_TEST_CASE(a_412_on_conditional_delete_is_a_precondition_failure) {
    MockBlob mock;
    mock.responders.push_back([](const recorded_request&, httplib::Response& res) {
        res.status = 412;
        res.set_header("x-ms-error-code", "ConditionNotMet");
        res.set_content(azure_error_xml("ConditionNotMet", "not met"), "application/xml");
    });
    mock.start();

    const azure_blob_client client{config_for(mock)};
    BOOST_CHECK_THROW(client.delete_object_if("c", "k", precondition{if_version{"\"0x8DEF\""}}),
                      object_precondition_failed);
}

// A 409 that is *not* a lost create-only race must not latch. Azure uses 409 for
// leases and for a container being deleted, and S3 uses it for the opposite of a
// precondition failure entirely — which is why the verdict is keyed on the error
// code and never on the status.
BOOST_AUTO_TEST_CASE(a_409_that_is_not_blob_already_exists_does_not_latch) {
    MockBlob mock;
    mock.responders.push_back([](const recorded_request&, httplib::Response& res) {
        res.status = 409;
        res.set_header("x-ms-error-code", "LeaseIdMissing");
        res.set_content(
            azure_error_xml("LeaseIdMissing", "There is currently a lease on the blob."),
            "application/xml");
    });
    mock.start();

    const azure_blob_client client{config_for(mock)};
    try {
        client.put_object_if("c", "k", "v", precondition{if_absent{}});
        BOOST_FAIL("expected a throw for 409 LeaseIdMissing");
    } catch (const object_precondition_failed&) {
        BOOST_FAIL(
            "a 409 that is not BlobAlreadyExists must not be read as a lost precondition — "
            "the verdict is the error code, never the status");
    } catch (const std::runtime_error& e) {
        BOOST_TEST(std::string(e.what()).find("LeaseIdMissing") != std::string::npos);
    }
}

// An unconditional PUT carries no precondition to fail, so even a 409
// BlobAlreadyExists is an ordinary error there. The engine's fence latches on
// `object_precondition_failed` alone.
BOOST_AUTO_TEST_CASE(an_unconditional_put_never_reports_a_precondition_failure) {
    MockBlob mock;
    mock.responders.push_back([](const recorded_request&, httplib::Response& res) {
        res.status = 409;
        res.set_header("x-ms-error-code", "BlobAlreadyExists");
        res.set_content(azure_error_xml("BlobAlreadyExists", "exists"), "application/xml");
    });
    mock.start();

    const azure_blob_client client{config_for(mock)};
    try {
        client.put_object("c", "k", "v");
        BOOST_FAIL("expected a throw");
    } catch (const object_precondition_failed&) {
        BOOST_FAIL("an unconditional PUT carries no precondition to fail");
    } catch (const std::runtime_error& e) {
        BOOST_TEST(std::string(e.what()).find("BlobAlreadyExists") != std::string::npos);
    }
}

BOOST_AUTO_TEST_SUITE_END()

// ── Listing ──────────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(azure_blob_listing)

BOOST_AUTO_TEST_CASE(a_single_page_listing_returns_every_name) {
    MockBlob mock;
    mock.responders.push_back([](const recorded_request&, httplib::Response& res) {
        res.set_content(listing_xml(blob_entry("raft/term") + blob_entry("raft/log/001")),
                        "application/xml");
    });
    mock.start();

    const azure_blob_client client{config_for(mock)};
    const auto keys = client.list_keys("c", "raft/");
    BOOST_REQUIRE(keys.size() == 2U);
    BOOST_TEST(keys[0] == "raft/term");
    BOOST_TEST(keys[1] == "raft/log/001");

    const auto reqs = mock.requests();
    BOOST_REQUIRE(reqs.size() == 1U);
    BOOST_TEST(reqs[0].param("restype") == "container");
    BOOST_TEST(reqs[0].param("comp") == "list");
    BOOST_TEST(reqs[0].param("prefix") == "raft/");
}

// A persistence engine that read only the first page would silently lose the
// tail of its log and look healthy doing it.
BOOST_AUTO_TEST_CASE(pagination_follows_next_marker_to_exhaustion) {
    MockBlob mock;
    mock.responders.push_back([](const recorded_request&, httplib::Response& res) {
        res.set_content(listing_xml(blob_entry("a"), "MARKER-2"), "application/xml");
    });
    mock.responders.push_back([](const recorded_request&, httplib::Response& res) {
        res.set_content(listing_xml(blob_entry("b")), "application/xml");
    });
    mock.start();

    const azure_blob_client client{config_for(mock)};
    const auto keys = client.list_keys("c", "");
    BOOST_REQUIRE(keys.size() == 2U);
    BOOST_TEST(keys[0] == "a");
    BOOST_TEST(keys[1] == "b");

    const auto reqs = mock.requests();
    BOOST_REQUIRE(reqs.size() == 2U);
    BOOST_TEST(reqs[0].param("marker").empty());
    BOOST_TEST(reqs[1].param("marker") == "MARKER-2");
}

// Blob has no IsTruncated flag, so "I could not read this" and "the container is
// empty" are otherwise the same answer — and to a persistence engine that answer
// is an empty log. The structural guard is that the body must actually be an
// EnumerationResults document.
BOOST_AUTO_TEST_CASE(a_body_that_is_not_an_enumeration_result_throws) {
    MockBlob mock;
    mock.responders.push_back([](const recorded_request&, httplib::Response& res) {
        res.set_content("<html><body>200 OK from a proxy</body></html>", "text/html");
    });
    mock.start();

    const azure_blob_client client{config_for(mock)};
    BOOST_CHECK_THROW((void)client.list_keys("c", ""), std::runtime_error);
}

// Task 8's verify bar names this case specifically.
BOOST_AUTO_TEST_CASE(a_truncated_xml_body_throws) {
    MockBlob mock;
    mock.responders.push_back([](const recorded_request&, httplib::Response& res) {
        res.set_content("<EnumerationResults><Blobs><Blob><Name>raft/term", "application/xml");
    });
    mock.start();

    const azure_blob_client client{config_for(mock)};
    BOOST_CHECK_THROW((void)client.list_keys("c", ""), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(an_empty_container_lists_as_no_keys) {
    MockBlob mock;
    mock.responders.push_back([](const recorded_request&, httplib::Response& res) {
        res.set_content(listing_xml(""), "application/xml");
    });
    mock.start();

    const azure_blob_client client{config_for(mock)};
    BOOST_TEST(client.list_keys("c", "raft/").empty());
}

// Blob listings have no `encoding-type` escape hatch, so a name containing an
// XML metacharacter arrives escaped. The engine's own keys never contain one; a
// foreign key under the same prefix can, and returning it mangled would make the
// listing disagree with the store.
BOOST_AUTO_TEST_CASE(xml_entities_in_a_name_are_decoded) {
    MockBlob mock;
    mock.responders.push_back([](const recorded_request&, httplib::Response& res) {
        res.set_content(listing_xml(blob_entry("raft/a&amp;b&lt;c&gt;d&quot;e&apos;f")),
                        "application/xml");
    });
    mock.start();

    const azure_blob_client client{config_for(mock)};
    const auto keys = client.list_keys("c", "raft/");
    BOOST_REQUIRE(keys.size() == 1U);
    BOOST_TEST(keys[0] == "raft/a&b<c>d\"e'f");
}

BOOST_AUTO_TEST_CASE(a_failed_page_throws_rather_than_truncating) {
    MockBlob mock;
    mock.responders.push_back([](const recorded_request&, httplib::Response& res) {
        res.set_content(listing_xml(blob_entry("a"), "MARKER-2"), "application/xml");
    });
    mock.responders.push_back([](const recorded_request&, httplib::Response& res) {
        res.status = 500;
        res.set_header("x-ms-error-code", "InternalError");
        res.set_content(azure_error_xml("InternalError",
                                        "The server encountered an internal "
                                        "error."),
                        "application/xml");
    });
    mock.start();

    const azure_blob_client client{config_for(mock)};
    BOOST_CHECK_THROW((void)client.list_keys("c", ""), std::runtime_error);
}

BOOST_AUTO_TEST_SUITE_END()

// ── Round trips ──────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(azure_blob_operations)

BOOST_AUTO_TEST_CASE(put_get_delete_round_trip) {
    MockBlob mock;
    std::string stored;
    bool present = false;
    mock.responders.push_back([&](const recorded_request& req, httplib::Response& res) {
        if (req.method == "PUT") {
            stored = req.body;
            present = true;
            res.set_header("ETag", "\"0x8DEF\"");
            res.status = 201;
        } else if (req.method == "GET") {
            if (!present) {
                res.status = 404;
                res.set_header("x-ms-error-code", "BlobNotFound");
                res.set_content(azure_error_xml("BlobNotFound", "not here"), "application/xml");
                return;
            }
            res.set_header("ETag", "\"0x8DEF\"");
            res.set_content(stored, "application/octet-stream");
        } else {
            present = false;
            res.status = 202;
        }
    });
    mock.start();

    const azure_blob_client client{config_for(mock)};
    BOOST_TEST(client.put_object("c", "raft/term", "42").version == "\"0x8DEF\"");

    const auto got = client.get_object("c", "raft/term");
    BOOST_REQUIRE(got.has_value());
    BOOST_TEST(got->body == "42");
    BOOST_TEST(got->version == "\"0x8DEF\"");

    client.delete_object("c", "raft/term");
    BOOST_TEST(!client.get_object("c", "raft/term").has_value());
}

BOOST_AUTO_TEST_CASE(binary_bytes_survive_a_round_trip) {
    MockBlob mock;
    std::string stored;
    mock.responders.push_back([&](const recorded_request& req, httplib::Response& res) {
        if (req.method == "PUT") {
            stored = req.body;
            res.set_header("ETag", "\"e\"");
            res.status = 201;
        } else {
            res.set_header("ETag", "\"e\"");
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

    const azure_blob_client client{config_for(mock)};
    client.put_object("c", "blob", payload);
    const auto got = client.get_object("c", "blob");
    BOOST_REQUIRE(got.has_value());
    BOOST_TEST(got->body == payload);
}

BOOST_AUTO_TEST_SUITE_END()

#else  // KYTHIRA_HAS_AZURE_SDK

BOOST_AUTO_TEST_CASE(azure_sdk_not_available) {
    BOOST_TEST_MESSAGE("Azure SDK not available — azure_blob_client tests skipped");
    BOOST_TEST(true);
}

#endif  // KYTHIRA_HAS_AZURE_SDK
