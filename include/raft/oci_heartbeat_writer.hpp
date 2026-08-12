#pragma once

/// @file oci_heartbeat_writer.hpp
/// @brief The node-side half of the OCI heartbeat protocol (Requirement 4.4).
///
/// `oci_instance_pool_quorum_manager::assess_quorum` classifies an instance
/// live only while its `kythira-last-heartbeat` freeform tag is fresh
/// (Requirement 5.3) — and Requirement 4.4 places writing that tag
/// *exclusively* with the kythira node process running on the instance, via
/// its own OCI API calls under Instance Principal auth. This is that writer.
/// The quorum manager never touches the tag; a manager that wrote it would be
/// attesting its own liveness guess rather than relaying the node's.
///
/// The writer is auth-mode agnostic on purpose: it speaks through
/// `oci_http_client`, so the same code path a real node exercises with
/// `use_instance_principal` runs in the mock tier with API-key signing —
/// which is also why Instance Principal routing itself is not re-tested here
/// (that is `oci_federation_unit_test`'s contract).

#include <raft/oci_client_config.hpp>
#include <raft/oci_http_client.hpp>
#include <raft/oci_instance_pool_quorum_manager.hpp>

#include <boost/json.hpp>

#include <httplib.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <ctime>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace kythira {

struct oci_heartbeat_writer_config {
    /// Auth + region for the API calls. On a real instance this has
    /// `use_instance_principal = true` and `region` set; the mock tier points
    /// `endpoint_override` at `oci_mock_server` with API-key fields instead.
    oci_client_config client;

    /// The instance to heartbeat. Empty means "this instance": the OCID is
    /// fetched once, lazily, from the metadata service's `/instance/id` —
    /// the same v2 endpoint (and the same `Authorization: Bearer Oracle`
    /// requirement) `instance_principal_signer` reads its identity from.
    std::string instance_id;

    /// Metadata service base for the instance-id discovery above,
    /// `/opc/v2` included. Overridden by `$OCI_METADATA_BASE_URL` when set —
    /// the same override, honoured the same way, as
    /// `instance_principal_signer::options_from`, so a mock metadata server
    /// stands in for both consumers with one environment variable.
    std::string metadata_base_url{"http://169.254.169.254/opc/v2"};

    /// How often `start()`'s background loop writes. The default sits well
    /// inside `assess_quorum`'s 30s default `heartbeat_timeout`: three missed
    /// writes, not one, separate a hiccup from a stale classification.
    std::chrono::seconds interval{10};
};

/// @brief Periodically stamps this instance's `kythira-last-heartbeat` tag.
///
/// Each write is the read-merge-write cycle Requirement 4.2 mandates for
/// *every* tag write: `GetInstance` for the current `freeformTags`, merge the
/// heartbeat key client-side, `UpdateInstance` with the full merged map —
/// because `UpdateInstance` replaces the tag map rather than merging into it,
/// and a bare write would silently clobber the manager's identity tags and
/// any operator-set ones (design.md Property 1).
///
/// Failures inside the background loop are recorded (`last_error`,
/// `write_failures`) and the loop keeps going: a missed heartbeat is exactly
/// the signal `assess_quorum` exists to notice, so dying on the first API
/// error would convert a transient 500 into a permanently silent node. The
/// synchronous `write_once` throws instead — a caller invoking it directly
/// asked a question and deserves the answer.
class oci_heartbeat_writer {
public:
    explicit oci_heartbeat_writer(oci_heartbeat_writer_config cfg)
        : _cfg(std::move(cfg)), _http(_cfg.client) {
        if (const char* base = std::getenv("OCI_METADATA_BASE_URL"); base != nullptr) {
            _cfg.metadata_base_url = base;
        }
    }

    oci_heartbeat_writer(const oci_heartbeat_writer&) = delete;
    auto operator=(const oci_heartbeat_writer&) -> oci_heartbeat_writer& = delete;
    oci_heartbeat_writer(oci_heartbeat_writer&&) = delete;
    auto operator=(oci_heartbeat_writer&&) -> oci_heartbeat_writer& = delete;

    ~oci_heartbeat_writer() { stop(); }

    /// @brief One read-merge-write heartbeat, throwing on any failure.
    ///
    /// @param now Injectable for the same reason `oci_signing`'s is: a test
    ///            that cannot pin the clock cannot pin the written value.
    /// @return The timestamp written, i.e. @p now.
    auto write_once(std::time_t now = std::time(nullptr)) -> std::time_t {
        const auto& id = resolve_instance_id();
        const auto instance = _http.request("iaas", "GET", "/20160918/instances/" + id);
        auto tags = oci_detail::freeform_tags_of(instance);
        tags[oci_detail::tag_last_heartbeat] = std::to_string(static_cast<std::int64_t>(now));

        boost::json::object merged;
        for (const auto& [key, value] : tags) {
            merged[key] = value;
        }
        boost::json::object update;
        update["freeformTags"] = std::move(merged);
        (void)_http.request("iaas", "PUT", "/20160918/instances/" + id,
                            boost::json::serialize(update));
        _writes.fetch_add(1);
        return now;
    }

    /// @brief Start the background loop: one write immediately, then one per
    ///        `interval` until `stop()`.
    ///
    /// The immediate first write is load-bearing: a freshly launched node is
    /// only covered by `heartbeat_grace_period`, and waiting a full interval
    /// before the first stamp spends that grace for no reason.
    auto start() -> void {
        if (_thread.joinable()) {
            return;
        }
        _thread = std::jthread([this](std::stop_token stop_token) {
            std::unique_lock<std::mutex> lock(_sleep_mutex);
            while (!stop_token.stop_requested()) {
                lock.unlock();
                try {
                    (void)write_once();
                    set_last_error({});
                } catch (const std::exception& ex) {
                    _write_failures.fetch_add(1);
                    set_last_error(ex.what());
                }
                lock.lock();
                // condition_variable_any + stop_token: stop() interrupts the
                // wait instead of paying up to a full interval on shutdown.
                _sleep_cv.wait_for(lock, stop_token, _cfg.interval, [] { return false; });
            }
        });
    }

    auto stop() -> void {
        if (_thread.joinable()) {
            _thread.request_stop();
            _thread.join();
        }
    }

    /// Successful writes so far — lets a test (or an operator probe) assert
    /// "the loop is alive" without reading tags back.
    [[nodiscard]] auto writes() const -> std::uint64_t { return _writes.load(); }

    /// Failed write attempts from the background loop.
    [[nodiscard]] auto write_failures() const -> std::uint64_t { return _write_failures.load(); }

    /// The most recent loop failure, empty after any subsequent success.
    [[nodiscard]] auto last_error() const -> std::string {
        const std::lock_guard<std::mutex> lock(_error_mutex);
        return _last_error;
    }

private:
    /// The configured instance OCID, or — once, cached — this instance's own,
    /// from the metadata service. Same retry shape as
    /// `instance_principal_signer::fetch_metadata`: three attempts with a
    /// short pause, because the caller has its own timeout budget. Guarded by
    /// `_id_mutex` so a direct `write_once` caller and the background loop
    /// cannot race the first discovery.
    [[nodiscard]] auto resolve_instance_id() -> const std::string& {
        const std::lock_guard<std::mutex> lock(_id_mutex);
        if (!_cfg.instance_id.empty()) {
            return _cfg.instance_id;
        }
        const auto [origin, prefix] = split_origin(_cfg.metadata_base_url);
        httplib::Client client(origin);
        client.set_connection_timeout(static_cast<time_t>(_cfg.client.api_timeout.count()), 0);
        client.set_read_timeout(static_cast<time_t>(_cfg.client.api_timeout.count()), 0);
        const httplib::Headers headers{{"Authorization", "Bearer Oracle"}};

        std::string last_error;
        for (int attempt = 0; attempt < 3; ++attempt) {
            if (attempt != 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
            }
            auto result = client.Get(prefix + "/instance/id", headers);
            if (result && result->status == 200 && !result->body.empty()) {
                _cfg.instance_id = result->body;
                return _cfg.instance_id;
            }
            last_error = result ? "HTTP " + std::to_string(result->status)
                                : httplib::to_string(result.error());
        }
        throw std::runtime_error("oci_heartbeat_writer: could not read /instance/id from " +
                                 _cfg.metadata_base_url + ": " + last_error);
    }

    /// `"http://host:port/opc/v2"` → `{"http://host:port", "/opc/v2"}`.
    [[nodiscard]] static auto split_origin(const std::string& base)
        -> std::pair<std::string, std::string> {
        const auto scheme_end = base.find("://");
        const auto path_start =
            base.find('/', scheme_end == std::string::npos ? 0 : scheme_end + 3);
        if (path_start == std::string::npos) {
            return {base, ""};
        }
        return {base.substr(0, path_start), base.substr(path_start)};
    }

    auto set_last_error(std::string error) -> void {
        const std::lock_guard<std::mutex> lock(_error_mutex);
        _last_error = std::move(error);
    }

    oci_heartbeat_writer_config _cfg;
    std::mutex _id_mutex;
    oci_http_client _http;
    std::jthread _thread;
    std::mutex _sleep_mutex;
    std::condition_variable_any _sleep_cv;
    std::atomic<std::uint64_t> _writes{0};
    std::atomic<std::uint64_t> _write_failures{0};
    mutable std::mutex _error_mutex;
    std::string _last_error;
};

}  // namespace kythira
