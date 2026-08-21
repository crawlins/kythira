// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// Measures what the media-type negotiation retry actually costs on the wire,
// and what `Accept-Post` saves -- the number doc/TODO.md's own entries
// ("Accept-Post layered on top of the 415 retry") recorded as **not
// measured** when the feature landed.
//
// Four arms, one variable apart, all against the same raw single-JSON
// server (an httplib::Server returning canned responses -- raw so the
// blind-walk arm can exist at all: the shipped servers always advertise
// `Accept-Post` on 415, and an absent header IS the shipped client's
// blind-walk path, so both arms exercise production client code against
// byte-identical server behaviour except for that one header):
//
//   matched            client's first preference is JSON: 1 round trip.
//   informed (2 RT)    client prefers {cbor, alt-json, json}; the 415
//                      carries `Accept-Post: application/json`, the client
//                      jumps straight to JSON: 2 round trips.
//   blind (3 RT)       same client, 415 carries no header, the client
//                      walks cbor -> alt-json -> json: 3 round trips.
//   renegotiated       the informed client's SECOND call on the same
//                      client instance -- the peer_capability_cache
//                      remembers the format that worked, so this should
//                      match `matched`. This is the amortisation claim
//                      ("the retry costs once per peer, not per call")
//                      measured rather than asserted.
//
// Every first-call sample uses a fresh boost_beast_client (the capability
// cache is per-client state and would otherwise erase the very thing being
// measured after sample one), so each sample includes one TCP connection
// establishment -- identically in every arm, which is why the *differences*
// between arms are meaningful while the absolute numbers include a
// connect's worth of floor. The Beast transport is the measured client for
// the same reason it is the fast row in
// doc/http_transport_performance_comparison.md: its per-round-trip cost is
// low enough (~130us loopback) that one saved round trip is visible rather
// than lost in an 80ms Nagle stall (cpp-httplib's default posture, same
// document).

#include <raft/beast_http_transport.hpp>
#include <raft/beast_http_transport_impl.hpp>
#include <raft/cbor_serializer.hpp>
#include <raft/executor_default.hpp>
#include <raft/http_content_negotiation.hpp>
#include <raft/json_serializer.hpp>
#include <raft/metrics.hpp>
#include <raft/serializer_registry.hpp>

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>

#include <httplib.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

constexpr std::uint64_t node_id = 1;
constexpr std::chrono::milliseconds rpc_timeout{5000};
int warmup_samples = 20;
int measured_samples = 300;

/// Same shape as tests/beast_negotiation_retry_test.cpp's bundles: the
/// shipped Beast bundle with only the registry swapped, so the arms differ
/// in exactly the property under measurement.
template<typename Default_Serializer, typename Registry>
struct beast_negotiating_types
    : kythira::future_default_http_transport_types<Default_Serializer, kythira::noop_metrics,
                                                   kythira::executor_default> {
    using serializer_registry_type = Registry;
};

/// A relabelled JSON serializer -- the third registered type without which a
/// blind walk and an informed jump are indistinguishable (the negotiation
/// suites' own reasoning, reused verbatim).
struct alt_json_serializer : kythira::json_serializer {
    [[nodiscard]] auto media_type() const -> std::string { return "application/x-alt-json"; }
    [[nodiscard]] auto name() const -> std::string { return "alt-json"; }
};

using single_json =
    beast_negotiating_types<kythira::json_serializer,
                            kythira::single_serializer_registry<kythira::json_serializer>>;
using multi_three = beast_negotiating_types<
    kythira::cbor_serializer,
    kythira::multi_serializer_registry<kythira::cbor_serializer, alt_json_serializer,
                                       kythira::json_serializer>>;

struct arm_result {
    std::string name;
    double p50_us{0.0};
    double p95_us{0.0};
};

auto percentile(std::vector<double>& sorted_us, double pct) -> double {
    if (sorted_us.empty()) {
        return 0.0;
    }
    auto idx = static_cast<std::size_t>(pct * static_cast<double>(sorted_us.size() - 1));
    return sorted_us[idx];
}

/// The single-JSON peer, raw. Answers `/v1/raft/request_vote` with a canned
/// JSON `request_vote_response` when the request Content-Type is JSON, and
/// 415 otherwise -- with or without the `Accept-Post` header, which is the
/// one server-side degree of freedom this benchmark exists to compare.
class raw_json_server {
public:
    explicit raw_json_server(bool advertise_accept_post) {
        // Two defaults that are wrong for a latency benchmark, both observed
        // directly before being overridden here: without TCP_NODELAY the
        // renegotiated (reused-connection, steady-state ping-pong) arm reads
        // ~41ms/call -- the Nagle/delayed-ACK interaction
        // doc/http_transport_performance_comparison.md documents for
        // cpp-httplib's client -- and the default keep-alive request cap
        // closes the reused connection mid-arm, which surfaced as an
        // end-of-stream error from the Beast client's pooled connection.
        _server.set_tcp_nodelay(true);
        _server.set_keep_alive_max_count(1u << 20);
        kythira::request_vote_response<> canned{};
        canned._term = 2;
        canned._vote_granted = true;
        const auto body_bytes = kythira::json_serializer{}.serialize(canned);
        _response_body.assign(reinterpret_cast<const char*>(body_bytes.data()), body_bytes.size());

        _server.Post(
            "/v1/raft/request_vote",
            [this, advertise_accept_post](const httplib::Request& req, httplib::Response& resp) {
                if (req.get_header_value("Content-Type") == "application/json") {
                    resp.status = 200;
                    resp.set_content(_response_body, "application/json");
                    return;
                }
                resp.status = 415;
                if (advertise_accept_post) {
                    resp.set_header(kythira::header_accept_post, "application/json");
                }
                resp.set_content("Unsupported Content-Type", "text/plain");
            });
    }

    auto start() -> void {
        _port = static_cast<std::uint16_t>(_server.bind_to_any_port("127.0.0.1"));
        _thread = std::thread([this] { _server.listen_after_bind(); });
        _server.wait_until_ready();
    }

    auto stop() -> void {
        _server.stop();
        if (_thread.joinable()) {
            _thread.join();
        }
    }

    [[nodiscard]] auto port() const -> std::uint16_t { return _port; }

private:
    httplib::Server _server;
    std::thread _thread;
    std::uint16_t _port{0};
    std::string _response_body;
};

/// One first-call sample: fresh client (fresh capability cache, fresh
/// connection), one RequestVote, wall-clock microseconds.
template<typename Bundle>
auto first_call_us(boost::asio::io_context& ioc, std::uint16_t port) -> double {
    std::unordered_map<std::uint64_t, std::string> node_map{
        {node_id, "http://127.0.0.1:" + std::to_string(port)}};
    kythira::boost_beast_client<Bundle> client(ioc, node_map, {}, kythira::noop_metrics{});
    kythira::request_vote_request<> req{};
    req._term = 1;
    auto start = std::chrono::steady_clock::now();
    std::move(client.send_request_vote(node_id, req, rpc_timeout)).get();
    auto end = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::duration<double, std::micro>>(end - start)
        .count();
}

template<typename Bundle>
auto measure_first_call_arm(std::string name, boost::asio::io_context& ioc,
                            bool advertise_accept_post) -> arm_result {
    raw_json_server server(advertise_accept_post);
    server.start();
    for (int i = 0; i < warmup_samples; ++i) {
        (void)first_call_us<Bundle>(ioc, server.port());
    }
    std::vector<double> samples_us;
    samples_us.reserve(measured_samples);
    for (int i = 0; i < measured_samples; ++i) {
        samples_us.push_back(first_call_us<Bundle>(ioc, server.port()));
    }
    server.stop();
    std::sort(samples_us.begin(), samples_us.end());
    return {std::move(name), percentile(samples_us, 0.50), percentile(samples_us, 0.95)};
}

/// The renegotiated arm: one client, one negotiating first call, then the
/// measured calls all on that same client -- what every call after the
/// first looks like for the life of the (client, peer) pairing.
auto measure_renegotiated_arm(boost::asio::io_context& ioc) -> arm_result {
    raw_json_server server(/*advertise_accept_post=*/true);
    server.start();
    std::unordered_map<std::uint64_t, std::string> node_map{
        {node_id, "http://127.0.0.1:" + std::to_string(server.port())}};
    kythira::boost_beast_client<multi_three> client(ioc, node_map, {}, kythira::noop_metrics{});
    kythira::request_vote_request<> req{};
    req._term = 1;
    for (int i = 0; i < warmup_samples; ++i) {
        std::move(client.send_request_vote(node_id, req, rpc_timeout)).get();
    }
    std::vector<double> samples_us;
    samples_us.reserve(measured_samples);
    for (int i = 0; i < measured_samples; ++i) {
        auto start = std::chrono::steady_clock::now();
        std::move(client.send_request_vote(node_id, req, rpc_timeout)).get();
        auto end = std::chrono::steady_clock::now();
        samples_us.push_back(
            std::chrono::duration_cast<std::chrono::duration<double, std::micro>>(end - start)
                .count());
    }
    server.stop();
    std::sort(samples_us.begin(), samples_us.end());
    return {"renegotiated (cached, 1 RT)", percentile(samples_us, 0.50),
            percentile(samples_us, 0.95)};
}

}  // namespace

auto main(int argc, char** argv) -> int {
    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "--samples" && i + 1 < argc) {
            measured_samples = std::atoi(argv[++i]);
        } else if (arg == "--warmup" && i + 1 < argc) {
            warmup_samples = std::atoi(argv[++i]);
        }
    }

    boost::asio::io_context ioc;
    auto work_guard = boost::asio::make_work_guard(ioc);
    std::vector<std::thread> io_threads;
    for (int i = 0; i < 2; ++i) {
        // An exception escaping a completion handler propagates out of
        // run() and would terminate the whole benchmark; log it and rejoin
        // the pool instead -- the affected sample's own future still
        // carries its error to the arm that made the call.
        io_threads.emplace_back([&ioc] {
            for (;;) {
                try {
                    ioc.run();
                    break;
                } catch (const std::exception& ex) {
                    std::cerr << "io thread: " << ex.what() << "\n";
                }
            }
        });
    }

    std::cout << "Accept-Post retry-latency benchmark (Boost.Beast client, raw JSON-only peer)\n"
              << warmup_samples << " warmup + " << measured_samples
              << " measured samples per arm; first-call arms use a fresh client per sample\n"
              << "(fresh capability cache AND fresh connection -- the connect cost is in every\n"
              << "arm identically, so read the differences, not the absolute floor).\n\n";

    std::vector<arm_result> results;
    results.push_back(
        measure_first_call_arm<single_json>("matched (1 RT)", ioc, /*advertise=*/true));
    results.push_back(measure_first_call_arm<multi_three>("informed Accept-Post (2 RT)", ioc,
                                                          /*advertise=*/true));
    results.push_back(
        measure_first_call_arm<multi_three>("blind walk (3 RT)", ioc, /*advertise=*/false));
    results.push_back(measure_renegotiated_arm(ioc));

    std::cout << std::left << std::setw(32) << "Arm" << std::right << std::setw(12) << "p50 (us)"
              << std::setw(12) << "p95 (us)" << "\n"
              << std::string(56, '-') << "\n";
    for (const auto& r : results) {
        std::cout << std::left << std::setw(32) << r.name << std::right << std::setw(12)
                  << std::fixed << std::setprecision(1) << r.p50_us << std::setw(12) << r.p95_us
                  << "\n";
    }
    std::cout << "\n";

    work_guard.reset();
    ioc.stop();
    for (auto& t : io_threads) {
        t.join();
    }
    return 0;
}
