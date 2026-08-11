#pragma once

/// @file oci_certificates_provider.hpp
/// @brief `certificate_provider` backed by OCI Certificates Management
///        (`.kiro/specs/oci-cloud-provider/`, Requirement 12).
///
/// The OCI analogue of `aws_acm_pca_provider`, and — importantly — the same
/// shape, not a compromise on it. OCI's default issuance flow
/// (`ISSUED_BY_INTERNAL_CA`) generates the key pair itself and stores the
/// private key in Vault, which would have broken `certificate_provider.hpp`'s
/// standing invariant that the CA never sees a private key. It turns out there
/// is a third config type, `MANAGED_EXTERNALLY_ISSUED_BY_INTERNAL_CA`, that
/// takes a caller-supplied CSR and nothing else (confirmed in `spike-notes.md`
/// Finding 3), so `sign_csr` here is the direct counterpart of ACM Private CA's
/// `IssueCertificate(CSR)` and `private_key_pem` comes back empty for the same
/// reason it does everywhere else in this tree: there was never a key to return.
///
/// Two OCI *services* are involved, which is why `oci_http_client::request`
/// takes the service per call. The management plane
/// (`certificatesmanagement.{region}...`) creates, reads and revokes; the
/// retrieval plane (`certificates.{region}...`) is where bundles — the actual
/// PEM — are read from. Sending either call to the other host is a 404 with
/// nothing to suggest why.
///
/// Does not create or delete the Certificate Authority resource. Provisioning a
/// CA is a billed, ongoing commitment and stays an out-of-band operator action,
/// exactly as `aws_acm_pca_provider` treats the ACM Private CA.

#include <raft/certificate_provider.hpp>
#include <raft/fault_injection.hpp>
#include <raft/future_default.hpp>
#include <raft/oci_client_config.hpp>
#include <raft/oci_http_client.hpp>

#include <boost/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <optional>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace raft::testing {

namespace oci_certificates_detail {

/// Format `when` as the RFC 3339 UTC stamp OCI's `validity` field wants.
///
/// Built from the broken-down time by hand rather than through `strftime`, for
/// the same reason `oci_signing::rfc1123_now` is: `%b`-style conversions are
/// locale-sensitive, and a machine whose only difference is `$LC_TIME` would
/// emit a stamp the service rejects.
[[nodiscard]] inline auto rfc3339_utc(std::time_t when) -> std::string {
    std::tm tm_utc{};
#if defined(_WIN32)
    gmtime_s(&tm_utc, &when);
#else
    gmtime_r(&when, &tm_utc);
#endif
    std::array<char, 32> buf{};
    const int written = std::snprintf(buf.data(), buf.size(), "%04d-%02d-%02dT%02d:%02d:%02dZ",
                                      tm_utc.tm_year + 1900, tm_utc.tm_mon + 1, tm_utc.tm_mday,
                                      tm_utc.tm_hour, tm_utc.tm_min, tm_utc.tm_sec);
    if (written <= 0) {
        throw std::runtime_error("oci_certificates_provider: failed to format a validity stamp");
    }
    return std::string(buf.data(), static_cast<std::size_t>(written));
}

/// Reduce an arbitrary identifier to something usable as an OCI resource name.
///
/// OCI certificate names are restricted and must be unique within the
/// compartment; a DNS name with dots in it is not one. Anything outside
/// `[A-Za-z0-9_-]` becomes `-`, and an empty result falls back to a fixed stem
/// so the caller never ends up sending an empty `name`.
[[nodiscard]] inline auto sanitize_name(std::string_view raw) -> std::string {
    std::string out;
    out.reserve(raw.size());
    for (const char c : raw) {
        const auto uc = static_cast<unsigned char>(c);
        out.push_back((std::isalnum(uc) != 0 || c == '-' || c == '_') ? c : '-');
    }
    if (out.empty()) {
        return "kythira-node";
    }
    return out;
}

[[nodiscard]] inline auto json_string(const boost::json::value& v, std::string_view key)
    -> std::string {
    const auto* obj = v.if_object();
    if (obj == nullptr) {
        return {};
    }
    const auto* field = obj->if_contains(key);
    if (field == nullptr || !field->is_string()) {
        return {};
    }
    return std::string(field->get_string());
}

/// Read an integral field, or `fallback` when it is absent or not a number.
///
/// Both JSON integer representations are accepted because which one arrives is
/// a property of the value, not of the field: `boost::json` parses a small
/// positive number as `uint64` and the same field could arrive as `int64` from
/// a different response.
[[nodiscard]] inline auto json_int(const boost::json::value& v, std::string_view key,
                                   std::int64_t fallback = 0) -> std::int64_t {
    const auto* obj = v.if_object();
    if (obj == nullptr) {
        return fallback;
    }
    const auto* field = obj->if_contains(key);
    if (field == nullptr) {
        return fallback;
    }
    if (field->is_int64()) {
        return field->get_int64();
    }
    if (field->is_uint64()) {
        return static_cast<std::int64_t>(field->get_uint64());
    }
    return fallback;
}

}  // namespace oci_certificates_detail

/// @brief Configuration for @ref oci_certificates_provider (Requirement 12.4).
struct oci_certificates_provider_config {
    /// Credentials, region and signing mode.
    kythira::oci_client_config oci{};
    /// Compartment OCID the certificate resources are created in. Required.
    std::string compartment_id;
    /// OCID of a pre-existing OCI Certificate Authority. Required.
    std::string certificate_authority_id;
    /// Default certificate validity, used when the per-call
    /// `csr_signing_options::validity` is zero. A caller that sets the option
    /// wins; this is the deployment-wide default behind it.
    std::chrono::seconds validity{std::chrono::hours(24 * 30)};
    /// Interval between `GetCertificate` polls while issuance completes.
    std::chrono::milliseconds poll_interval{1000};
};

/// @brief `certificate_provider` issuing from an OCI Certificate Authority.
///
/// Not copyable or movable — it owns a `std::mutex` guarding the cached root.
/// The concept requires neither, and `aws_acm_pca_provider` is in the same
/// position for the same reason.
class oci_certificates_provider {
public:
    explicit oci_certificates_provider(oci_certificates_provider_config config)
        : _config(std::move(config)), _http(_config.oci) {
        if (_config.compartment_id.empty()) {
            throw std::invalid_argument(
                "oci_certificates_provider: compartment_id must be non-empty");
        }
        if (_config.certificate_authority_id.empty()) {
            throw std::invalid_argument(
                "oci_certificates_provider: certificate_authority_id must be non-empty");
        }
    }

    oci_certificates_provider(const oci_certificates_provider&) = delete;
    auto operator=(const oci_certificates_provider&) -> oci_certificates_provider& = delete;
    oci_certificates_provider(oci_certificates_provider&&) = delete;
    auto operator=(oci_certificates_provider&&) -> oci_certificates_provider& = delete;
    ~oci_certificates_provider() = default;

    /// @brief The CA's own certificate, cached after the first success
    ///        (Requirement 12.5).
    ///
    /// Cached because it does not change and every node bootstrapping against
    /// this CA asks for it — the same reason `aws_acm_pca_provider` caches it.
    /// Only a *successful* fetch populates the cache, so a transient failure
    /// does not poison it.
    [[nodiscard]] auto root_certificate_pem() -> kythira::future_default<std::string> {
        try {
            {
                const std::lock_guard<std::mutex> lock(_mutex);
                if (_cached_root_pem.has_value()) {
                    return kythira::future_factory_default::makeReadyFuture(*_cached_root_pem);
                }
            }

            fiu_do_on("raft/oci/certificates/get_bundle",
                      throw std::runtime_error("fault: raft/oci/certificates/get_bundle"););

            const auto bundle = _http.request(
                "certificates", "GET",
                "/20210224/certificateAuthorityBundles/" + _config.certificate_authority_id);
            auto pem = oci_certificates_detail::json_string(bundle, "certificatePem");
            if (pem.empty()) {
                throw std::runtime_error(
                    "GetCertificateAuthorityBundle returned no certificatePem for " +
                    _config.certificate_authority_id);
            }

            {
                const std::lock_guard<std::mutex> lock(_mutex);
                _cached_root_pem = pem;
            }
            return kythira::future_factory_default::makeReadyFuture(std::move(pem));
        } catch (const std::exception& ex) {
            return kythira::future_factory_default::makeExceptionalFuture<std::string>(
                std::make_exception_ptr(std::runtime_error(
                    std::string("oci_certificates_provider::root_certificate_pem: ") + ex.what())));
        }
    }

    /// @brief Submits @p csr_pem for issuance and waits for the certificate
    ///        (Requirement 12.2).
    ///
    /// The CSR is authoritative for subject and SANs, because that is what
    /// `MANAGED_EXTERNALLY_ISSUED_BY_INTERNAL_CA` means: the config details
    /// carry `issuerCertificateAuthorityId` and `csrPem` and nothing about the
    /// name being certified. `options` therefore contributes the validity window
    /// and a display name for the certificate *resource* — it is not, and cannot
    /// be, a second source of truth for the identity. (Requirement 12.2 says
    /// `options`' subject/SAN populate `CreateCertificate`'s `subject`/
    /// `certificateRules`; those fields belong to the internally-generated
    /// config type, not this one. See `tasks.md`'s status block.)
    ///
    /// @return `pem_material` with `private_key_pem` empty — the key never left
    ///         the caller.
    [[nodiscard]] auto sign_csr(std::string csr_pem, csr_signing_options options)
        -> kythira::future_default<pem_material> {
        try {
            fiu_do_on("raft/oci/certificates/create_certificate",
                      throw std::runtime_error("fault: raft/oci/certificates/create_certificate"););

            if (csr_pem.empty()) {
                throw std::invalid_argument("sign_csr: csr_pem must be non-empty");
            }

            const auto validity =
                options.validity.count() > 0 ? options.validity : _config.validity;

            boost::json::object config_details;
            config_details["configType"] = "MANAGED_EXTERNALLY_ISSUED_BY_INTERNAL_CA";
            config_details["issuerCertificateAuthorityId"] = _config.certificate_authority_id;
            config_details["csrPem"] = csr_pem;
            boost::json::object validity_obj;
            validity_obj["timeOfValidityNotAfter"] = oci_certificates_detail::rfc3339_utc(
                std::time(nullptr) + static_cast<std::time_t>(validity.count()));
            config_details["validity"] = std::move(validity_obj);

            boost::json::object create;
            create["compartmentId"] = _config.compartment_id;
            create["name"] = next_certificate_name(options);
            create["certificateConfig"] = std::move(config_details);

            const auto created =
                _http.request("certificatesmanagement", "POST", "/20210224/certificates",
                              boost::json::serialize(create));
            const auto certificate_id = oci_certificates_detail::json_string(created, "id");
            if (certificate_id.empty()) {
                throw std::runtime_error("CreateCertificate returned no certificate id");
            }

            const auto version_number = await_active_version(certificate_id);

            fiu_do_on("raft/oci/certificates/get_bundle",
                      throw std::runtime_error("fault: raft/oci/certificates/get_bundle"););

            const auto bundle =
                _http.request("certificates", "GET",
                              "/20210224/certificateBundles/" + certificate_id +
                                  "?certificateVersionNumber=" + std::to_string(version_number));
            auto certificate_pem = oci_certificates_detail::json_string(bundle, "certificatePem");
            if (certificate_pem.empty()) {
                throw std::runtime_error("GetCertificateBundle returned no certificatePem for " +
                                         certificate_id);
            }

            return kythira::future_factory_default::makeReadyFuture(pem_material{
                .certificate_pem = std::move(certificate_pem),
                .private_key_pem = {},
                .chain_pem = oci_certificates_detail::json_string(bundle, "certChainPem"),
                .serial =
                    parse_serial(oci_certificates_detail::json_string(bundle, "serialNumber")),
            });
        } catch (const std::exception& ex) {
            return kythira::future_factory_default::makeExceptionalFuture<pem_material>(
                std::make_exception_ptr(std::runtime_error(
                    std::string("oci_certificates_provider::sign_csr: ") + ex.what())));
        }
    }

    /// @brief Revokes the certificate version carrying @p certificate_serial
    ///        (Requirement 12.6).
    ///
    /// `RevokeCertificateVersion`, not `ScheduleCertificateDeletion` — the
    /// latter deletes the certificate *resource*, which is a different operation
    /// with a different blast radius (`spike-notes.md` Finding 4).
    ///
    /// OCI addresses a version by `(certificate OCID, version number)`, and a
    /// serial is neither, so this searches: the CA's certificates, then each
    /// one's versions, for a matching `serialNumber`. That costs API calls, and
    /// the alternative — caching what `sign_csr` issued — would work only until
    /// the process restarted, which is precisely when a revocation is most
    /// likely to be needed.
    ///
    /// Idempotent: an unmatched serial, or a version already `REVOKED`, resolves
    /// successfully without a second revoke call.
    [[nodiscard]] auto revoke(const std::string& certificate_serial)
        -> kythira::future_default<void> {
        try {
            fiu_do_on("raft/oci/certificates/revoke",
                      throw std::runtime_error("fault: raft/oci/certificates/revoke"););

            if (certificate_serial.empty()) {
                throw std::invalid_argument("revoke: certificate_serial must be non-empty");
            }

            const auto found = find_version_by_serial(certificate_serial);
            if (!found.has_value()) {
                return kythira::future_factory_default::makeFuture();
            }
            if (found->already_revoked) {
                return kythira::future_factory_default::makeFuture();
            }

            // The version is addressed by the path; the body carries only why.
            // `UNSPECIFIED` because this call is invoked on decommission, and
            // inventing `KEY_COMPROMISE` or `SUPERSEDED` would put a claim into
            // the CRL that the caller never made.
            boost::json::object body;
            body["revocationReason"] = "UNSPECIFIED";
            (void)_http.request("certificatesmanagement", "POST",
                                "/20210224/certificates/" + found->certificate_id + "/versions/" +
                                    std::to_string(found->version_number) + "/actions/revoke",
                                boost::json::serialize(body));
            return kythira::future_factory_default::makeFuture();
        } catch (const std::exception& ex) {
            return kythira::future_factory_default::makeExceptionalFuture<void>(
                std::make_exception_ptr(std::runtime_error(
                    std::string("oci_certificates_provider::revoke: ") + ex.what())));
        }
    }

private:
    struct located_version {
        std::string certificate_id;
        std::int64_t version_number{0};
        bool already_revoked{false};
    };

    /// A display name for the certificate *resource*, unique within the
    /// compartment.
    ///
    /// The epoch second plus a per-instance counter, because OCI rejects a
    /// duplicate name outright and two nodes bootstrapping in the same second
    /// with the same DNS name is an ordinary occurrence, not an edge case.
    [[nodiscard]] auto next_certificate_name(const csr_signing_options& options) -> std::string {
        std::string stem = "kythira-node";
        if (!options.dns_names.empty()) {
            stem = oci_certificates_detail::sanitize_name(options.dns_names.front());
        } else if (!options.ip_addresses.empty()) {
            stem = oci_certificates_detail::sanitize_name(options.ip_addresses.front());
        }
        return stem + "-" + std::to_string(static_cast<long long>(std::time(nullptr))) + "-" +
               std::to_string(_name_counter.fetch_add(1));
    }

    /// Poll `GetCertificate` until it is `ACTIVE`, returning the current version
    /// number.
    ///
    /// Bounded by `oci.api_timeout`, which is also the per-HTTP-call timeout —
    /// deliberately the same knob rather than a second one, since the question a
    /// caller is answering in both cases is "how long am I willing to wait for
    /// OCI". A `FAILED` state aborts immediately: waiting out the timeout on a
    /// certificate OCI has already given up on tells the caller nothing and
    /// hides the reason.
    [[nodiscard]] auto await_active_version(const std::string& certificate_id) -> std::int64_t {
        const auto deadline = std::chrono::steady_clock::now() + _config.oci.api_timeout;
        std::string last_state;
        for (;;) {
            const auto current = _http.request("certificatesmanagement", "GET",
                                               "/20210224/certificates/" + certificate_id);
            last_state = oci_certificates_detail::json_string(current, "lifecycleState");
            if (last_state == "ACTIVE") {
                const auto* obj = current.if_object();
                const auto* version = obj != nullptr ? obj->if_contains("currentVersion") : nullptr;
                // Version numbers start at 1, so 0 is unambiguously "absent"
                // and does not need a separate sentinel.
                const auto number = version != nullptr ? oci_certificates_detail::json_int(
                                                             *version, "versionNumber")
                                                       : 0;
                if (number > 0) {
                    return number;
                }
                throw std::runtime_error("certificate " + certificate_id +
                                         " is ACTIVE but reported no currentVersion.versionNumber");
            }
            if (last_state == "FAILED" || last_state == "DELETED") {
                throw std::runtime_error("certificate " + certificate_id +
                                         " entered lifecycleState " + last_state);
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                throw std::runtime_error("timed out after " +
                                         std::to_string(_config.oci.api_timeout.count()) +
                                         "s waiting for certificate " + certificate_id +
                                         " to become ACTIVE (last state: " + last_state + ")");
            }
            std::this_thread::sleep_for(_config.poll_interval);
        }
    }

    [[nodiscard]] auto find_version_by_serial(const std::string& serial) const
        -> std::optional<located_version> {
        const auto certificates =
            _http.request("certificatesmanagement", "GET",
                          "/20210224/certificates?compartmentId=" + _config.compartment_id +
                              "&issuerCertificateAuthorityId=" + _config.certificate_authority_id);
        const auto* items = collection_items(certificates);
        if (items == nullptr) {
            return std::nullopt;
        }
        for (const auto& certificate : *items) {
            const auto certificate_id = oci_certificates_detail::json_string(certificate, "id");
            if (certificate_id.empty()) {
                continue;
            }
            const auto versions =
                _http.request("certificatesmanagement", "GET",
                              "/20210224/certificates/" + certificate_id + "/versions");
            const auto* version_items = collection_items(versions);
            if (version_items == nullptr) {
                continue;
            }
            for (const auto& version : *version_items) {
                if (oci_certificates_detail::json_string(version, "serialNumber") != serial) {
                    continue;
                }
                located_version out;
                out.certificate_id = certificate_id;
                out.version_number = oci_certificates_detail::json_int(version, "versionNumber");
                // A revoked version carries a `revocationStatus` object; an
                // active one omits it. Presence is the signal rather than any
                // particular reason code, since every reason means the same
                // thing to this call — there is nothing left to do.
                if (const auto* obj = version.if_object(); obj != nullptr) {
                    if (const auto* status = obj->if_contains("revocationStatus");
                        status != nullptr && !status->is_null()) {
                        out.already_revoked = true;
                    }
                }
                return out;
            }
        }
        return std::nullopt;
    }

    /// OCI list operations answer with either a bare array or a
    /// `{"items": [...]}` collection depending on the service. Both shapes are
    /// accepted rather than one being assumed, because guessing wrong yields an
    /// empty result that looks exactly like "nothing matched".
    [[nodiscard]] static auto collection_items(const boost::json::value& value)
        -> const boost::json::array* {
        if (const auto* arr = value.if_array(); arr != nullptr) {
            return arr;
        }
        if (const auto* obj = value.if_object(); obj != nullptr) {
            if (const auto* items = obj->if_contains("items"); items != nullptr) {
                return items->if_array();
            }
        }
        return nullptr;
    }

    /// OCI serials are decimal strings that may exceed 64 bits (X.509 allows 20
    /// octets). `pem_material::serial` is a `std::uint64_t`, so anything that
    /// does not fit comes back as 0 rather than as a truncated value that would
    /// silently match the wrong certificate. `revoke` takes the string form for
    /// exactly this reason.
    [[nodiscard]] static auto parse_serial(const std::string& raw) -> std::uint64_t {
        if (raw.empty()) {
            return 0;
        }
        try {
            std::size_t consumed = 0;
            const auto value = std::stoull(raw, &consumed);
            return consumed == raw.size() ? value : 0;
        } catch (const std::exception&) {
            return 0;
        }
    }

    oci_certificates_provider_config _config;
    kythira::oci_http_client _http;
    std::mutex _mutex;
    std::optional<std::string> _cached_root_pem;
    std::atomic<std::uint64_t> _name_counter{0};
};

static_assert(certificate_provider<oci_certificates_provider>);

}  // namespace raft::testing
