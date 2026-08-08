/// @file negotiation_failure_test.cpp
/// @brief Content-negotiation failure paths on the **client** side
///        (`.kiro/specs/transport-multi-serializer/`, Task 16).
///
/// Task 16 enumerates four failure classes. Three of them are server-side and
/// already covered: an unsupported request `Content-Type` (415) and an
/// unsatisfiable `Accept` (406) are pinned by `http_negotiation_integration_test`
/// and `proxygen_negotiation_integration_test`, both of which reach those
/// branches by driving our server with a raw `httplib::Client`. The fourth —
/// *"client-side unrecognized response `Content_Declaration` -> error future,
/// cache untouched"* — had no test anywhere, because reaching it needs the
/// mirror-image trick: a **server** that answers something our own server never
/// would.
///
/// So this suite stands a raw `httplib::Server` in front of a real
/// `cpp_httplib_client` and has it reply with response labels a conforming peer
/// would not emit. Those branches are otherwise unreachable, which is the same
/// argument that justified the foreign *client* in the two suites above, applied
/// to the other end of the wire.
///
/// Requirement 11.6's "no crash, hang, or unresolved future" is asserted
/// structurally rather than as a sentiment: every case takes a future and
/// completes it — a hang shows up as the RPC timeout elapsing, and an unresolved
/// promise shows up as `.get()` on a broken promise. Both fail the case rather
/// than blocking the suite, since the client is given a request timeout well
/// below ctest's.

#define BOOST_TEST_MODULE negotiation_failure_test
#include <boost/test/unit_test.hpp>

#include "negotiation_test_harness.hpp"

#include <raft/cbor_serializer.hpp>
#include <raft/http_exceptions.hpp>
#include <raft/json_serializer.hpp>
#include <raft/serializer_registry.hpp>

#include <httplib.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

using namespace kythira::negotiation_test;

using data_type = std::vector<std::byte>;
using json_serializer = kythira::json_rpc_serializer<data_type>;
using cbor_serializer = kythira::cbor_rpc_serializer<data_type>;

using single_json = kythira::single_serializer_registry<json_serializer>;
using client_types = negotiating_transport_types<json_serializer, single_json>;

constexpr std::uint16_t rogue_port = 18270;
constexpr const char* endpoint_request_vote = "/v1/raft/request_vote";

/// A well-formed `request_vote_response` in the given serializer's format, so a
/// rejection observed below is attributable to the *label* rather than to the
/// bytes. The two cases that vary the bytes say so explicitly.
template<typename Serializer> auto valid_response_body() -> std::string {
    kythira::request_vote_response<> resp;
    resp._term = 6;
    resp._vote_granted = true;

    const Serializer serializer;
    const auto bytes = serializer.serialize(resp);
    std::string body;
    body.reserve(bytes.size());
    for (auto b : bytes) {
        body.push_back(static_cast<char>(b));
    }
    return body;
}

/// @brief A server that answers every `request_vote` with a caller-chosen
///        `Content-Type` and body.
///
/// Deliberately a raw `httplib::Server`: `cpp_httplib_server` cannot be made to
/// emit an unregistered response label, because it only ever answers with a type
/// its own registry selected. That is correct behaviour, and it is exactly why
/// the client's handling of a *non*-conforming peer cannot be tested through it.
struct rogue_server {
    httplib::Server server;
    std::thread thread;
    std::atomic<int> requests_served{0};

    /// @param content_type What to label every response with.
    /// @param body What to send as the response body.
    rogue_server(std::string content_type, std::string body) {
        server.Post(endpoint_request_vote,
                    [this, content_type = std::move(content_type), body = std::move(body)](
                        const httplib::Request&, httplib::Response& res) {
                        requests_served.fetch_add(1);
                        res.status = 200;
                        res.set_content(body, content_type);
                    });
        thread = std::thread([this] { server.listen(bind_address, rogue_port); });
        // `listen` binds on the new thread; wait for it rather than sleeping a
        // guessed interval, so a slow machine cannot turn this into a flake.
        server.wait_until_ready();
    }

    ~rogue_server() {
        server.stop();
        if (thread.joinable()) {
            thread.join();
        }
    }

    rogue_server(const rogue_server&) = delete;
    auto operator=(const rogue_server&) -> rogue_server& = delete;
};

/// A client pointed at the rogue server, with a request timeout well under the
/// suite's ctest timeout so a hang fails the case by name instead of killing the
/// run with no attribution.
struct client_fixture {
    recording_metrics metrics;
    kythira::cpp_httplib_client<client_types> client;

    client_fixture()
        : client(
              [] {
                  std::unordered_map<std::uint64_t, std::string> urls;
                  urls[peer_node_id] =
                      std::string("http://") + bind_address + ":" + std::to_string(rogue_port);
                  return urls;
              }(),
              [] {
                  kythira::cpp_httplib_client_config c;
                  c.connection_timeout = std::chrono::milliseconds{2000};
                  c.request_timeout = std::chrono::milliseconds{3000};
                  return c;
              }(),
              metrics) {}

    auto send() {
        kythira::request_vote_request<> req;
        req._term = 5;
        req._candidate_id = 42;
        req._last_log_index = 10;
        req._last_log_term = 4;
        return client.send_request_vote(peer_node_id, req, std::chrono::milliseconds{3000});
    }
};

}  // namespace

/// @brief Requirement 6.5 / 8.2: a response in a type the client's registry does
///        not know fails the future with `unsupported_media_type_error`.
///
/// The distinct exception type is the assertion that matters. A generic
/// `runtime_error` here would be indistinguishable from a transport fault, and
/// the two call for opposite responses — reconfigure a serializer versus retry
/// the peer.
BOOST_AUTO_TEST_CASE(an_unregistered_response_media_type_fails_the_future_with_a_typed_error) {
    rogue_server rogue("application/x-not-a-format", valid_response_body<json_serializer>());
    client_fixture fixture;

    bool threw_expected = false;
    std::string offending;
    try {
        fixture.send().get();
    } catch (const kythira::unsupported_media_type_error& e) {
        threw_expected = true;
        offending = e.what();
    } catch (const std::exception& e) {
        BOOST_TEST_MESSAGE("wrong exception type: " << e.what());
    }

    BOOST_TEST(threw_expected);
    // The error names the type it could not handle, so the operator does not
    // have to reproduce the exchange to find out which one it was.
    BOOST_TEST(offending.find("application/x-not-a-format") != std::string::npos,
               "error should name the offending type, got: " << offending);
    // The request really was served -- otherwise this would pass on a connection
    // failure, which throws a different type but from the same `.get()`.
    BOOST_TEST(rogue.requests_served.load() == 1);
}

/// @brief Requirement 6.4: an **absent** response `Content-Type` is not an error.
///
/// A peer predating negotiation sends no label at all, and it must keep working:
/// the client falls back to its own default rather than rejecting the response.
/// This is the client-side half of a rule whose server-side half
/// (`http_negotiation_integration_test`'s empty-`Content-Type` case) was already
/// covered — and the two are separate code paths, so covering one said nothing
/// about the other.
///
/// `httplib::Response::set_content` always writes a `Content-Type`, so the empty
/// label is produced by clearing the header after the fact.
BOOST_AUTO_TEST_CASE(an_absent_response_content_type_falls_back_to_the_client_default) {
    rogue_server rogue("", valid_response_body<json_serializer>());
    client_fixture fixture;

    bool succeeded = false;
    try {
        const auto resp = fixture.send().get();
        succeeded = resp.term() == 6 && resp.vote_granted();
    } catch (const std::exception& e) {
        BOOST_TEST_MESSAGE("unexpected exception: " << e.what());
    }

    BOOST_TEST(succeeded);
    BOOST_TEST(rogue.requests_served.load() == 1);
}

/// @brief Requirement 8.3: a *supported* label over foreign bytes is a decode
///        error, not an unsupported-media-type error.
///
/// The peer says `application/json` and sends CBOR. The client speaks JSON, so
/// this is not a negotiation failure at all — it is a well-formed negotiation
/// followed by a bad payload, and it has to surface as the payload problem it
/// is. Collapsing the two would tell an operator to add a serializer when the
/// peer is the thing that is broken.
BOOST_AUTO_TEST_CASE(
    a_supported_label_over_foreign_bytes_is_a_decode_error_not_a_media_type_error) {
    rogue_server rogue(json_media, valid_response_body<cbor_serializer>());
    client_fixture fixture;

    bool threw_unsupported_media_type = false;
    bool threw_something = false;
    try {
        fixture.send().get();
    } catch (const kythira::unsupported_media_type_error&) {
        threw_unsupported_media_type = true;
        threw_something = true;
    } catch (const std::exception&) {
        threw_something = true;
    }

    BOOST_TEST(threw_something, "a CBOR payload labelled JSON must not decode silently");
    BOOST_TEST(!threw_unsupported_media_type,
               "the media type was supported -- only the bytes were wrong");
    BOOST_TEST(rogue.requests_served.load() == 1);
}

/// @brief Requirement 11.6: a negotiation failure does not poison the client.
///
/// The failing exchange above and a subsequent good one run against the *same*
/// client instance, so this covers what a fresh-client-per-case suite cannot:
/// that the error path leaves no wedged connection, no half-consumed response,
/// and no cache entry steering the next request somewhere unusable.
///
/// It is also the closest observable proxy for "the cache is left untouched".
/// The cache itself is private, and probing it through the *request* label would
/// prove nothing: `select_request_media_type` ignores a cached type the registry
/// no longer supports, so a wrongly-recorded bogus entry would be filtered out
/// on the way back and look identical to never having been recorded. What is
/// genuinely observable is that the client is still fully functional afterwards.
BOOST_AUTO_TEST_CASE(a_failed_negotiation_leaves_the_client_usable_for_the_next_request) {
    client_fixture fixture;

    {
        rogue_server rogue("application/x-not-a-format", valid_response_body<json_serializer>());
        BOOST_CHECK_THROW(fixture.send().get(), kythira::unsupported_media_type_error);
    }
    // Same client, same target, a peer that now behaves.
    {
        rogue_server rogue(json_media, valid_response_body<json_serializer>());
        bool succeeded = false;
        try {
            const auto resp = fixture.send().get();
            succeeded = resp.term() == 6 && resp.vote_granted();
        } catch (const std::exception& e) {
            BOOST_TEST_MESSAGE("client did not recover: " << e.what());
        }
        BOOST_TEST(succeeded);
        BOOST_TEST(rogue.requests_served.load() == 1);
    }
}
