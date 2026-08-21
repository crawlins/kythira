// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

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
/// merged, a detached instance terminates, a certificate becomes ACTIVE.
///
/// **It also verifies request signatures, which an earlier version deliberately
/// did not.** That decision (`tasks.md` Task 4: "a mock that re-derived the
/// signature would be verifying the client against a second copy of the
/// client's own logic") is what let two real defects ship — the client signed
/// one `Host` and sent another, and the endpoint domain was wrong for every
/// service. Both were found only by calling live OCI, and both were invisible
/// to 31 green tests.
///
/// The reasoning it replaces was subtly wrong. Verification here does not
/// re-derive what the client *meant* to sign; it reconstructs the canonical
/// string **from the bytes that actually arrived** — the `Host` header on the
/// request, the raw request-target, the date the request carries — and checks
/// the signature over that with the public key. So it answers a question the
/// golden-vector tests structurally cannot: *does what we sent match what we
/// signed?* That is the axis both defects lived on. A mismatched host, date,
/// path, body hash or method casing now fails here rather than at Oracle.
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

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
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
        /// Remaining `GetInstance` reads that report `PROVISIONING` before this
        /// instance settles into `RUNNING`. While non-zero, `UpdateInstance`
        /// against it answers 409, exactly as OCI does.
        int provisioning_reads_remaining{0};
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

    /// @brief Teach the server which key requests must be signed with.
    ///
    /// Takes the same PEM the client is configured with — a private key, since
    /// that is what a test already has to hand; only its public half is used.
    /// Until this is called the server checks the shape of the `authorization`
    /// header and no more, which is what makes it usable standalone; every test
    /// in this tree calls it, because not calling it is how two real defects
    /// shipped past a green suite.
    auto set_signing_key_pem(const std::string& pem) -> void {
        const std::lock_guard<std::mutex> lock(_mutex);
        std::unique_ptr<BIO, decltype(&BIO_free_all)> bio{
            BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())), &BIO_free_all};
        EVP_PKEY* key = PEM_read_bio_PrivateKey(bio.get(), nullptr, nullptr, nullptr);
        if (key == nullptr) {
            // Try a public key PEM before giving up, so either form works.
            std::unique_ptr<BIO, decltype(&BIO_free_all)> pub_bio{
                BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())), &BIO_free_all};
            key = PEM_read_bio_PUBKEY(pub_bio.get(), nullptr, nullptr, nullptr);
        }
        if (key == nullptr) {
            throw std::runtime_error("oci_mock_server: could not read the signing key PEM");
        }
        _public_key.reset(key);
    }

    /// @brief How long a newly launched instance stays `PROVISIONING`.
    ///
    /// Zero (the default) is Task 4's instantaneous transition. Non-zero models
    /// what live OCI does: the instance appears in the pool listing at once but
    /// is not yet modifiable, and `UpdateInstance` against it answers
    /// `409 Conflict: instance ... is currently being modified`.
    ///
    /// That gap broke the first real `provision_node`, which took the candidate
    /// as soon as it appeared and immediately tried to tag it. A mock that hands
    /// out instances already `RUNNING` cannot express the bug, which is the only
    /// reason this knob exists.
    auto set_provisioning_reads(int reads) -> void {
        const std::lock_guard<std::mutex> lock(_mutex);
        _provisioning_reads = reads;
    }

    /// @brief How many `GetInstancePool` reads report `SCALING` after a size
    ///        change before the pool settles back to `RUNNING`.
    ///
    /// Zero (the default) keeps the pool always `RUNNING`. Non-zero models what
    /// live OCI does after `UpdateInstancePool`, including refusing
    /// `DetachInstancePoolInstance` with `409 IncorrectState: instancepool ...
    /// Must be in State 'Running'` for the duration.
    ///
    /// That window is why a decommission immediately after a provision failed
    /// against real OCI — which is not an exotic sequence, it is exactly what
    /// `maintain_quorum` does when it replaces a node.
    auto set_scaling_reads(int reads) -> void {
        const std::lock_guard<std::mutex> lock(_mutex);
        _scaling_reads = reads;
    }

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
    // ── Direct inspection, bypassing the request log ─────────────────────────
    //
    // The conformance suite counts requests around engine operations, so its
    // *inspection* helpers must not themselves issue any — `h.keys()` is called
    // before `count_requests("LIST")` in several cases. These mirror
    // `mock_object_store`'s `seed`/`keys`/`has`/`body`, which exist for exactly
    // that reason and are documented as bypassing the log.

    auto seed_object(const std::string& bucket, const std::string& key, std::string_view body)
        -> void {
        const std::lock_guard lock(_mutex);
        _objects[bucket + "/" + key] = stored_object{std::string(body), mint_etag()};
    }

    [[nodiscard]] auto object_keys(const std::string& bucket) const -> std::vector<std::string> {
        const std::lock_guard lock(_mutex);
        std::vector<std::string> out;
        for (const auto& [full, _] : _objects) {
            if (full.starts_with(bucket + "/")) {
                out.push_back(full.substr(bucket.size() + 1));
            }
        }
        return out;
    }

    [[nodiscard]] auto object_has(const std::string& bucket, const std::string& key) const -> bool {
        const std::lock_guard lock(_mutex);
        return _objects.contains(bucket + "/" + key);
    }

    [[nodiscard]] auto object_body(const std::string& bucket, const std::string& key) const
        -> std::string {
        const std::lock_guard lock(_mutex);
        const auto it = _objects.find(bucket + "/" + key);
        return it == _objects.end() ? std::string{} : it->second.body;
    }

    /// The object-route request log, by value. Callers must bind it to a local
    /// before iterating — `begin()`/`end()` on two separate calls name two
    /// different vectors, a shape this repo has already been bitten by.
    [[nodiscard]] auto object_requests() const -> std::vector<std::string> {
        const std::lock_guard lock(_mutex);
        return _object_requests;
    }

    [[nodiscard]] auto count_object_requests(std::string_view verb) const -> std::size_t {
        const std::lock_guard lock(_mutex);
        return static_cast<std::size_t>(
            std::count_if(_object_requests.begin(), _object_requests.end(),
                          [verb](const std::string& entry) { return entry.starts_with(verb); }));
    }

    auto clear_object_requests() -> void {
        const std::lock_guard lock(_mutex);
        _object_requests.clear();
    }

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

    /// Extract one `name="value"` parameter from an `authorization` header.
    [[nodiscard]] static auto auth_param(const std::string& header, const std::string& name)
        -> std::string {
        const auto key = name + "=\"";
        const auto start = header.find(key);
        if (start == std::string::npos) {
            return {};
        }
        const auto from = start + key.size();
        const auto end = header.find('"', from);
        if (end == std::string::npos) {
            return {};
        }
        return header.substr(from, end - from);
    }

    /// Rebuild the canonical string **from the received request**.
    ///
    /// Every value here comes off the wire — `Host` as the request carries it,
    /// `req.target` rather than the decoded path, the date the request sent.
    /// That is the whole point: it can disagree with what the client intended
    /// to sign, and when it does, that disagreement is the bug.
    [[nodiscard]] static auto canonical_string_of(const httplib::Request& req,
                                                  const std::string& signed_headers)
        -> std::string {
        std::string out;
        std::istringstream names(signed_headers);
        std::string name;
        bool first = true;
        while (names >> name) {
            std::string value;
            if (name == "(request-target)") {
                std::string method = req.method;
                std::transform(method.begin(), method.end(), method.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                value = method + " " + (req.target.empty() ? req.path : req.target);
            } else {
                value = req.get_header_value(name.c_str());
            }
            if (!first) {
                out += "\n";
            }
            out += name + ": " + value;
            first = false;
        }
        return out;
    }

    [[nodiscard]] static auto base64_decode(const std::string& in) -> std::vector<unsigned char> {
        std::vector<unsigned char> out(((in.size() + 3) / 4) * 3 + 1);
        const int written =
            EVP_DecodeBlock(out.data(), reinterpret_cast<const unsigned char*>(in.data()),
                            static_cast<int>(in.size()));
        if (written < 0) {
            return {};
        }
        // EVP_DecodeBlock reports the padded length; trim the '=' bytes back off
        // or the signature is the right bytes plus one or two zeros, which
        // verifies as false and looks like a signing bug rather than a decode one.
        std::size_t len = static_cast<std::size_t>(written);
        for (auto it = in.rbegin(); it != in.rend() && *it == '='; ++it) {
            --len;
        }
        out.resize(len);
        return out;
    }

    /// The `authorization` header must be present, well formed, **and verify**.
    auto authorized(const httplib::Request& req, httplib::Response& resp) const -> bool {
        const auto header = req.get_header_value("authorization");
        if (!header.starts_with("Signature ")) {
            error_reply(resp, 401, "NotAuthenticated",
                        "request carried no OCI Request Signing v1 authorization header");
            return false;
        }
        if (_public_key == nullptr) {
            // No key configured: shape check only, so the server stays usable
            // standalone. Every test in this tree sets one.
            return true;
        }

        const auto signed_headers = auth_param(header, "headers");
        const auto signature_b64 = auth_param(header, "signature");
        if (signed_headers.empty() || signature_b64.empty()) {
            error_reply(resp, 401, "NotAuthenticated", "authorization header is malformed");
            return false;
        }
        const auto signature = base64_decode(signature_b64);
        const auto canonical = canonical_string_of(req, signed_headers);

        EVP_MD_CTX* ctx = EVP_MD_CTX_new();
        bool ok = false;
        if (ctx != nullptr) {
            if (EVP_DigestVerifyInit(ctx, nullptr, EVP_sha256(), nullptr, _public_key.get()) == 1) {
                ok = EVP_DigestVerify(ctx, signature.data(), signature.size(),
                                      reinterpret_cast<const unsigned char*>(canonical.data()),
                                      canonical.size()) == 1;
            }
            EVP_MD_CTX_free(ctx);
        }
        if (!ok) {
            // Echo the string the *server* built. When this fires, the
            // difference between it and what the client signed is the defect,
            // and printing it is the difference between a five-minute fix and
            // the afternoon a bare 401 costs.
            error_reply(resp, 401, "NotAuthenticated",
                        "Failed to verify the HTTP(S) Signature; server rebuilt this canonical "
                        "string from the received request:\n" +
                            canonical);
            return false;
        }
        return true;
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
        inst.provisioning_reads_remaining = _provisioning_reads;
        if (inst.provisioning_reads_remaining > 0) {
            inst.lifecycle_state = "PROVISIONING";
        }
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

    // ── Object Storage (task 15.4) ───────────────────────────────────────────
    //
    // Extended into this server rather than duplicated into a new one, so the
    // object routes inherit the signature verification the control-plane routes
    // already have: this mock checks the client's signature against the bytes
    // that **arrived**, with the public key, so a client that signs the wrong
    // canonical form fails here rather than only against the real service.
    //
    // What that check can and cannot prove is worth stating, because task 16
    // found two OCI defects this tier is structurally unable to catch. It
    // verifies the signature *this* server reconstructs — so a client and mock
    // that encode a request identically always agree, however wrongly they both
    // encode it. It also never exercises endpoint derivation, because
    // `endpoint_override` replaces the host outright. Those two blind spots are
    // exactly where the real defects lived.
    auto install_object_storage_routes() -> void {
        // `GET /n/` — the namespace, resolved once at client construction.
        _server.Get("/n/", [this](const httplib::Request& req, httplib::Response& res) {
            if (!authorized(req, res)) {
                return;
            }
            const std::lock_guard lock(_mutex);
            res.status = 200;
            res.set_content("\"" + _namespace + "\"", "application/json");
        });

        const std::string object_route = R"(/n/([^/]+)/b/([^/]+)/o/(.+))";

        _server.Put(object_route, [this](const httplib::Request& req, httplib::Response& res) {
            if (!authorized(req, res)) {
                return;
            }
            const std::string key = decode(req.matches[3]);
            const std::lock_guard lock(_mutex);
            _object_requests.push_back("PUT " + key);
            const std::string full = std::string(req.matches[2]) + "/" + key;

            // Conditional writes. OCI spells a lost create-only precondition
            // `412 IfNoneMatchFailed` and a lost overwrite `412
            // PreconditionFailed`; both latch, and both are 412, which is the
            // shape the client's mapping is written against.
            const auto if_none_match = req.get_header_value("if-none-match");
            const auto if_match = req.get_header_value("if-match");
            const auto existing = _objects.find(full);
            if (if_none_match == "*" && existing != _objects.end()) {
                oci_error(res, 412, "IfNoneMatchFailed", "the object already exists");
                return;
            }
            if (!if_match.empty()) {
                if (existing == _objects.end() || existing->second.etag != if_match) {
                    oci_error(res, 412, "PreconditionFailed", "the ETag does not match");
                    return;
                }
            }

            const std::string etag = mint_etag();
            _objects[full] = stored_object{req.body, etag};
            res.status = 200;
            // Lowercased deliberately: OCI spells it `etag`, and task 0's probe
            // looked up `ETag`, got nothing, and misread the run as "OCI cannot
            // do CAS". The client lowercases every response header for that
            // reason; serving it lowercase here keeps the mock honest to the
            // service rather than to the client.
            res.set_header("etag", etag);
        });

        _server.Get(object_route, [this](const httplib::Request& req, httplib::Response& res) {
            if (!authorized(req, res)) {
                return;
            }
            const std::string key = decode(req.matches[3]);
            const std::lock_guard lock(_mutex);
            _object_requests.push_back("GET " + key);
            const auto it = _objects.find(std::string(req.matches[2]) + "/" + key);
            if (it == _objects.end()) {
                oci_error(res, 404, "ObjectNotFound", "no such object");
                return;
            }
            res.status = 200;
            res.set_header("etag", it->second.etag);
            res.set_content(it->second.body, "application/octet-stream");
        });

        _server.Delete(object_route, [this](const httplib::Request& req, httplib::Response& res) {
            if (!authorized(req, res)) {
                return;
            }
            const std::string key = decode(req.matches[3]);
            const std::lock_guard lock(_mutex);
            _object_requests.push_back("DELETE " + key);
            const std::string full = std::string(req.matches[2]) + "/" + key;
            const auto it = _objects.find(full);
            const auto if_match = req.get_header_value("if-match");
            if (!if_match.empty() && (it == _objects.end() || it->second.etag != if_match)) {
                oci_error(res, 412, "PreconditionFailed", "the ETag does not match");
                return;
            }
            if (it == _objects.end()) {
                oci_error(res, 404, "ObjectNotFound", "no such object");
                return;
            }
            _objects.erase(it);
            res.status = 204;
        });

        // Listing. `prefix`, `limit` and `start` are the three the client uses;
        // `nextStartWith` drives its pagination loop.
        _server.Get(R"(/n/([^/]+)/b/([^/]+)/o)", [this](const httplib::Request& req,
                                                        httplib::Response& res) {
            if (!authorized(req, res)) {
                return;
            }
            const std::string bucket(req.matches[2]);
            const std::string prefix = req.get_param_value("prefix");
            const std::string start = req.get_param_value("start");
            std::size_t limit = 1000;
            if (req.has_param("limit")) {
                limit = static_cast<std::size_t>(std::stoul(req.get_param_value("limit")));
            }
            const std::lock_guard lock(_mutex);
            _object_requests.push_back("LIST " + prefix);

            std::string objects;
            std::string next;
            std::size_t emitted = 0;
            for (const auto& [full, obj] : _objects) {
                if (!full.starts_with(bucket + "/")) {
                    continue;
                }
                const std::string key = full.substr(bucket.size() + 1);
                if (!key.starts_with(prefix) || (!start.empty() && key < start)) {
                    continue;
                }
                if (emitted == limit) {
                    next = key;  // where the next page resumes
                    break;
                }
                objects += (emitted == 0 ? "" : ",");
                objects += R"({"name":")" + json_escape(key) + R"("})";
                ++emitted;
            }
            std::string body = R"({"objects":[)" + objects + "]";
            if (!next.empty()) {
                body += R"(,"nextStartWith":")" + json_escape(next) + R"(")";
            }
            body += "}";
            res.status = 200;
            res.set_content(body, "application/json");
        });
    }

    [[nodiscard]] auto mint_etag() -> std::string {
        // Shaped like the UUID OCI returns, and opaque: nothing about it is
        // derivable from the content.
        return "mock-etag-" + std::to_string(++_etag_counter);
    }

    static auto oci_error(httplib::Response& res, int status, const char* code, const char* message)
        -> void {
        res.status = status;
        res.set_content(std::string(R"({"code":")") + code + R"(","message":")" + message + R"("})",
                        "application/json");
    }

    [[nodiscard]] static auto decode(const std::string& value) -> std::string {
        std::string out;
        for (std::size_t i = 0; i < value.size(); ++i) {
            if (value[i] == '%' && i + 2 < value.size()) {
                out.push_back(static_cast<char>(std::stoi(value.substr(i + 1, 2), nullptr, 16)));
                i += 2;
            } else {
                out.push_back(value[i]);
            }
        }
        return out;
    }

    [[nodiscard]] static auto json_escape(const std::string& value) -> std::string {
        std::string out;
        for (const char c : value) {
            if (c == '"' || c == '\\') {
                out.push_back('\\');
            }
            out.push_back(c);
        }
        return out;
    }

    auto install_routes() -> void {
        install_object_storage_routes();
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
            // Composed before the countdown ticks, so `reads = N` yields
            // exactly N answers of SCALING.
            pool["lifecycleState"] = _scaling_reads_remaining > 0 ? "SCALING" : "RUNNING";
            if (_scaling_reads_remaining > 0) {
                --_scaling_reads_remaining;
            }
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
            // A size change puts the pool into SCALING, exactly as OCI does —
            // and that is what refuses a detach until it settles.
            _scaling_reads_remaining = _scaling_reads;
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
                if (_scaling_reads_remaining > 0) {
                    error_reply(resp, 409, "IncorrectState",
                                "instancepool " + _pool_id + " is Must be in State 'Running'");
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
                        // Composed before the countdown ticks, so `reads = N`
                        // means exactly N answers of PROVISIONING.
                        auto reply = instance_json(it->second);
                        if (it->second.provisioning_reads_remaining > 0) {
                            --it->second.provisioning_reads_remaining;
                            if (it->second.provisioning_reads_remaining == 0) {
                                it->second.lifecycle_state = "RUNNING";
                            }
                        }
                        json_reply(resp, reply);
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
            if (it->second.provisioning_reads_remaining > 0) {
                error_reply(
                    resp, 409, "Conflict",
                    "instance " + it->second.id + " is currently being modified, try again later");
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

    struct evp_pkey_deleter {
        void operator()(EVP_PKEY* p) const noexcept {
            if (p != nullptr) {
                EVP_PKEY_free(p);
            }
        }
    };

    httplib::Server _server;
    std::thread _thread;
    std::uint16_t _port;
    std::string _availability_domain;
    std::unique_ptr<EVP_PKEY, evp_pkey_deleter> _public_key;

    mutable std::mutex _mutex;
    std::string _pool_id{"ocid1.instancepool.oc1.phx.mock"};
    std::string _ca_id{"ocid1.certificateauthority.oc1.phx.mock"};
    std::string _root_pem{
        "-----BEGIN CERTIFICATE-----\nmock-oci-root\n-----END CERTIFICATE-----\n"};
    std::int64_t _pool_size{0};
    bool _auto_launch{true};
    int _provisioning_reads{0};
    int _scaling_reads{0};
    int _scaling_reads_remaining{0};
    std::uint64_t _instance_counter{0};
    std::uint64_t _certificate_counter{0};
    int _certificate_creating_polls{0};
    std::vector<std::string> _pool_membership;
    /// Object Storage's keyspace (task 15.4). One flat map, because that is
    /// what the service is: `bucket/key` -> bytes plus the ETag OCI mints.
    ///
    /// **OCI's ETag is a UUID**, not a content digest (spike-notes Finding 13),
    /// so this mints an opaque counter rather than an MD5. A mock that returned
    /// a digest here would let a client that wrongly assumed
    /// `version_is_content_md5` pass — and `oci_object_storage_client`
    /// static_asserts the negative precisely because that assumption is wrong
    /// for this provider.
    struct stored_object {
        std::string body;
        std::string etag;
    };
    std::map<std::string, stored_object> _objects;
    /// `"PUT <key>"` / `"GET <key>"` / `"DELETE <key>"` / `"LIST <prefix>"`, in
    /// order — the same shape `mock_object_store` records, so the conformance
    /// suite's request-counting cases read identically over both substrates.
    std::vector<std::string> _object_requests;
    std::uint64_t _etag_counter{0};
    std::string _namespace{"axunmw4f0mln"};

    std::map<std::string, instance_state> _instances;
    std::map<std::string, certificate_state> _certificates;
    std::map<std::string, std::size_t> _hits;
    std::vector<std::string> _update_instance_bodies;
};

}  // namespace kythira::testing
