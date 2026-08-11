#pragma once

/// @file oci_mock_server.hpp
/// @brief An in-memory stand-in for the handful of OCI REST routes
///        `oci_instance_pool_quorum_manager` and `oci_certificates_provider`
///        actually call (`.kiro/specs/oci-cloud-provider/`, Requirement 13.5).
///
/// OCI has no LocalStack. Every other cloud provider in this tree gets a
/// vendor-or-community emulator for its mid-tier tests; OCI's tier has to be
/// built, which is what this is — the same role `tests/acme_test_server.hpp`
/// already plays for ACME, and for the same reason.
///
/// **What it models and what it does not.** It models *state machines and call
/// sequences*: pool size drives instance creation, tags are replaced rather than
/// merged, a detached instance terminates, a certificate becomes ACTIVE. It does
/// not model signing beyond checking that an `authorization` header of the right
/// shape arrived — the signature itself is already pinned by
/// `oci_signing_unit_test.cpp` against golden vectors, and a mock that re-derived
/// it would be verifying the client against a second copy of the client's own
/// logic. It rejects an unsigned request, though: that is the one signing claim
/// this file is in a position to make independently.
///
/// **Tag replacement is modelled faithfully on purpose.** `UpdateInstance` here
/// *replaces* `freeformTags` exactly as OCI does. A mock that merged would make
/// design.md's Property 1 untestable — the read-merge-write bug this project
/// specifically calls out would pass against it.
///
/// One server answers for every OCI service, because `oci_client_config::
/// endpoint_override` replaces the host for all of them at once. Route patterns
/// are therefore globally distinct, which they naturally are: the two services
/// have different API-version prefixes.

#include <httplib.h>

#include <boost/json.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace kythira::testing {

/// @brief A fake OCI control plane over `httplib::Server`.
///
/// Every accessor takes the same lock the request handlers do, so a test can
/// inspect or mutate state while the server is running without racing it.
class oci_mock_server {
public:
    /// One compute instance, as `GetInstance` reports it.
    struct instance_state {
        std::string id;
        std::string lifecycle_state{"RUNNING"};
        std::string availability_domain;
        std::string time_created;
        std::map<std::string, std::string> freeform_tags;
        std::string private_ip;
        std::string vnic_id;
        bool in_pool{true};
    };

    /// One certificate version, as `ListCertificateVersions` reports it.
    struct certificate_version_state {
        std::int64_t version_number{1};
        std::string version_name;
        std::string serial_number;
        bool revoked{false};
    };

    /// One certificate resource, as `GetCertificate` reports it.
    struct certificate_state {
        std::string id;
        std::string name;
        std::string issuer_certificate_authority_id;
        std::string csr_pem;
        std::string lifecycle_state{"ACTIVE"};
        /// How many `GetCertificate` calls answer `CREATING` before one answers
        /// `ACTIVE`. Zero (the default) is the instantaneous transition Task 4
        /// asks for; a test that wants to exercise the poll loop sets it
        /// non-zero, and then sees `ACTIVE` on call N+1.
        int creating_polls_remaining{0};
        std::vector<certificate_version_state> versions;
    };

    explicit oci_mock_server(std::uint16_t port, std::string availability_domain = "kIdk:PHX-AD-1")
        : _port(port), _availability_domain(std::move(availability_domain)) {
        install_routes();
    }

    oci_mock_server(const oci_mock_server&) = delete;
    auto operator=(const oci_mock_server&) -> oci_mock_server& = delete;
    oci_mock_server(oci_mock_server&&) = delete;
    auto operator=(oci_mock_server&&) -> oci_mock_server& = delete;

    ~oci_mock_server() { stop(); }

    auto start() -> void {
        _thread = std::thread([this] { _server.listen("127.0.0.1", _port); });
        _server.wait_until_ready();
    }

    auto stop() -> void {
        _server.stop();
        if (_thread.joinable()) {
            _thread.join();
        }
    }

    /// The `endpoint_override` a client should be pointed at.
    [[nodiscard]] auto origin() const -> std::string {
        return "http://127.0.0.1:" + std::to_string(_port);
    }

    [[nodiscard]] auto pool_id() const -> std::string { return _pool_id; }
    [[nodiscard]] auto certificate_authority_id() const -> std::string { return _ca_id; }
    [[nodiscard]] auto availability_domain() const -> std::string { return _availability_domain; }

    /// @brief Whether growing the pool immediately materialises instances.
    ///
    /// True (the default) is Task 4's "synchronous transition, no simulated boot
    /// delay". False is how a test reaches `provision_node`'s timeout-and-roll-
    /// back path: the size goes up and nothing ever appears, which is exactly
    /// what a real capacity shortfall looks like from the client's side.
    auto set_auto_launch(bool enabled) -> void {
        const std::lock_guard<std::mutex> lock(_mutex);
        _auto_launch = enabled;
    }

    [[nodiscard]] auto pool_size() -> std::int64_t {
        const std::lock_guard<std::mutex> lock(_mutex);
        return _pool_size;
    }

    /// Seed an instance that is already in the pool. Returns its OCID.
    auto add_instance(std::map<std::string, std::string> tags,
                      std::string lifecycle_state = "RUNNING") -> std::string {
        const std::lock_guard<std::mutex> lock(_mutex);
        auto id = launch_locked();
        _instances[id].freeform_tags = std::move(tags);
        _instances[id].lifecycle_state = std::move(lifecycle_state);
        _pool_size = static_cast<std::int64_t>(live_membership_locked());
        return id;
    }

    [[nodiscard]] auto instance(const std::string& id) -> std::optional<instance_state> {
        const std::lock_guard<std::mutex> lock(_mutex);
        const auto it = _instances.find(id);
        if (it == _instances.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    /// Every instance the pool currently reports, in creation order.
    [[nodiscard]] auto pool_instances() -> std::vector<instance_state> {
        const std::lock_guard<std::mutex> lock(_mutex);
        std::vector<instance_state> out;
        for (const auto& id : _pool_membership) {
            const auto it = _instances.find(id);
            if (it != _instances.end() && it->second.in_pool) {
                out.push_back(it->second);
            }
        }
        return out;
    }

    auto set_lifecycle_state(const std::string& id, std::string state) -> void {
        const std::lock_guard<std::mutex> lock(_mutex);
        if (const auto it = _instances.find(id); it != _instances.end()) {
            it->second.lifecycle_state = std::move(state);
        }
    }

    auto set_freeform_tag(const std::string& id, const std::string& key, std::string value)
        -> void {
        const std::lock_guard<std::mutex> lock(_mutex);
        if (const auto it = _instances.find(id); it != _instances.end()) {
            it->second.freeform_tags[key] = std::move(value);
        }
    }

    /// How many `CREATING` answers a freshly created certificate gives before
    /// going `ACTIVE`.
    auto set_certificate_creating_polls(int polls) -> void {
        const std::lock_guard<std::mutex> lock(_mutex);
        _certificate_creating_polls = polls;
    }

    [[nodiscard]] auto certificates() -> std::vector<certificate_state> {
        const std::lock_guard<std::mutex> lock(_mutex);
        std::vector<certificate_state> out;
        out.reserve(_certificates.size());
        for (const auto& [id, cert] : _certificates) {
            out.push_back(cert);
        }
        return out;
    }

    /// The PEM `GetCertificateAuthorityBundle` answers with.
    [[nodiscard]] auto root_pem() const -> std::string { return _root_pem; }

    /// How many times each route was hit, keyed `"{METHOD} {path-pattern}"`.
    ///
    /// The caching claim in Requirement 12.5 is a claim about *call count*, not
    /// about the returned value — two identical PEMs prove nothing on their own.
    [[nodiscard]] auto hits(const std::string& key) -> std::size_t {
        const std::lock_guard<std::mutex> lock(_mutex);
        const auto it = _hits.find(key);
        return it == _hits.end() ? 0 : it->second;
    }

    /// Bodies of every `PUT /20160918/instances/{id}` the server saw, in order.
    [[nodiscard]] auto update_instance_bodies() -> std::vector<std::string> {
        const std::lock_guard<std::mutex> lock(_mutex);
        return _update_instance_bodies;
    }

private:
    static auto rfc3339_now(std::time_t offset_seconds = 0) -> std::string {
        const std::time_t when = std::time(nullptr) + offset_seconds;
        std::tm tm_utc{};
        gmtime_r(&when, &tm_utc);
        std::array<char, 32> buf{};
        const int written = std::snprintf(buf.data(), buf.size(), "%04d-%02d-%02dT%02d:%02d:%02dZ",
                                          tm_utc.tm_year + 1900, tm_utc.tm_mon + 1, tm_utc.tm_mday,
                                          tm_utc.tm_hour, tm_utc.tm_min, tm_utc.tm_sec);
        return std::string(buf.data(), static_cast<std::size_t>(written));
    }

    static auto json_reply(httplib::Response& resp, const boost::json::value& value,
                           int status = 200) -> void {
        resp.status = status;
        resp.set_content(boost::json::serialize(value), "application/json");
    }

    static auto error_reply(httplib::Response& resp, int status, const std::string& code,
                            const std::string& message) -> void {
        boost::json::object body;
        body["code"] = code;
        body["message"] = message;
        resp.status = status;
        resp.set_content(boost::json::serialize(body), "application/json");
    }

    /// Requirement 1.4's `authorization` header must be there and must look like
    /// the scheme. Not a signature check — see the file comment.
    static auto authorized(const httplib::Request& req, httplib::Response& resp) -> bool {
        const auto header = req.get_header_value("authorization");
        if (header.starts_with(R"(Signature version="1",)")) {
            return true;
        }
        error_reply(resp, 401, "NotAuthenticated",
                    "request carried no OCI Request Signing v1 authorization header");
        return false;
    }

    auto record_hit(const std::string& key) -> void { _hits[key]++; }

    /// Caller holds `_mutex`.
    [[nodiscard]] auto launch_locked() -> std::string {
        const auto ordinal = ++_instance_counter;
        instance_state inst;
        inst.id = "ocid1.instance.oc1.phx.mock" + std::to_string(ordinal);
        inst.availability_domain = _availability_domain;
        inst.time_created = rfc3339_now();
        inst.private_ip = "10.0.0." + std::to_string(10 + ordinal);
        inst.vnic_id = "ocid1.vnic.oc1.phx.mock" + std::to_string(ordinal);
        _pool_membership.push_back(inst.id);
        _instances.emplace(inst.id, std::move(inst));
        return _pool_membership.back();
    }

    /// Caller holds `_mutex`.
    [[nodiscard]] auto live_membership_locked() const -> std::size_t {
        std::size_t count = 0;
        for (const auto& id : _pool_membership) {
            const auto it = _instances.find(id);
            if (it != _instances.end() && it->second.in_pool) {
                ++count;
            }
        }
        return count;
    }

    /// Caller holds `_mutex`. Grows membership to match `_pool_size`.
    auto reconcile_size_locked() -> void {
        if (!_auto_launch) {
            return;
        }
        while (static_cast<std::int64_t>(live_membership_locked()) < _pool_size) {
            (void)launch_locked();
        }
    }

    auto install_routes() -> void {
        install_compute_routes();
        install_certificate_routes();
    }

    auto install_compute_routes() -> void {
        // GetInstancePool
        _server.Get(R"(/20160918/instancePools/([^/]+))", [this](const httplib::Request& req,
                                                                 httplib::Response& resp) {
            if (!authorized(req, resp)) {
                return;
            }
            const std::lock_guard<std::mutex> lock(_mutex);
            record_hit("GET /20160918/instancePools/{id}");
            if (req.matches[1] != _pool_id) {
                error_reply(resp, 404, "NotAuthorizedOrNotFound", "instance pool not found");
                return;
            }
            boost::json::object placement;
            placement["availabilityDomain"] = _availability_domain;
            placement["primarySubnetId"] = "ocid1.subnet.oc1.phx.mock";
            boost::json::object pool;
            pool["id"] = _pool_id;
            pool["compartmentId"] = "ocid1.compartment.oc1..mock";
            pool["size"] = _pool_size;
            pool["lifecycleState"] = "RUNNING";
            pool["placementConfigurations"] = boost::json::array{std::move(placement)};
            json_reply(resp, pool);
        });

        // UpdateInstancePool
        _server.Put(R"(/20160918/instancePools/([^/]+))", [this](const httplib::Request& req,
                                                                 httplib::Response& resp) {
            if (!authorized(req, resp)) {
                return;
            }
            const std::lock_guard<std::mutex> lock(_mutex);
            record_hit("PUT /20160918/instancePools/{id}");
            if (req.matches[1] != _pool_id) {
                error_reply(resp, 404, "NotAuthorizedOrNotFound", "instance pool not found");
                return;
            }
            boost::json::value parsed;
            try {
                parsed = boost::json::parse(req.body);
            } catch (const std::exception&) {
                error_reply(resp, 400, "InvalidParameter", "unparseable body");
                return;
            }
            const auto* obj = parsed.if_object();
            const auto* size = obj != nullptr ? obj->if_contains("size") : nullptr;
            if (size == nullptr || !(size->is_int64() || size->is_uint64())) {
                error_reply(resp, 400, "InvalidParameter", "size is required");
                return;
            }
            _pool_size = size->is_int64() ? size->get_int64()
                                          : static_cast<std::int64_t>(size->get_uint64());
            reconcile_size_locked();
            boost::json::object pool;
            pool["id"] = _pool_id;
            pool["size"] = _pool_size;
            pool["lifecycleState"] = "SCALING";
            json_reply(resp, pool);
        });

        // ListInstancePoolInstances
        _server.Get(R"(/20160918/instancePools/([^/]+)/instances)",
                    [this](const httplib::Request& req, httplib::Response& resp) {
                        if (!authorized(req, resp)) {
                            return;
                        }
                        const std::lock_guard<std::mutex> lock(_mutex);
                        record_hit("GET /20160918/instancePools/{id}/instances");
                        if (req.matches[1] != _pool_id) {
                            error_reply(resp, 404, "NotAuthorizedOrNotFound",
                                        "instance pool not found");
                            return;
                        }
                        boost::json::array out;
                        for (const auto& id : _pool_membership) {
                            const auto it = _instances.find(id);
                            if (it == _instances.end() || !it->second.in_pool) {
                                continue;
                            }
                            boost::json::object summary;
                            summary["id"] = it->second.id;
                            summary["availabilityDomain"] = it->second.availability_domain;
                            summary["state"] = it->second.lifecycle_state;
                            out.push_back(std::move(summary));
                        }
                        json_reply(resp, out);
                    });

        // DetachInstancePoolInstance
        _server.Post(
            R"(/20160918/instancePools/([^/]+)/actions/detachInstance)",
            [this](const httplib::Request& req, httplib::Response& resp) {
                if (!authorized(req, resp)) {
                    return;
                }
                const std::lock_guard<std::mutex> lock(_mutex);
                record_hit("POST /20160918/instancePools/{id}/actions/detachInstance");
                if (req.matches[1] != _pool_id) {
                    error_reply(resp, 404, "NotAuthorizedOrNotFound", "instance pool not found");
                    return;
                }
                boost::json::value parsed;
                try {
                    parsed = boost::json::parse(req.body);
                } catch (const std::exception&) {
                    error_reply(resp, 400, "InvalidParameter", "unparseable body");
                    return;
                }
                const auto* obj = parsed.if_object();
                const auto* instance_id = obj != nullptr ? obj->if_contains("instanceId") : nullptr;
                if (instance_id == nullptr || !instance_id->is_string()) {
                    error_reply(resp, 400, "InvalidParameter", "instanceId is required");
                    return;
                }
                const auto it = _instances.find(std::string(instance_id->get_string()));
                if (it == _instances.end() || !it->second.in_pool) {
                    error_reply(resp, 404, "NotAuthorizedOrNotFound",
                                "instance not found in this pool");
                    return;
                }
                it->second.in_pool = false;
                const auto* auto_terminate = obj->if_contains("isAutoTerminate");
                if (auto_terminate != nullptr && auto_terminate->is_bool() &&
                    auto_terminate->get_bool()) {
                    it->second.lifecycle_state = "TERMINATED";
                } else {
                    it->second.lifecycle_state = "STOPPED";
                }
                const auto* decrement = obj->if_contains("isDecrementSize");
                if (decrement != nullptr && decrement->is_bool() && decrement->get_bool() &&
                    _pool_size > 0) {
                    --_pool_size;
                }
                resp.status = 204;
            });

        // GetInstance
        _server.Get(R"(/20160918/instances/([^/]+))",
                    [this](const httplib::Request& req, httplib::Response& resp) {
                        if (!authorized(req, resp)) {
                            return;
                        }
                        const std::lock_guard<std::mutex> lock(_mutex);
                        record_hit("GET /20160918/instances/{id}");
                        const auto it = _instances.find(req.matches[1]);
                        if (it == _instances.end()) {
                            error_reply(resp, 404, "NotAuthorizedOrNotFound", "instance not found");
                            return;
                        }
                        json_reply(resp, instance_json(it->second));
                    });

        // UpdateInstance — replaces freeformTags outright, exactly as OCI does.
        _server.Put(R"(/20160918/instances/([^/]+))", [this](const httplib::Request& req,
                                                             httplib::Response& resp) {
            if (!authorized(req, resp)) {
                return;
            }
            const std::lock_guard<std::mutex> lock(_mutex);
            record_hit("PUT /20160918/instances/{id}");
            _update_instance_bodies.push_back(req.body);
            const auto it = _instances.find(req.matches[1]);
            if (it == _instances.end()) {
                error_reply(resp, 404, "NotAuthorizedOrNotFound", "instance not found");
                return;
            }
            boost::json::value parsed;
            try {
                parsed = boost::json::parse(req.body);
            } catch (const std::exception&) {
                error_reply(resp, 400, "InvalidParameter", "unparseable body");
                return;
            }
            const auto* obj = parsed.if_object();
            const auto* tags = obj != nullptr ? obj->if_contains("freeformTags") : nullptr;
            if (tags != nullptr) {
                std::map<std::string, std::string> replacement;
                if (const auto* tag_obj = tags->if_object(); tag_obj != nullptr) {
                    for (const auto& entry : *tag_obj) {
                        if (entry.value().is_string()) {
                            replacement.emplace(std::string(entry.key()),
                                                std::string(entry.value().get_string()));
                        }
                    }
                }
                it->second.freeform_tags = std::move(replacement);
            }
            json_reply(resp, instance_json(it->second));
        });

        // ListVnicAttachments
        _server.Get(R"(/20160918/vnicAttachments)",
                    [this](const httplib::Request& req, httplib::Response& resp) {
                        if (!authorized(req, resp)) {
                            return;
                        }
                        const std::lock_guard<std::mutex> lock(_mutex);
                        record_hit("GET /20160918/vnicAttachments");
                        const auto instance_id = req.get_param_value("instanceId");
                        boost::json::array out;
                        for (const auto& [id, inst] : _instances) {
                            if (!instance_id.empty() && id != instance_id) {
                                continue;
                            }
                            boost::json::object attachment;
                            attachment["id"] = "ocid1.vnicattachment.oc1.phx." + id;
                            attachment["instanceId"] = id;
                            attachment["vnicId"] = inst.vnic_id;
                            attachment["lifecycleState"] = "ATTACHED";
                            out.push_back(std::move(attachment));
                        }
                        json_reply(resp, out);
                    });

        // GetVnic
        _server.Get(R"(/20160918/vnics/([^/]+))",
                    [this](const httplib::Request& req, httplib::Response& resp) {
                        if (!authorized(req, resp)) {
                            return;
                        }
                        const std::lock_guard<std::mutex> lock(_mutex);
                        record_hit("GET /20160918/vnics/{id}");
                        const std::string vnic_id = req.matches[1];
                        for (const auto& [id, inst] : _instances) {
                            if (inst.vnic_id != vnic_id) {
                                continue;
                            }
                            boost::json::object vnic;
                            vnic["id"] = vnic_id;
                            vnic["privateIp"] = inst.private_ip;
                            vnic["isPrimary"] = true;
                            json_reply(resp, vnic);
                            return;
                        }
                        error_reply(resp, 404, "NotAuthorizedOrNotFound", "vnic not found");
                    });
    }

    auto install_certificate_routes() -> void {
        // CreateCertificate
        _server.Post(R"(/20210224/certificates)", [this](const httplib::Request& req,
                                                         httplib::Response& resp) {
            if (!authorized(req, resp)) {
                return;
            }
            const std::lock_guard<std::mutex> lock(_mutex);
            record_hit("POST /20210224/certificates");
            boost::json::value parsed;
            try {
                parsed = boost::json::parse(req.body);
            } catch (const std::exception&) {
                error_reply(resp, 400, "InvalidParameter", "unparseable body");
                return;
            }
            const auto* obj = parsed.if_object();
            const auto* config = obj != nullptr ? obj->if_contains("certificateConfig") : nullptr;
            const auto* config_obj = config != nullptr ? config->if_object() : nullptr;
            if (config_obj == nullptr) {
                error_reply(resp, 400, "InvalidParameter", "certificateConfig is required");
                return;
            }
            const auto config_type = string_field(*config_obj, "configType");
            const auto csr_pem = string_field(*config_obj, "csrPem");
            const auto issuer = string_field(*config_obj, "issuerCertificateAuthorityId");
            // The two mandatory fields of
            // MANAGED_EXTERNALLY_ISSUED_BY_INTERNAL_CA. Rejecting
            // here is what makes "the provider sends a CSR" an
            // assertion rather than an assumption.
            if (config_type != "MANAGED_EXTERNALLY_ISSUED_BY_INTERNAL_CA") {
                error_reply(resp, 400, "InvalidParameter",
                            "unsupported configType: " + config_type);
                return;
            }
            if (csr_pem.empty() || issuer.empty()) {
                error_reply(resp, 400, "InvalidParameter",
                            "csrPem and issuerCertificateAuthorityId are mandatory");
                return;
            }

            const auto ordinal = ++_certificate_counter;
            certificate_state cert;
            cert.id = "ocid1.certificate.oc1.phx.mock" + std::to_string(ordinal);
            cert.name = string_field(*obj, "name");
            cert.issuer_certificate_authority_id = issuer;
            cert.csr_pem = csr_pem;
            cert.creating_polls_remaining = _certificate_creating_polls;
            cert.lifecycle_state = cert.creating_polls_remaining > 0 ? "CREATING" : "ACTIVE";
            cert.versions.push_back(certificate_version_state{
                .version_number = 1,
                .version_name = "v1",
                .serial_number = std::to_string(100000 + ordinal),
                .revoked = false,
            });
            const auto id = cert.id;
            _certificates.emplace(id, std::move(cert));
            json_reply(resp, certificate_json(_certificates.at(id)), 200);
        });

        // ListCertificates
        _server.Get(R"(/20210224/certificates)",
                    [this](const httplib::Request& req, httplib::Response& resp) {
                        if (!authorized(req, resp)) {
                            return;
                        }
                        const std::lock_guard<std::mutex> lock(_mutex);
                        record_hit("GET /20210224/certificates");
                        const auto issuer = req.get_param_value("issuerCertificateAuthorityId");
                        boost::json::array items;
                        for (const auto& [id, cert] : _certificates) {
                            if (!issuer.empty() && cert.issuer_certificate_authority_id != issuer) {
                                continue;
                            }
                            items.push_back(certificate_json(cert));
                        }
                        boost::json::object collection;
                        collection["items"] = std::move(items);
                        json_reply(resp, collection);
                    });

        // ListCertificateVersions
        _server.Get(R"(/20210224/certificates/([^/]+)/versions)",
                    [this](const httplib::Request& req, httplib::Response& resp) {
                        if (!authorized(req, resp)) {
                            return;
                        }
                        const std::lock_guard<std::mutex> lock(_mutex);
                        record_hit("GET /20210224/certificates/{id}/versions");
                        const auto it = _certificates.find(req.matches[1]);
                        if (it == _certificates.end()) {
                            error_reply(resp, 404, "NotAuthorizedOrNotFound",
                                        "certificate not found");
                            return;
                        }
                        boost::json::array items;
                        for (const auto& version : it->second.versions) {
                            items.push_back(version_json(version));
                        }
                        boost::json::object collection;
                        collection["items"] = std::move(items);
                        json_reply(resp, collection);
                    });

        // RevokeCertificateVersion
        _server.Post(
            R"(/20210224/certificates/([^/]+)/versions/([0-9]+)/actions/revoke)",
            [this](const httplib::Request& req, httplib::Response& resp) {
                if (!authorized(req, resp)) {
                    return;
                }
                const std::lock_guard<std::mutex> lock(_mutex);
                record_hit("POST /20210224/certificates/{id}/versions/{n}/actions/revoke");
                const auto it = _certificates.find(req.matches[1]);
                if (it == _certificates.end()) {
                    error_reply(resp, 404, "NotAuthorizedOrNotFound", "certificate not found");
                    return;
                }
                const auto wanted = std::stoll(req.matches[2].str());
                for (auto& version : it->second.versions) {
                    if (version.version_number != wanted) {
                        continue;
                    }
                    if (version.revoked) {
                        // A real OCI double-revoke is an error. The
                        // provider is supposed to never issue one, so
                        // answering 409 here is what makes that claim
                        // testable rather than assumed.
                        error_reply(resp, 409, "IncorrectState",
                                    "certificate version is already revoked");
                        return;
                    }
                    version.revoked = true;
                    resp.status = 204;
                    return;
                }
                error_reply(resp, 404, "NotAuthorizedOrNotFound", "certificate version not found");
            });

        // GetCertificate
        _server.Get(R"(/20210224/certificates/([^/]+))", [this](const httplib::Request& req,
                                                                httplib::Response& resp) {
            if (!authorized(req, resp)) {
                return;
            }
            const std::lock_guard<std::mutex> lock(_mutex);
            record_hit("GET /20210224/certificates/{id}");
            const auto it = _certificates.find(req.matches[1]);
            if (it == _certificates.end()) {
                error_reply(resp, 404, "NotAuthorizedOrNotFound", "certificate not found");
                return;
            }
            // The flip happens *after* this response is composed, so
            // `creating_polls_remaining = N` means exactly N answers
            // of `CREATING` and `ACTIVE` on the N+1th — the reading
            // the field name suggests. Flipping before replying
            // would make N=1 indistinguishable from N=0.
            auto& cert = it->second;
            auto reply = certificate_json(cert);
            if (cert.creating_polls_remaining > 0) {
                --cert.creating_polls_remaining;
                if (cert.creating_polls_remaining == 0) {
                    cert.lifecycle_state = "ACTIVE";
                }
            }
            json_reply(resp, reply);
        });

        // GetCertificateBundle (retrieval plane)
        _server.Get(R"(/20210224/certificateBundles/([^/]+))", [this](const httplib::Request& req,
                                                                      httplib::Response& resp) {
            if (!authorized(req, resp)) {
                return;
            }
            const std::lock_guard<std::mutex> lock(_mutex);
            record_hit("GET /20210224/certificateBundles/{id}");
            const auto it = _certificates.find(req.matches[1]);
            if (it == _certificates.end() || it->second.versions.empty()) {
                error_reply(resp, 404, "NotAuthorizedOrNotFound", "certificate bundle not found");
                return;
            }
            const auto& version = it->second.versions.front();
            boost::json::object bundle;
            bundle["certificateId"] = it->second.id;
            bundle["versionNumber"] = version.version_number;
            bundle["serialNumber"] = version.serial_number;
            bundle["certificatePem"] = "-----BEGIN CERTIFICATE-----\nmock-leaf-" + it->second.id +
                                       "\n-----END CERTIFICATE-----\n";
            bundle["certChainPem"] = _root_pem;
            json_reply(resp, bundle);
        });

        // GetCertificateAuthorityBundle (retrieval plane)
        _server.Get(R"(/20210224/certificateAuthorityBundles/([^/]+))",
                    [this](const httplib::Request& req, httplib::Response& resp) {
                        if (!authorized(req, resp)) {
                            return;
                        }
                        const std::lock_guard<std::mutex> lock(_mutex);
                        record_hit("GET /20210224/certificateAuthorityBundles/{id}");
                        if (req.matches[1] != _ca_id) {
                            error_reply(resp, 404, "NotAuthorizedOrNotFound", "CA not found");
                            return;
                        }
                        boost::json::object bundle;
                        bundle["certificateAuthorityId"] = _ca_id;
                        bundle["certificatePem"] = _root_pem;
                        bundle["certChainPem"] = _root_pem;
                        json_reply(resp, bundle);
                    });
    }

    [[nodiscard]] static auto string_field(const boost::json::object& obj, std::string_view key)
        -> std::string {
        const auto* field = obj.if_contains(key);
        if (field == nullptr || !field->is_string()) {
            return {};
        }
        return std::string(field->get_string());
    }

    [[nodiscard]] static auto instance_json(const instance_state& inst) -> boost::json::value {
        boost::json::object tags;
        for (const auto& [key, value] : inst.freeform_tags) {
            tags[key] = value;
        }
        boost::json::object out;
        out["id"] = inst.id;
        out["lifecycleState"] = inst.lifecycle_state;
        out["availabilityDomain"] = inst.availability_domain;
        out["timeCreated"] = inst.time_created;
        out["freeformTags"] = std::move(tags);
        return out;
    }

    [[nodiscard]] static auto version_json(const certificate_version_state& version)
        -> boost::json::value {
        boost::json::object out;
        out["versionNumber"] = version.version_number;
        out["versionName"] = version.version_name;
        out["serialNumber"] = version.serial_number;
        if (version.revoked) {
            boost::json::object status;
            status["revocationReason"] = "UNSPECIFIED";
            status["timeOfRevocation"] = rfc3339_now();
            out["revocationStatus"] = std::move(status);
        }
        return out;
    }

    [[nodiscard]] static auto certificate_json(const certificate_state& cert)
        -> boost::json::value {
        boost::json::object out;
        out["id"] = cert.id;
        out["name"] = cert.name;
        out["lifecycleState"] = cert.lifecycle_state;
        out["issuerCertificateAuthorityId"] = cert.issuer_certificate_authority_id;
        if (!cert.versions.empty()) {
            out["currentVersion"] = version_json(cert.versions.front());
        }
        return out;
    }

    httplib::Server _server;
    std::thread _thread;
    std::uint16_t _port;
    std::string _availability_domain;

    mutable std::mutex _mutex;
    std::string _pool_id{"ocid1.instancepool.oc1.phx.mock"};
    std::string _ca_id{"ocid1.certificateauthority.oc1.phx.mock"};
    std::string _root_pem{
        "-----BEGIN CERTIFICATE-----\nmock-oci-root\n-----END CERTIFICATE-----\n"};
    std::int64_t _pool_size{0};
    bool _auto_launch{true};
    std::uint64_t _instance_counter{0};
    std::uint64_t _certificate_counter{0};
    int _certificate_creating_polls{0};
    std::vector<std::string> _pool_membership;
    std::map<std::string, instance_state> _instances;
    std::map<std::string, certificate_state> _certificates;
    std::map<std::string, std::size_t> _hits;
    std::vector<std::string> _update_instance_bodies;
};

}  // namespace kythira::testing
