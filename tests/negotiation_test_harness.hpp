#pragma once

/// @file negotiation_test_harness.hpp
/// @brief Shared rig for the content-negotiation suites
///        (`.kiro/specs/transport-multi-serializer/`, Tasks 13-16).
///
/// Three suites drive a live `cpp_httplib_client`/`cpp_httplib_server` pair with
/// varying registries and ask the same question of each exchange: *which media
/// type did the two sides actually settle on?* That question cannot be answered
/// from the RPC's return value — a round-trip that succeeds proves only that the
/// two sides agreed on something, not that they negotiated rather than both
/// defaulting to the same thing and ignoring the headers. So the rig observes
/// the `media_type` metric dimension the transports emit, which is the only
/// place the negotiated type is visible from outside the transport.
///
/// Shared rather than copied because the three suites differ in their registry
/// pairings and in nothing else. Two copies of "start a server, run three RPCs,
/// read the metrics back" would drift, and the drift would show up as one suite
/// quietly measuring something slightly different from its neighbour.

#include <raft/http_transport.hpp>
#include <raft/http_transport_impl.hpp>
#include <raft/metrics.hpp>

#include <folly/executors/CPUThreadPoolExecutor.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace kythira::negotiation_test {

inline constexpr const char* bind_address = "127.0.0.1";
inline constexpr std::uint64_t peer_node_id = 7;
inline constexpr const char* json_media = "application/json";
inline constexpr const char* cbor_media = "application/cbor";

// ── A metrics backend that remembers what it was told ──────────────────────

/// One emitted metric, flattened to the two things these suites ask about.
struct emitted_metric {
    std::string name;
    std::unordered_map<std::string, std::string> dimensions;
};

/// Shared sink behind every copy of `recording_metrics`.
///
/// The transports take metrics *by value* and copy per emission
/// (`auto metric = _metrics;`), which is what a metrics handle is supposed to
/// support — so recording state cannot live in the handle. It lives here, behind
/// a `shared_ptr`, and every copy writes to the same sink.
struct metric_recorder {
    mutable std::mutex mutex;
    std::vector<emitted_metric> entries;

    auto record(emitted_metric e) -> void {
        const std::lock_guard<std::mutex> lock(mutex);
        entries.push_back(std::move(e));
    }

    /// @brief The `media_type` dimension of the first metric named @p name, or
    ///        an empty string if no such metric carried one.
    ///
    /// Callers must treat empty as a failure rather than as "no preference": a
    /// missing emission is exactly the "green while doing nothing" shape this
    /// repo keeps hitting, and it must not be indistinguishable from a
    /// legitimately empty value.
    [[nodiscard]] auto media_type_of(std::string_view name) const -> std::string {
        const std::lock_guard<std::mutex> lock(mutex);
        for (const auto& e : entries) {
            if (e.name != name) {
                continue;
            }
            if (const auto it = e.dimensions.find("media_type"); it != e.dimensions.end()) {
                return it->second;
            }
        }
        return {};
    }

    /// @brief Every `media_type` value seen on metrics named @p name, in order.
    [[nodiscard]] auto media_types_of(std::string_view name) const -> std::vector<std::string> {
        const std::lock_guard<std::mutex> lock(mutex);
        std::vector<std::string> result;
        for (const auto& e : entries) {
            if (e.name != name) {
                continue;
            }
            if (const auto it = e.dimensions.find("media_type"); it != e.dimensions.end()) {
                result.push_back(it->second);
            }
        }
        return result;
    }

    /// @brief Every value of dimension @p dimension on metrics named @p name.
    [[nodiscard]] auto dimension_values(std::string_view name, std::string_view dimension) const
        -> std::vector<std::string> {
        const std::lock_guard<std::mutex> lock(mutex);
        std::vector<std::string> result;
        for (const auto& e : entries) {
            if (e.name != name) {
                continue;
            }
            if (const auto it = e.dimensions.find(std::string(dimension));
                it != e.dimensions.end()) {
                result.push_back(it->second);
            }
        }
        return result;
    }

    [[nodiscard]] auto count_named(std::string_view name) const -> std::size_t {
        const std::lock_guard<std::mutex> lock(mutex);
        return static_cast<std::size_t>(
            std::count_if(entries.begin(), entries.end(),
                          [&](const emitted_metric& e) { return e.name == name; }));
    }
};

/// Models `kythira::metrics`, forwarding each emission to a shared recorder.
class recording_metrics {
public:
    recording_metrics() : _recorder(std::make_shared<metric_recorder>()) {}
    explicit recording_metrics(std::shared_ptr<metric_recorder> r) : _recorder(std::move(r)) {}

    auto set_metric_name(std::string_view name) -> void { _pending.name = std::string(name); }
    auto add_dimension(std::string_view name, std::string_view value) -> void {
        _pending.dimensions.emplace(std::string(name), std::string(value));
    }
    auto add_one() -> void {}
    auto add_count(std::int64_t) -> void {}
    auto add_duration(std::chrono::nanoseconds) -> void {}
    auto add_value(double) -> void {}
    auto emit() -> void {
        if (_recorder && !_pending.name.empty()) {
            _recorder->record(_pending);
        }
        _pending = {};
    }

    [[nodiscard]] auto recorder() const -> const std::shared_ptr<metric_recorder>& {
        return _recorder;
    }

private:
    std::shared_ptr<metric_recorder> _recorder;
    emitted_metric _pending;
};

static_assert(kythira::metrics<recording_metrics>);

// ── Types bundles ──────────────────────────────────────────────────────────

/// A `transport_types` bundle whose registry is a parameter.
///
/// The shipped bundles all hardcode `single_serializer_registry<RPC_Serializer>`
/// (`http_transport.hpp:53`), so nothing in the tree reaches a
/// `multi_serializer_registry` at all. Declaring one here is what makes the
/// multi path reachable. `Default_Serializer` must be the registry's *default*
/// serializer, since `transport_types` still requires `serializer_type` beside
/// the registry (Requirement 3.2).
///
/// Derived from `http_transport_types` rather than spelled out member by member,
/// so a bundle here differs from a shipped one in exactly one alias — the
/// registry — and in nothing else. Writing out `future_template` explicitly
/// would have hard-coded a future backend, which is wrong twice over: it would
/// silently disagree with the shipped bundle on a
/// `-DKYTHIRA_DEFAULT_FUTURE_BACKEND=boost` build, and it would make these
/// suites' results depend on a choice unrelated to content negotiation.
template<typename Default_Serializer, typename Registry>
struct negotiating_transport_types
    : kythira::http_transport_types<Default_Serializer, recording_metrics,
                                    folly::CPUThreadPoolExecutor> {
    using serializer_registry_type = Registry;
};

// ── Observation ────────────────────────────────────────────────────────────

/// What one client/server pairing was observed to do.
struct exchange_observation {
    /// The `media_type` the client reported sending in, per request.
    std::vector<std::string> client_request_types;
    /// The `media_type` the client reported decoding each response in.
    std::vector<std::string> client_response_types;
    /// The `media_type` the server reported encoding its response in.
    std::string server_response_type;
    /// Handler invocations, so "it round-tripped" can be told from "it 415'd".
    int handler_invocations{0};
    /// False if any RPC returned a wrong value.
    bool round_trips_correct{true};
    /// RPCs whose future came back failed, and the `what()` of the first.
    int failed_rpcs{0};
    std::string first_error;
};

/// @brief Run `request_vote`, `append_entries` and `install_snapshot` from a
///        @p Client_Types client against a @p Server_Types server on @p port.
///
/// All three RPCs rather than one, because each has its own endpoint and its own
/// `handle` instantiation in `dispatch` — negotiation wired into one endpoint
/// and not the others would round-trip perfectly on whichever one a single-RPC
/// test happened to pick.
///
/// Failures are **recorded, not thrown**. The interop suite needs to observe
/// that a pairing fails and how, and a rig that let the first exception escape
/// could only ever report "something went wrong somewhere".
template<typename Client_Types, typename Server_Types>
auto run_exchange(std::uint16_t port) -> exchange_observation {
    exchange_observation obs;

    recording_metrics server_metrics;
    recording_metrics client_metrics;

    kythira::cpp_httplib_server_config server_config;
    server_config.request_timeout = std::chrono::seconds{5};

    kythira::cpp_httplib_server<Server_Types> server(bind_address, port, server_config,
                                                     server_metrics);

    int invocations = 0;
    server.register_request_vote_handler([&](const kythira::request_vote_request<>& req) {
        ++invocations;
        kythira::request_vote_response<> resp;
        resp._term = req.term() + 1;
        resp._vote_granted = true;
        return resp;
    });
    server.register_append_entries_handler([&](const kythira::append_entries_request<>& req) {
        ++invocations;
        kythira::append_entries_response<> resp;
        resp._term = req.term();
        resp._success = true;
        return resp;
    });
    server.register_install_snapshot_handler([&](const kythira::install_snapshot_request<>& req) {
        ++invocations;
        kythira::install_snapshot_response<> resp;
        resp._term = req.term();
        return resp;
    });

    server.start();
    std::this_thread::sleep_for(std::chrono::milliseconds{150});

    auto note_failure = [&](const std::exception& e) {
        ++obs.failed_rpcs;
        if (obs.first_error.empty()) {
            obs.first_error = e.what();
        }
    };

    {
        kythira::cpp_httplib_client_config client_config;
        client_config.connection_timeout = std::chrono::milliseconds{2000};
        client_config.request_timeout = std::chrono::milliseconds{4000};

        std::unordered_map<std::uint64_t, std::string> node_urls;
        node_urls[peer_node_id] =
            std::string("http://") + bind_address + ":" + std::to_string(port);

        kythira::cpp_httplib_client<Client_Types> client(std::move(node_urls), client_config,
                                                         client_metrics);

        try {
            kythira::request_vote_request<> req;
            req._term = 5;
            req._candidate_id = 42;
            req._last_log_index = 10;
            req._last_log_term = 4;
            auto resp =
                client.send_request_vote(peer_node_id, req, std::chrono::milliseconds{3000}).get();
            obs.round_trips_correct =
                obs.round_trips_correct && resp.term() == 6 && resp.vote_granted();
        } catch (const std::exception& e) {
            note_failure(e);
        }
        try {
            kythira::append_entries_request<> req;
            req._term = 6;
            req._leader_id = 42;
            req._prev_log_index = 10;
            req._prev_log_term = 5;
            req._leader_commit = 9;
            auto resp =
                client.send_append_entries(peer_node_id, req, std::chrono::milliseconds{3000})
                    .get();
            obs.round_trips_correct = obs.round_trips_correct && resp.term() == 6 && resp.success();
        } catch (const std::exception& e) {
            note_failure(e);
        }
        try {
            kythira::install_snapshot_request<> req;
            req._term = 7;
            req._leader_id = 42;
            req._last_included_index = 100;
            req._last_included_term = 6;
            req._offset = 0;
            req._data = {std::byte{'s'}, std::byte{'n'}, std::byte{'a'}, std::byte{'p'}};
            req._done = true;
            auto resp =
                client.send_install_snapshot(peer_node_id, req, std::chrono::milliseconds{3000})
                    .get();
            obs.round_trips_correct = obs.round_trips_correct && resp.term() == 7;
        } catch (const std::exception& e) {
            note_failure(e);
        }
    }

    server.stop();

    obs.handler_invocations = invocations;
    obs.client_request_types =
        client_metrics.recorder()->media_types_of("http.client.request.sent");
    // `http.client.response.size` is the client-side emission carrying the
    // *response* media type — the one the client actually decoded with, as
    // opposed to `request.sent`'s outgoing type. The two differ whenever a peer
    // answers in something other than what it was asked in, so reading only one
    // would leave half of each exchange unobserved.
    obs.client_response_types =
        client_metrics.recorder()->media_types_of("http.client.response.size");
    obs.server_response_type =
        server_metrics.recorder()->media_type_of("http.server.request.latency");
    return obs;
}

}  // namespace kythira::negotiation_test
