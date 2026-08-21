// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file accept_post_negotiation_test.cpp
/// @brief `Accept-Post` as an optimisation on top of the 415 retry
///        (W3C Linked Data Platform 1.0 §7.1), over cpp-httplib.
///
/// The 415 retry (`doc/TODO.md`, Requirement 7.3) is a **blind walk**: a client
/// whose first guess is refused works through the rest of its own preference
/// list, one round trip per guess. That is the mechanism, and it has to stay the
/// mechanism, because an unmodified single-serializer peer — the case the
/// requirement is about — tells us nothing about itself.
///
/// `Accept-Post` is the peer telling us anyway. A server that puts it on its 415
/// names the request media types it *would* have taken, and a client that reads
/// it converges on the second attempt instead of possibly the Nth.
///
/// **Three serializers, not two, and that is the whole design of this file.**
/// With two, the second is the only candidate left after the first rejection, so
/// an informed jump and a blind walk pick the same type and no assertion can
/// tell them apart — the tests would pass with the feature deleted. With
/// `{cbor, alt-json, json}` against a JSON-only peer, the blind walk *must*
/// spend a round trip on `alt-json` and the informed jump *must not*, so the
/// recorded retry sequence separates them outright.
///
/// The load-bearing assertions are therefore about the retry sequence and the
/// handler count, never about the RPC merely succeeding: a blind walk succeeds
/// too, just more slowly, which is exactly why this needs to measure rather than
/// check a return value.

#define BOOST_TEST_MODULE accept_post_negotiation_test
#include <boost/test/unit_test.hpp>

#include "negotiation_test_harness.hpp"

#include <raft/cbor_serializer.hpp>
#include <raft/http_content_negotiation.hpp>
#include <raft/json_serializer.hpp>
#include <raft/serializer_registry.hpp>

#include <httplib.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

using namespace kythira::negotiation_test;

using data_type = std::vector<std::byte>;
using json_serializer = kythira::json_rpc_serializer<data_type>;
using cbor_serializer = kythira::cbor_rpc_serializer<data_type>;

/// A serializer byte-identical to JSON that announces a different media type.
///
/// A third *label* is all this needs — the thing under test is which media type
/// the client picks for its retry, and it never gets as far as encoding in this
/// one on the path that matters. A third real codec would tie the file to
/// whichever optional serializer happened to be installed on the build leg,
/// which is a dependency this file has no reason to acquire.
struct alt_json_serializer : json_serializer {
    [[nodiscard]] auto media_type() const -> std::string { return "application/x-alt-json"; }
    [[nodiscard]] auto name() const -> std::string { return "alt-json"; }
};

constexpr const char* alt_media = "application/x-alt-json";

// Fresh ports: the other negotiation suites hold 18230-18239 and 18295-18296,
// and may run concurrently under `ctest -j`.
constexpr std::uint16_t advertise_port = 18310;
constexpr std::uint16_t informed_retry_port = 18311;
constexpr std::uint16_t single_serializer_port = 18312;
constexpr std::uint16_t flip_port = 18313;
constexpr std::uint16_t asymmetric_port = 18314;

// Spelled locally, as `negotiation_failure_test` does for the same reason: the
// transport's own copies live in an anonymous namespace inside `kythira`, so
// they are per-translation-unit and not this file's to reach into.
constexpr const char* endpoint_request_vote = "/v1/raft/request_vote";
constexpr const char* header_content_type = "Content-Type";

using single_json = kythira::single_serializer_registry<json_serializer>;
/// Preference order `{cbor, alt-json, json}`: cbor is the default and therefore
/// the first guess, `alt-json` is what a blind walk tries next, and `json` is
/// what the peer will actually name.
using multi_three =
    kythira::multi_serializer_registry<cbor_serializer, alt_json_serializer, json_serializer>;

using single_json_types = negotiating_transport_types<json_serializer, single_json>;
using multi_three_types = negotiating_transport_types<cbor_serializer, multi_three>;
using single_cbor_types =
    negotiating_transport_types<cbor_serializer,
                                kythira::single_serializer_registry<cbor_serializer>>;

/// @brief A peer that decodes exactly one request media type at a time, and can
///        be told to change its mind between RPCs.
///
/// **A hand-rolled peer rather than `cpp_httplib_server`, which is the only way
/// to ask this question at all.** A real server derives what it decodes from its
/// registry *type*, so its decodable set is fixed at compile time. Every suite
/// in the tree therefore drives a peer that never contradicts what was cached
/// about it — which leaves the claim in `peer_capability_cache.hpp` that
/// justifies the cache having no TTL and no invalidation path entirely
/// unexercised.
///
/// Answers in the same media type it accepted unless @p answers_in says
/// otherwise. Symmetric is the default because it isolates "the cache was
/// contradicted" from the separate question of a peer whose request and response
/// types differ, which would move two variables at once. The asymmetric case is
/// its own test below, and the one place this parameter is used.
class flipping_peer {
public:
    explicit flipping_peer(std::string accepted, std::string answers_in = {})
        : _accepted(std::move(accepted)), _answers_in(std::move(answers_in)) {
        _server.Post(endpoint_request_vote, [this](const httplib::Request& req,
                                                   httplib::Response& resp) { handle(req, resp); });
    }

    /// Change what the peer decodes. The RPCs below are sequential, so this only
    /// ever runs between them.
    auto accept_only(std::string media_type) -> void {
        const std::lock_guard<std::mutex> lock(_mutex);
        _accepted = std::move(media_type);
    }

    auto start(std::uint16_t port) -> void {
        _thread = std::thread([this, port] { _server.listen(bind_address, port); });
        _server.wait_until_ready();
    }

    auto stop() -> void {
        _server.stop();
        if (_thread.joinable()) {
            _thread.join();
        }
    }

    /// Requests that got as far as being decoded — this peer's equivalent of the
    /// handler count, so "it round-tripped" can be told from "it 415'd".
    [[nodiscard]] auto accepted_requests() const -> int { return _accepted_requests.load(); }

    /// Every `Content-Type` this peer was sent, in order, 415s included. The
    /// client-side metric records the same sequence, so a disagreement between
    /// the two is visible rather than averaged away.
    [[nodiscard]] auto received_content_types() const -> std::vector<std::string> {
        const std::lock_guard<std::mutex> lock(_mutex);
        return _received;
    }

private:
    auto handle(const httplib::Request& req, httplib::Response& resp) -> void {
        std::string content_type;
        if (req.has_header(header_content_type)) {
            content_type =
                kythira::strip_media_type_parameters(req.get_header_value(header_content_type));
        }

        std::string accepted;
        {
            const std::lock_guard<std::mutex> lock(_mutex);
            _received.push_back(content_type);
            accepted = _accepted;
        }

        if (content_type != accepted) {
            // Byte-for-byte the 415 `cpp_httplib_server` emits. A client reading
            // a differently-shaped rejection here would prove nothing about the
            // real pairing.
            resp.status = 415;
            resp.body = "Unsupported Content-Type: " + content_type;
            resp.set_header(header_content_type, "text/plain");
            resp.set_header(kythira::header_accept_post, accepted);
            return;
        }

        _accepted_requests.fetch_add(1);

        kythira::request_vote_response<> response;
        response._term = 6;
        response._vote_granted = true;

        // What this peer answers in, which is not necessarily what it accepts.
        const std::string answer = _answers_in.empty() ? accepted : _answers_in;

        // Encoded through the same serializers the client holds, so a decode
        // failure would be a real incompatibility rather than an artefact of a
        // hand-written body.
        data_type encoded = answer == cbor_media ? cbor_serializer{}.serialize(response)
                                                 : json_serializer{}.serialize(response);

        std::string body;
        body.reserve(encoded.size());
        for (auto b : encoded) {
            body.push_back(static_cast<char>(b));
        }
        resp.status = 200;
        resp.set_content(body, answer.c_str());
    }

    httplib::Server _server;
    std::thread _thread;
    mutable std::mutex _mutex;
    std::string _accepted;
    /// Empty means "answer in whatever was accepted"; see the class comment.
    std::string _answers_in;
    std::vector<std::string> _received;
    std::atomic<int> _accepted_requests{0};
};

}  // namespace

BOOST_AUTO_TEST_SUITE(accept_post_negotiation_tests)

/// The server half, checked on the wire rather than through our own client: a
/// raw `httplib::Client` sends a `Content-Type` the server cannot decode and
/// reads the response headers directly.
///
/// A foreign client is the only honest way to ask this. Going through
/// `cpp_httplib_client` would prove at most that our client and our server agree
/// with each other, which is not what a header on the wire is for.
BOOST_AUTO_TEST_CASE(a_server_names_what_it_would_have_taken_on_a_415) {
    recording_metrics server_metrics;
    kythira::cpp_httplib_server<single_json_types> server(bind_address, advertise_port, {},
                                                          server_metrics);
    int invocations = 0;
    server.register_request_vote_handler([&](const kythira::request_vote_request<>&) {
        ++invocations;
        return kythira::request_vote_response<>{};
    });
    server.start();
    std::this_thread::sleep_for(std::chrono::milliseconds{150});

    httplib::Client raw(bind_address, advertise_port);
    raw.set_connection_timeout(5, 0);
    auto resp = raw.Post("/v1/raft/request_vote", "{}", alt_media);
    BOOST_REQUIRE_MESSAGE(resp, "the request itself failed");
    BOOST_TEST(resp->status == 415);
    // The header is present and says exactly what this server can decode.
    BOOST_TEST(resp->has_header(kythira::header_accept_post));
    BOOST_CHECK_EQUAL(resp->get_header_value(kythira::header_accept_post), "application/json");
    // Still answered before the handler ran, which is what makes retrying safe
    // in the first place. Advertising must not have moved the rejection later.
    BOOST_TEST(invocations == 0);

    server.stop();
}

/// A 415 is the only status this belongs on. A 404 carries no statement about
/// media types, and a client that read one there would be acting on a header
/// that answered a different question.
BOOST_AUTO_TEST_CASE(a_server_does_not_advertise_on_an_unrelated_error) {
    recording_metrics server_metrics;
    kythira::cpp_httplib_server<single_json_types> server(bind_address, advertise_port, {},
                                                          server_metrics);
    server.register_request_vote_handler(
        [](const kythira::request_vote_request<>&) { return kythira::request_vote_response<>{}; });
    server.start();
    std::this_thread::sleep_for(std::chrono::milliseconds{150});

    httplib::Client raw(bind_address, advertise_port);
    raw.set_connection_timeout(5, 0);
    auto resp = raw.Post("/v1/raft/no_such_endpoint", "{}", "application/json");
    BOOST_REQUIRE_MESSAGE(resp, "the request itself failed");
    BOOST_TEST(resp->status == 404);
    BOOST_TEST(!resp->has_header(kythira::header_accept_post));

    server.stop();
}

/// The client half, and the case the whole feature exists for.
///
/// `{cbor, alt-json, json}` client against a JSON-only server. cbor is refused;
/// the peer's 415 names `application/json`; the retry goes straight there.
/// **Exactly one retry, naming json** is the assertion — a blind walk would
/// record two, the first of them `alt-json`, and would otherwise look identical
/// from the outside.
BOOST_AUTO_TEST_CASE(a_client_retries_straight_to_the_type_the_peer_named) {
    const auto obs = run_exchange<multi_three_types, single_json_types>(informed_retry_port);

    BOOST_TEST(obs.failed_rpcs == 0, "RPC failed: " << obs.first_error);
    BOOST_TEST(obs.round_trips_correct);
    // Three RPCs, three handler entries: the 415s cost nothing on the server and
    // nothing was applied twice.
    BOOST_TEST(obs.handler_invocations == 3);

    BOOST_REQUIRE_EQUAL(obs.retry_media_types.size(), 1U);
    BOOST_CHECK_EQUAL(obs.retry_media_types.front(), json_media);
    // Stated as its own check because it is the specific waste being removed:
    // `alt-json` is what the blind walk would have burned a round trip on.
    const auto tried_alt = std::find(obs.retry_media_types.begin(), obs.retry_media_types.end(),
                                     alt_media) != obs.retry_media_types.end();
    BOOST_CHECK_MESSAGE(
        !tried_alt, "the retry walked through " << alt_media << " instead of reading Accept-Post");

    // The whole wire sequence, since `request.sent` is emitted per *attempt*
    // rather than per RPC: cbor is refused, the retry sends json, and RPCs 2 and
    // 3 open with json because the capability cache recorded it after the first
    // success. Four sends for three RPCs. A blind walk would make it five, with
    // alt-json second — so this pins the saving as a count, not just as an
    // absence.
    BOOST_REQUIRE_EQUAL(obs.client_request_types.size(), 4U);
    BOOST_CHECK_EQUAL(obs.client_request_types[0], cbor_media);
    BOOST_CHECK_EQUAL(obs.client_request_types[1], json_media);
    BOOST_CHECK_EQUAL(obs.client_request_types[2], json_media);
    BOOST_CHECK_EQUAL(obs.client_request_types[3], json_media);
}

/// Every configuration shipping today has one serializer, so this is the path
/// that must not change. The peer advertises, and it still cannot help: the one
/// type we have is the one just refused, so the 415 surfaces as the error it is
/// rather than being retried or turned into a broken promise.
BOOST_AUTO_TEST_CASE(a_single_serializer_client_still_fails_cleanly_when_advertised_at) {
    const auto obs = run_exchange<single_cbor_types, single_json_types>(single_serializer_port);

    BOOST_TEST(obs.failed_rpcs == 3);
    BOOST_TEST(obs.handler_invocations == 0);
    BOOST_TEST(obs.retry_media_types.empty());
    BOOST_TEST(obs.first_error.find("415") != std::string::npos,
               "expected the 415 itself, got: " << obs.first_error);
}

/// The case no other suite reaches: a peer that accepts a media type, is cached
/// as accepting it, and then stops.
///
/// `peer_capability_cache.hpp` justifies having no TTL and no invalidation path
/// with a specific claim — that a stale entry "costs at most one extra round of
/// the peer choosing again" and "can never make a request fail", because "a peer
/// that changes its formats has its entry corrected by its next successful
/// response". Every other negotiation suite drives a peer whose decodable set is
/// fixed for the life of the test, so the cache is written once and never
/// contradicted, and neither half of that claim is exercised anywhere.
///
/// Three `request_vote` calls rather than one of each RPC, which is the opposite
/// of what `negotiation_test_harness` does and deliberate. The harness spreads
/// across all three endpoints because *server dispatch* is what it exercises,
/// and negotiation wired into one endpoint and not the others would hide there.
/// Here the subject is the client's per-peer cache, which is keyed by node id
/// and shared across endpoints, so repeating one endpoint is what makes the flip
/// legible: each call differs from the last only in what the peer will now take.
///
/// **The load-bearing assertion is a count.** A client that ignored the cache, a
/// client that corrected it, and a client that never corrected it all complete
/// all three RPCs; they differ only in how many attempts that takes. Five
/// attempts means the flip was paid for once, six means it is paid on every
/// subsequent call forever. Asserting that the RPCs succeeded would pass under
/// all three.
BOOST_AUTO_TEST_CASE(a_cached_media_type_is_corrected_when_the_peer_stops_taking_it) {
    flipping_peer peer{json_media};
    peer.start(flip_port);

    recording_metrics client_metrics;
    int failures = 0;
    std::string first_error;

    {
        kythira::cpp_httplib_client_config client_config;
        client_config.connection_timeout = std::chrono::milliseconds{2000};
        client_config.request_timeout = std::chrono::milliseconds{4000};

        std::unordered_map<std::uint64_t, std::string> node_urls;
        node_urls[peer_node_id] =
            std::string("http://") + bind_address + ":" + std::to_string(flip_port);

        kythira::cpp_httplib_client<multi_three_types> client(std::move(node_urls), client_config,
                                                              client_metrics);

        const auto call = [&](std::uint64_t term) {
            try {
                kythira::request_vote_request<> req;
                req._term = term;
                req._candidate_id = 42;
                req._last_log_index = 10;
                req._last_log_term = 4;
                auto resp =
                    client.send_request_vote(peer_node_id, req, std::chrono::milliseconds{3000})
                        .get();
                BOOST_TEST(resp.vote_granted());
            } catch (const std::exception& e) {
                ++failures;
                if (first_error.empty()) {
                    first_error = e.what();
                }
            }
        };

        // 1. Cache empty: the default (cbor) is refused, `Accept-Post` names
        //    json, the retry lands, and json is what gets cached.
        call(5);
        // The peer changes its mind. Nothing tells the client.
        peer.accept_only(cbor_media);
        // 2. The cache is now wrong. This is the request the claim is about.
        call(6);
        // 3. And this is the one that separates "corrected" from "survived".
        call(7);
    }

    peer.stop();

    const auto sent = client_metrics.recorder()->media_types_of("http.client.request.sent");
    const auto retries = client_metrics.recorder()->media_types_of("http.client.media_type_retry");

    // The half of the claim that would be a correctness bug rather than a cost.
    BOOST_TEST(failures == 0, "an RPC failed across the flip: " << first_error);
    BOOST_TEST(peer.accepted_requests() == 3);

    BOOST_REQUIRE_EQUAL(sent.size(), 5U);
    BOOST_CHECK_EQUAL(sent[0], cbor_media);  // default guess
    BOOST_CHECK_EQUAL(sent[1], json_media);  // Accept-Post-informed retry
    BOOST_CHECK_EQUAL(sent[2], json_media);  // cached, and now wrong
    BOOST_CHECK_EQUAL(sent[3], cbor_media);  // re-negotiated after the flip
    BOOST_CHECK_EQUAL(sent[4], cbor_media);  // corrected: the flip is paid once

    // Both retries read the header rather than falling back to the blind walk.
    // Stated separately from the sequence above because it is a different
    // property: the sequence would still be five long if the second jump had
    // guessed right by luck, since alt-json is not what the peer now wants.
    BOOST_REQUIRE_EQUAL(retries.size(), 2U);
    BOOST_CHECK_EQUAL(retries[0], json_media);
    BOOST_CHECK_EQUAL(retries[1], cbor_media);
    // Reduced to a bool first: BOOST_TEST would try to print the iterator pair.
    const auto spent_on_alt = std::find(sent.begin(), sent.end(), alt_media) != sent.end();
    BOOST_CHECK_MESSAGE(!spent_on_alt, "an attempt was spent on " << alt_media);

    // The peer's own record of what arrived, as a cross-check on the metric. If
    // these ever disagree, the metric is measuring something other than what
    // went on the wire, and every conclusion above drawn from it is void.
    const auto received = peer.received_content_types();
    BOOST_REQUIRE_EQUAL(received.size(), sent.size());
    for (std::size_t i = 0; i < received.size(); ++i) {
        BOOST_CHECK_EQUAL(received[i], sent[i]);
    }
}

/// A peer whose request and response media types differ — the case the cache's
/// own header comment promises cannot cost more than one extra round trip.
///
/// `peer_capability_cache.hpp` says a stale entry "costs at most one extra round
/// of the peer choosing again". That holds only while "the type this peer
/// answered in" and "the type this peer accepts" are the same thing. They
/// coincide for our own server, which answers in whatever its `Accept`-driven
/// choice picked from a set it also decodes, and they need not coincide for a
/// foreign one: nothing in HTTP, and nothing in Requirement 6, obliges a server
/// to accept the media type it replies in.
///
/// This peer decodes **cbor only** and answers in **json** every time. Both are
/// types the client holds, so nothing here fails; the whole defect is a cost.
/// Under a cache keyed on the *response* type the client relearns the same
/// lesson on every single call — send json, take a 415, retry cbor — and the
/// "at most one extra round" claim is false by an unbounded margin rather than
/// off by one.
///
/// **Counted, not merely succeeded**, for the same reason as the flip test
/// above: a client that never converges and a client that converges immediately
/// both complete all three RPCs. Three attempts means the cache learned what the
/// peer accepts; five means it is paying the retry forever.
BOOST_AUTO_TEST_CASE(a_peer_that_answers_in_a_type_it_will_not_accept_converges) {
    // Accepts cbor, answers json. The client's default guess is cbor, so the
    // *first* call succeeds outright and the cache is written from a success —
    // there is no rejection anywhere in this test to blame the cost on.
    flipping_peer peer{cbor_media, json_media};
    peer.start(asymmetric_port);

    recording_metrics client_metrics;
    int failures = 0;
    std::string first_error;

    {
        kythira::cpp_httplib_client_config client_config;
        client_config.connection_timeout = std::chrono::milliseconds{2000};
        client_config.request_timeout = std::chrono::milliseconds{4000};

        std::unordered_map<std::uint64_t, std::string> node_urls;
        node_urls[peer_node_id] =
            std::string("http://") + bind_address + ":" + std::to_string(asymmetric_port);

        kythira::cpp_httplib_client<multi_three_types> client(std::move(node_urls), client_config,
                                                              client_metrics);

        for (std::uint64_t term = 5; term <= 7; ++term) {
            try {
                kythira::request_vote_request<> req;
                req._term = term;
                req._candidate_id = 42;
                req._last_log_index = 10;
                req._last_log_term = 4;
                auto resp =
                    client.send_request_vote(peer_node_id, req, std::chrono::milliseconds{3000})
                        .get();
                BOOST_TEST(resp.vote_granted());
            } catch (const std::exception& e) {
                ++failures;
                if (first_error.empty()) {
                    first_error = e.what();
                }
            }
        }
    }

    peer.stop();

    const auto sent = client_metrics.recorder()->media_types_of("http.client.request.sent");
    const auto retries = client_metrics.recorder()->media_types_of("http.client.media_type_retry");

    BOOST_TEST(failures == 0, "an RPC failed against the asymmetric peer: " << first_error);
    BOOST_TEST(peer.accepted_requests() == 3);

    // The assertion the whole case exists for. Five sends is the defect: json on
    // calls 2 and 3 because that is what the peer *answered* in, each refused,
    // each retried back to cbor.
    BOOST_REQUIRE_EQUAL(sent.size(), 3U);
    BOOST_CHECK_EQUAL(sent[0], cbor_media);  // default guess, and accepted
    BOOST_CHECK_EQUAL(sent[1], cbor_media);  // cached from what the peer took
    BOOST_CHECK_EQUAL(sent[2], cbor_media);
    // Stated separately: a peer that never refuses anything must never provoke a
    // retry. This is what distinguishes "converged" from "guessed right twice".
    BOOST_TEST(retries.empty());

    // Cross-check against the peer's own record, as above: if the metric and the
    // wire disagree, every count drawn from the metric is void.
    const auto received = peer.received_content_types();
    BOOST_REQUIRE_EQUAL(received.size(), sent.size());
    for (std::size_t i = 0; i < received.size(); ++i) {
        BOOST_CHECK_EQUAL(received[i], sent[i]);
    }
}

BOOST_AUTO_TEST_SUITE_END()
