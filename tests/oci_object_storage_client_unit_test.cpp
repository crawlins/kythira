#define BOOST_TEST_MODULE oci_object_storage_client_unit_test
#include <boost/test/unit_test.hpp>

#include <raft/oci_object_storage_client.hpp>
#include <raft/oci_signing.hpp>

#include <httplib.h>

#include <openssl/evp.h>
#include <openssl/pem.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

// Unit coverage for `oci_object_storage_client` (cloud-object-persistence
// Requirement 15, task 10), against a local `httplib::Server`.
//
// The four things that would cost the most if wrong:
//
//  1. **The signed content type reaches the wire.** Object bodies are
//     `application/octet-stream`; `oci_signing` signed `application/json` as a
//     constant until this task, and signing one while sending the other yields
//     `NotAuthenticated: Failed to verify the HTTP(S) Signature` — an error
//     naming neither side of the mismatch. Both halves are checked: the header
//     that arrived, and that the canonical signing string changes with it.
//  2. **`etag` is read case-insensitively.** OCI spells it lowercase. Task 0's
//     probe looked up `ETag`, got nothing, sent `if-match: ""`, and read the
//     resulting 412 as "OCI cannot do CAS" — the conclusion Alibaba OSS
//     genuinely earns. Only a negative control caught it.
//  3. **`BucketNotFound` is never an absent object.** It is both a real
//     misconfiguration and the code this tenancy's intermittent authorization
//     flake returns, so reading it as absence would let a flake present as an
//     empty Raft log.
//  4. **A listing that cannot be read is an error, not an empty bucket.**

using namespace kythira;
using namespace std::chrono_literals;

namespace {

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

/// The one key this binary signs with. Generating an RSA key costs real time,
/// and every case wants the same thing from it.
auto shared_key_pem() -> const std::string& {
    static const std::string pem = generate_rsa_key_pem();
    return pem;
}

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

struct MockObjectStorage {
    httplib::Server server;
    std::thread thread;
    int port{0};

    mutable std::mutex mu;
    std::vector<recorded_request> log;

    /// Answers the Nth request (0-based); the last entry answers every
    /// subsequent one.
    std::vector<std::function<void(const recorded_request&, httplib::Response&)>> responders;

    MockObjectStorage() {
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

    ~MockObjectStorage() {
        server.stop();
        if (thread.joinable()) {
            thread.join();
        }
    }

    MockObjectStorage(const MockObjectStorage&) = delete;
    auto operator=(const MockObjectStorage&) -> MockObjectStorage& = delete;
    MockObjectStorage(MockObjectStorage&&) = delete;
    auto operator=(MockObjectStorage&&) -> MockObjectStorage& = delete;

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

/// A configured namespace, so construction issues no request unless a case
/// wants it to.
auto config_for(const MockObjectStorage& mock, std::string ns = "axunmw4f0mln")
    -> oci_object_storage_config {
    oci_object_storage_config cfg;
    cfg.oci.region = "us-phoenix-1";
    cfg.oci.tenancy_id = "ocid1.tenancy.oc1..aaaa";
    cfg.oci.user_id = "ocid1.user.oc1..bbbb";
    cfg.oci.fingerprint = "aa:bb:cc:dd";
    cfg.oci.private_key_pem = shared_key_pem();
    cfg.oci.endpoint_override = mock.origin();
    cfg.oci.api_timeout = 5s;
    cfg.namespace_name = std::move(ns);
    return cfg;
}

auto oci_error_json(const std::string& code, const std::string& message) -> std::string {
    return "{\"code\":\"" + code + "\",\"message\":\"" + message + "\"}";
}

auto listing_json(const std::string& names_csv, const std::string& next_start = {}) -> std::string {
    std::string body = "{\"objects\":[";
    bool first = true;
    std::string name;
    for (std::size_t i = 0; i <= names_csv.size(); ++i) {
        if (i == names_csv.size() || names_csv[i] == ',') {
            if (!name.empty()) {
                if (!first) {
                    body += ",";
                }
                first = false;
                body += "{\"name\":\"" + name + "\"}";
                name.clear();
            }
            continue;
        }
        name.push_back(names_csv[i]);
    }
    body += "]";
    if (!next_start.empty()) {
        body += ",\"nextStartWith\":\"" + next_start + "\"";
    }
    body += "}";
    return body;
}

}  // namespace

// ── Concept obligations, including the negative one ──────────────────────────

BOOST_AUTO_TEST_SUITE(oci_object_storage_concepts)

// The negative is load-bearing: OCI's ETag is a UUID, so declaring
// `version_is_content_md5` would make the engine compare a UUID against an MD5
// and reject every write it makes.
BOOST_AUTO_TEST_CASE(the_store_concepts_this_client_satisfies) {
    static_assert(key_object_store<oci_object_storage_client>);
    static_assert(conditional_key_object_store<oci_object_storage_client>);
    static_assert(!content_md5_versioned_store<oci_object_storage_client>);
    BOOST_TEST(oci_object_storage_client::provider_name() == "oci-objectstorage");
}

BOOST_AUTO_TEST_SUITE_END()

// ── The signed content type, which is the reason task 10 touched oci_signing ─

BOOST_AUTO_TEST_SUITE(oci_object_storage_signing)

// The canonical signing string must actually change with the content type —
// otherwise the parameter is decoration and every object PUT signs
// `application/json` while sending `application/octet-stream`.
BOOST_AUTO_TEST_CASE(the_content_type_is_part_of_the_signing_string) {
    const std::string body = "raft-state";
    const std::string date = "Sun, 17 Aug 2026 12:00:00 GMT";

    const auto as_json = oci_signing::build_signing_string("PUT", "/n/ns/b/b/o/k", "host", body,
                                                           date, "application/json");
    const auto as_octets = oci_signing::build_signing_string("PUT", "/n/ns/b/b/o/k", "host", body,
                                                             date, "application/octet-stream");

    BOOST_TEST(as_json.find("content-type: application/json") != std::string::npos);
    BOOST_TEST(as_octets.find("content-type: application/octet-stream") != std::string::npos);
    BOOST_TEST(as_json != as_octets);

    // And the default is unchanged, so no existing control-plane caller signs
    // anything different from what it signed before this parameter existed.
    const auto defaulted =
        oci_signing::build_signing_string("PUT", "/n/ns/b/b/o/k", "host", body, date);
    BOOST_TEST(defaulted == as_json);
}

// A bodiless request signs no content type at all, so the parameter must not
// introduce one.
BOOST_AUTO_TEST_CASE(a_bodyless_request_signs_no_content_type) {
    const auto signing = oci_signing::build_signing_string("GET", "/n/ns/b/b/o/k", "host", "",
                                                           "Sun, 17 Aug 2026 12:00:00 GMT",
                                                           "application/octet-stream");
    BOOST_TEST(signing.find("content-type") == std::string::npos);
}

// ...and the octet-stream type reaches the wire, not just the signature.
BOOST_AUTO_TEST_CASE(an_object_put_sends_octet_stream_on_the_wire) {
    MockObjectStorage mock;
    mock.responders.push_back([](const recorded_request&, httplib::Response& res) {
        res.set_header("etag", "9a86a492-43fa-4c8c-a80e-dbd650188ff8");
        res.status = 200;
    });
    mock.start();

    const oci_object_storage_client client{config_for(mock)};
    client.put_object("kythira-ci-artifacts", "raft/term", "42");

    const auto reqs = mock.requests();
    BOOST_REQUIRE(reqs.size() == 1U);
    BOOST_TEST(reqs[0].header("Content-Type") == "application/octet-stream");
    BOOST_TEST(!reqs[0].header("authorization").empty());
    // The signed set names content-type, so the header on the wire and the one
    // in the signature are the same string or the service refuses the request.
    BOOST_TEST(reqs[0].header("authorization").find("content-type") != std::string::npos);
}

BOOST_AUTO_TEST_SUITE_END()

// ── Addressing and the namespace ─────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(oci_object_storage_addressing)

BOOST_AUTO_TEST_CASE(paths_carry_the_namespace_and_the_listing_path_has_no_trailing_slash) {
    MockObjectStorage mock;
    mock.start();
    const oci_object_storage_client client{config_for(mock)};

    BOOST_TEST(client.object_path("kythira-ci-artifacts", "raft/term") ==
               "/n/axunmw4f0mln/b/kythira-ci-artifacts/o/raft/term");
    // A different resource from the object path — `/o` lists, `/o/` would name
    // an object whose key is empty.
    BOOST_TEST(client.list_path("kythira-ci-artifacts") ==
               "/n/axunmw4f0mln/b/kythira-ci-artifacts/o");
}

// Task 0.8: the namespace is both configurable and resolvable, so a configured
// one wins outright and costs no request at all.
BOOST_AUTO_TEST_CASE(a_configured_namespace_issues_no_request) {
    MockObjectStorage mock;
    mock.start();

    const oci_object_storage_client client{config_for(mock)};
    BOOST_TEST(client.namespace_name() == "axunmw4f0mln");
    BOOST_TEST(mock.request_count() == 0U);
}

// ...and an unconfigured one is resolved once, at construction, never per
// request. `GET /n/` returns it as a bare JSON string.
BOOST_AUTO_TEST_CASE(an_unconfigured_namespace_is_resolved_once_at_construction) {
    MockObjectStorage mock;
    mock.responders.push_back([](const recorded_request&, httplib::Response& res) {
        res.set_content("\"axunmw4f0mln\"", "application/json");
    });
    mock.responders.push_back([](const recorded_request&, httplib::Response& res) {
        res.set_header("etag", "uuid");
        res.status = 200;
    });
    mock.start();

    const oci_object_storage_client client{config_for(mock, "")};
    BOOST_TEST(client.namespace_name() == "axunmw4f0mln");

    client.put_object("b", "k", "v");
    client.put_object("b", "k", "v");

    const auto reqs = mock.requests();
    BOOST_REQUIRE(reqs.size() == 3U);
    BOOST_TEST(reqs[0].path == "/n/");
    // Two writes, and no second namespace lookup between them.
    BOOST_TEST(reqs[1].path == "/n/axunmw4f0mln/b/b/o/k");
    BOOST_TEST(reqs[2].path == "/n/axunmw4f0mln/b/b/o/k");
}

BOOST_AUTO_TEST_CASE(a_namespace_that_is_not_a_bare_string_throws) {
    MockObjectStorage mock;
    mock.responders.push_back([](const recorded_request&, httplib::Response& res) {
        res.set_content("{\"namespace\":\"axunmw4f0mln\"}", "application/json");
    });
    mock.start();

    BOOST_CHECK_THROW(oci_object_storage_client{config_for(mock, "")}, std::runtime_error);
}

BOOST_AUTO_TEST_SUITE_END()

// ── Request shape ────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(oci_object_storage_request_shape)

// Requirement 7.1's service-side half: OCI verifies Content-MD5 and rejects a
// mismatch with 400 UnmatchedContentMD5. Known answer written out rather than
// recomputed by the code under test.
BOOST_AUTO_TEST_CASE(every_put_carries_content_md5_as_base64) {
    MockObjectStorage mock;
    mock.responders.push_back([](const recorded_request&, httplib::Response& res) {
        res.set_header("etag", "uuid");
        res.status = 200;
    });
    mock.start();

    const oci_object_storage_client client{config_for(mock)};
    client.put_object("b", "k", "hello");

    const auto reqs = mock.requests();
    BOOST_REQUIRE(reqs.size() == 1U);
    // md5("hello") = 5d41402abc4b2a76b9719d911017c592
    BOOST_TEST(reqs[0].header("Content-MD5") == "XUFAKrxLKna5cZ2REBfFkg==");
}

BOOST_AUTO_TEST_CASE(create_only_sends_if_none_match_star) {
    MockObjectStorage mock;
    mock.responders.push_back([](const recorded_request&, httplib::Response& res) {
        res.set_header("etag", "uuid-1");
        res.status = 200;
    });
    mock.start();

    const oci_object_storage_client client{config_for(mock)};
    const auto result = client.put_object_if("b", "k", "v", precondition{if_absent{}});

    const auto reqs = mock.requests();
    BOOST_REQUIRE(reqs.size() == 1U);
    BOOST_TEST(reqs[0].header("if-none-match") == "*");
    BOOST_TEST(!reqs[0].has_header("if-match"));
    BOOST_TEST(result.version == "uuid-1");
}

BOOST_AUTO_TEST_CASE(overwrite_precondition_sends_if_match) {
    MockObjectStorage mock;
    mock.responders.push_back([](const recorded_request&, httplib::Response& res) {
        res.set_header("etag", "uuid-2");
        res.status = 200;
    });
    mock.start();

    const oci_object_storage_client client{config_for(mock)};
    client.put_object_if("b", "k", "v", precondition{if_version{"uuid-1"}});

    const auto reqs = mock.requests();
    BOOST_REQUIRE(reqs.size() == 1U);
    BOOST_TEST(reqs[0].header("if-match") == "uuid-1");
    BOOST_TEST(!reqs[0].has_header("if-none-match"));
}

BOOST_AUTO_TEST_CASE(conditional_delete_sends_if_match) {
    MockObjectStorage mock;
    mock.responders.push_back(
        [](const recorded_request&, httplib::Response& res) { res.status = 204; });
    mock.start();

    const oci_object_storage_client client{config_for(mock)};
    client.delete_object_if("b", "k", precondition{if_version{"uuid-1"}});

    const auto reqs = mock.requests();
    BOOST_REQUIRE(reqs.size() == 1U);
    BOOST_TEST(reqs[0].method == "DELETE");
    BOOST_TEST(reqs[0].header("if-match") == "uuid-1");
}

// OCI spells the header lowercase. Task 0's probe looked up `ETag`, got an empty
// string, sent `if-match: ""` and read the resulting 412 as "OCI cannot do CAS".
// The raw path lowercases every response header on the way in so a lookup cannot
// miss by case — asserted here in both spellings, since a client that only ever
// meets one of them has not been tested for this.
BOOST_AUTO_TEST_CASE(the_etag_is_read_whatever_case_the_service_spells_it) {
    for (const auto* spelling : {"etag", "ETag", "ETAG"}) {
        MockObjectStorage mock;
        mock.responders.push_back([spelling](const recorded_request&, httplib::Response& res) {
            res.set_header(spelling, "9a86a492-43fa-4c8c-a80e-dbd650188ff8");
            res.status = 200;
        });
        mock.start();

        const oci_object_storage_client client{config_for(mock)};
        BOOST_TEST(client.put_object("b", "k", "v").version ==
                   "9a86a492-43fa-4c8c-a80e-dbd650188ff8");
    }
}

BOOST_AUTO_TEST_SUITE_END()

// ── Conditional verdicts ─────────────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(oci_object_storage_conditional_verdicts)

BOOST_AUTO_TEST_CASE(if_none_match_failed_is_a_precondition_failure) {
    MockObjectStorage mock;
    mock.responders.push_back([](const recorded_request&, httplib::Response& res) {
        res.status = 412;
        res.set_content(oci_error_json("IfNoneMatchFailed", "The If-None-Match header failed"),
                        "application/json");
    });
    mock.start();

    const oci_object_storage_client client{config_for(mock)};
    try {
        client.put_object_if("b", "raft/log/7", "v", precondition{if_absent{}});
        BOOST_FAIL("expected object_precondition_failed");
    } catch (const object_precondition_failed& e) {
        BOOST_TEST(e.provider() == "oci-objectstorage");
        BOOST_TEST(e.key() == "raft/log/7");
        BOOST_TEST(e.expected_version().empty());
        BOOST_TEST(std::string(e.what()).find("expected: absent") != std::string::npos);
    }
}

BOOST_AUTO_TEST_CASE(if_match_failed_carries_the_version_it_lost_on) {
    MockObjectStorage mock;
    mock.responders.push_back([](const recorded_request&, httplib::Response& res) {
        res.status = 412;
        res.set_content(oci_error_json("IfMatchFailed", "The If-Match header failed"),
                        "application/json");
    });
    mock.start();

    const oci_object_storage_client client{config_for(mock)};
    try {
        client.put_object_if("b", "raft/term", "v", precondition{if_version{"stale-uuid"}});
        BOOST_FAIL("expected object_precondition_failed");
    } catch (const object_precondition_failed& e) {
        BOOST_TEST(e.expected_version() == "stale-uuid");
    }
}

BOOST_AUTO_TEST_CASE(a_stale_conditional_delete_is_a_precondition_failure) {
    MockObjectStorage mock;
    mock.responders.push_back([](const recorded_request&, httplib::Response& res) {
        res.status = 412;
        res.set_content(oci_error_json("IfMatchFailed", "failed"), "application/json");
    });
    mock.start();

    const oci_object_storage_client client{config_for(mock)};
    BOOST_CHECK_THROW(client.delete_object_if("b", "k", precondition{if_version{"stale"}}),
                      object_precondition_failed);
}

// An unconditional PUT carries no precondition to fail. The engine's fence
// latches on `object_precondition_failed` alone, so this is the difference
// between a confusing error and a permanently wedged node.
BOOST_AUTO_TEST_CASE(an_unconditional_put_never_reports_a_precondition_failure) {
    MockObjectStorage mock;
    mock.responders.push_back([](const recorded_request&, httplib::Response& res) {
        res.status = 412;
        res.set_content(oci_error_json("IfMatchFailed", "failed"), "application/json");
    });
    mock.start();

    const oci_object_storage_client client{config_for(mock)};
    try {
        client.put_object("b", "k", "v");
        BOOST_FAIL("expected a throw");
    } catch (const object_precondition_failed&) {
        BOOST_FAIL("an unconditional PUT carries no precondition to fail");
    } catch (const std::runtime_error& e) {
        BOOST_TEST(std::string(e.what()).find("IfMatchFailed") != std::string::npos);
    }
}

// A checksum the service rejected is retryable, not a fence latch: re-sending
// identical bytes is what repairs a corrupted transfer.
BOOST_AUTO_TEST_CASE(a_rejected_digest_is_an_ordinary_error) {
    MockObjectStorage mock;
    mock.responders.push_back([](const recorded_request&, httplib::Response& res) {
        res.status = 400;
        res.set_content(oci_error_json("UnmatchedContentMD5", "The Content-MD5 did not match"),
                        "application/json");
    });
    mock.start();

    const oci_object_storage_client client{config_for(mock)};
    try {
        client.put_object_if("b", "k", "v", precondition{if_absent{}});
        BOOST_FAIL("expected a throw");
    } catch (const object_precondition_failed&) {
        BOOST_FAIL("UnmatchedContentMD5 must not be reported as a precondition failure");
    } catch (const std::runtime_error& e) {
        BOOST_TEST(std::string(e.what()).find("UnmatchedContentMD5") != std::string::npos);
    }
}

BOOST_AUTO_TEST_SUITE_END()

// ── Absence, and the 404 that is not absence ─────────────────────────────────

BOOST_AUTO_TEST_SUITE(oci_object_storage_absence)

BOOST_AUTO_TEST_CASE(object_not_found_is_nullopt_on_get_and_success_on_delete) {
    MockObjectStorage mock;
    mock.responders.push_back([](const recorded_request&, httplib::Response& res) {
        res.status = 404;
        res.set_content(oci_error_json("ObjectNotFound", "The object does not exist"),
                        "application/json");
    });
    mock.start();

    const oci_object_storage_client client{config_for(mock)};
    BOOST_TEST(!client.get_object("b", "absent").has_value());
    BOOST_CHECK_NO_THROW(client.delete_object("b", "absent"));
}

// The 404 that must never be read as absence. It is a real misconfiguration —
// and it is also the code this tenancy's intermittent authorization flake
// returns for perfectly valid requests, at 3-16%. Reading it as "the object is
// not there" would let a flake present as an empty Raft log, which is the one
// failure this engine cannot detect at runtime.
BOOST_AUTO_TEST_CASE(bucket_not_found_is_an_error_on_both_get_and_delete) {
    MockObjectStorage mock;
    mock.responders.push_back([](const recorded_request&, httplib::Response& res) {
        res.status = 404;
        res.set_content(
            oci_error_json("BucketNotFound",
                           "Either the bucket named 'b' does not exist in the namespace "
                           "'axunmw4f0mln' or you are not authorized to access it"),
            "application/json");
    });
    mock.start();

    const oci_object_storage_client client{config_for(mock)};
    try {
        (void)client.get_object("b", "k");
        BOOST_FAIL("expected a runtime_error for BucketNotFound");
    } catch (const std::runtime_error& e) {
        BOOST_TEST(std::string(e.what()).find("BucketNotFound") != std::string::npos);
    }
    BOOST_CHECK_THROW(client.delete_object("b", "k"), std::runtime_error);
}

BOOST_AUTO_TEST_SUITE_END()

// ── Listing ──────────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(oci_object_storage_listing)

BOOST_AUTO_TEST_CASE(a_single_page_listing_returns_every_key) {
    MockObjectStorage mock;
    mock.responders.push_back([](const recorded_request&, httplib::Response& res) {
        res.set_content(listing_json("raft/term,raft/log/001"), "application/json");
    });
    mock.start();

    const oci_object_storage_client client{config_for(mock)};
    const auto keys = client.list_keys("b", "raft/");
    BOOST_REQUIRE(keys.size() == 2U);
    BOOST_TEST(keys[0] == "raft/term");
    BOOST_TEST(keys[1] == "raft/log/001");

    const auto reqs = mock.requests();
    BOOST_REQUIRE(reqs.size() == 1U);
    BOOST_TEST(reqs[0].param("prefix") == "raft/");
    BOOST_TEST(reqs[0].param("limit") == "1000");
}

BOOST_AUTO_TEST_CASE(pagination_follows_next_start_with_to_exhaustion) {
    MockObjectStorage mock;
    mock.responders.push_back([](const recorded_request&, httplib::Response& res) {
        res.set_content(listing_json("a", "b"), "application/json");
    });
    mock.responders.push_back([](const recorded_request&, httplib::Response& res) {
        res.set_content(listing_json("b"), "application/json");
    });
    mock.start();

    const oci_object_storage_client client{config_for(mock)};
    const auto keys = client.list_keys("bucket", "");
    BOOST_REQUIRE(keys.size() == 2U);
    BOOST_TEST(keys[0] == "a");
    BOOST_TEST(keys[1] == "b");

    const auto reqs = mock.requests();
    BOOST_REQUIRE(reqs.size() == 2U);
    BOOST_TEST(reqs[0].param("start").empty());
    BOOST_TEST(reqs[1].param("start") == "b");
}

// "Zero objects" and "I could not read this" are otherwise the same answer, and
// to a persistence engine that answer is an empty log.
BOOST_AUTO_TEST_CASE(a_body_with_no_objects_array_throws) {
    MockObjectStorage mock;
    mock.responders.push_back([](const recorded_request&, httplib::Response& res) {
        res.set_content("{\"prefixes\":[]}", "application/json");
    });
    mock.start();

    const oci_object_storage_client client{config_for(mock)};
    BOOST_CHECK_THROW((void)client.list_keys("b", ""), std::runtime_error);
}

// `objects` present but not an array. Found by a surviving mutant: dropping the
// `is_array()` half of the guard still threw, because `get_array()` throws on
// the wrong kind — but with Boost.JSON's message instead of one naming the
// listing, so the caller loses which key it was reading. Asserting the message
// is what makes the guard testable rather than merely present.
BOOST_AUTO_TEST_CASE(an_objects_field_that_is_not_an_array_throws_naming_the_listing) {
    MockObjectStorage mock;
    mock.responders.push_back([](const recorded_request&, httplib::Response& res) {
        res.set_content("{\"objects\":{\"name\":\"a\"}}", "application/json");
    });
    mock.start();

    const oci_object_storage_client client{config_for(mock)};
    try {
        (void)client.list_keys("b", "raft/");
        BOOST_FAIL("expected a runtime_error");
    } catch (const std::exception& e) {
        const std::string what = e.what();
        BOOST_TEST(what.find("objects") != std::string::npos);
        BOOST_TEST(what.find("ListObjects") != std::string::npos);
    }
}

BOOST_AUTO_TEST_CASE(an_unparseable_listing_body_throws) {
    MockObjectStorage mock;
    mock.responders.push_back([](const recorded_request&, httplib::Response& res) {
        res.set_content("<html>a proxy answered instead</html>", "text/html");
    });
    mock.start();

    const oci_object_storage_client client{config_for(mock)};
    BOOST_CHECK_THROW((void)client.list_keys("b", ""), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(an_entry_without_a_name_throws) {
    MockObjectStorage mock;
    mock.responders.push_back([](const recorded_request&, httplib::Response& res) {
        res.set_content("{\"objects\":[{\"size\":3}]}", "application/json");
    });
    mock.start();

    const oci_object_storage_client client{config_for(mock)};
    BOOST_CHECK_THROW((void)client.list_keys("b", ""), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(an_empty_bucket_lists_as_no_keys) {
    MockObjectStorage mock;
    mock.responders.push_back([](const recorded_request&, httplib::Response& res) {
        res.set_content("{\"objects\":[]}", "application/json");
    });
    mock.start();

    const oci_object_storage_client client{config_for(mock)};
    BOOST_TEST(client.list_keys("b", "raft/").empty());
}

BOOST_AUTO_TEST_CASE(a_failed_page_throws_rather_than_truncating) {
    MockObjectStorage mock;
    mock.responders.push_back([](const recorded_request&, httplib::Response& res) {
        res.set_content(listing_json("a", "b"), "application/json");
    });
    mock.responders.push_back([](const recorded_request&, httplib::Response& res) {
        res.status = 500;
        res.set_content(oci_error_json("InternalServerError", "try again"), "application/json");
    });
    mock.start();

    const oci_object_storage_client client{config_for(mock)};
    BOOST_CHECK_THROW((void)client.list_keys("b", ""), std::runtime_error);
}

BOOST_AUTO_TEST_SUITE_END()

// ── Round trips ──────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(oci_object_storage_operations)

BOOST_AUTO_TEST_CASE(put_get_delete_round_trip) {
    MockObjectStorage mock;
    std::string stored;
    bool present = false;
    mock.responders.push_back([&](const recorded_request& req, httplib::Response& res) {
        if (req.method == "PUT") {
            stored = req.body;
            present = true;
            res.set_header("etag", "uuid-1");
            res.status = 200;
        } else if (req.method == "GET") {
            if (!present) {
                res.status = 404;
                res.set_content(oci_error_json("ObjectNotFound", "gone"), "application/json");
                return;
            }
            res.set_header("etag", "uuid-1");
            res.set_content(stored, "application/octet-stream");
        } else {
            present = false;
            res.status = 204;
        }
    });
    mock.start();

    const oci_object_storage_client client{config_for(mock)};
    BOOST_TEST(client.put_object("b", "raft/term", "42").version == "uuid-1");

    const auto got = client.get_object("b", "raft/term");
    BOOST_REQUIRE(got.has_value());
    BOOST_TEST(got->body == "42");
    BOOST_TEST(got->version == "uuid-1");

    client.delete_object("b", "raft/term");
    BOOST_TEST(!client.get_object("b", "raft/term").has_value());
}

BOOST_AUTO_TEST_CASE(binary_bytes_survive_a_round_trip) {
    MockObjectStorage mock;
    std::string stored;
    mock.responders.push_back([&](const recorded_request& req, httplib::Response& res) {
        if (req.method == "PUT") {
            stored = req.body;
            res.set_header("etag", "uuid");
            res.status = 200;
        } else {
            res.set_header("etag", "uuid");
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

    const oci_object_storage_client client{config_for(mock)};
    client.put_object("b", "blob", payload);
    const auto got = client.get_object("b", "blob");
    BOOST_REQUIRE(got.has_value());
    BOOST_TEST(got->body == payload);
}

BOOST_AUTO_TEST_SUITE_END()
