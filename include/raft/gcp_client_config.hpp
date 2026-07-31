#pragma once

/// @file gcp_client_config.hpp
/// @brief Shared GCP connection settings for the Compute Engine / Managed
///        Instance Group quorum managers and the Certificate Authority Service
///        certificate provider, plus the two GCP naming/label validators.
///
/// Compiled unconditionally — pure standard-library types and string/regex
/// logic, no `google-cloud-cpp` dependency — so callers can fill a
/// `gcp_client_config` and validate names/labels on a build without the SDK,
/// exactly as `aws_client_config` is usable without the AWS SDK. This is the
/// GCP analogue of `aws_client_config.hpp`; every GCP component in this spec
/// embeds a `gcp_client_config gcp` field, mirroring how every AWS component
/// embeds an `aws_client_config aws`.

#include <chrono>
#include <string>
#include <string_view>

namespace kythira {

/// @brief Shared GCP connection settings consumed by every GCP component in
///        this spec (`gcp_compute_quorum_manager`, `gcp_mig_quorum_manager`,
///        `gcp_privateca_certificate_provider`).
///
/// An aggregate — no user-declared constructors — so it can be filled with a
/// designated initializer. There is deliberately no `credentials_file_path`
/// field: callers holding a service-account key file read it into
/// `credentials_json` themselves, keeping this struct free of filesystem I/O.
struct gcp_client_config {
    /// GCP project the managed resources live in. Required (non-empty); the
    /// component constructors enforce this.
    std::string project_id;

    /// Inline service-account JSON key. When empty, the implementations use
    /// Application Default Credentials (`MakeGoogleDefaultCredentials()`); when
    /// non-empty, `MakeServiceAccountCredentials(credentials_json)` is used.
    std::string credentials_json;

    /// When non-empty, overrides the REST endpoint. Used to point a GCP
    /// component at a hand-written test double instead of the real service.
    std::string endpoint_override;

    /// Maximum time allowed for a single GCP API call.
    std::chrono::seconds api_timeout{30};

    /// Interval between `zoneOperations.get` polls while waiting for a zone
    /// operation to reach `status == DONE`.
    std::chrono::milliseconds operation_poll_interval{2000};
};

/// @brief Returns `true` iff `value` is a valid GCP resource label key or
///        value: it matches `^[a-z][-a-z0-9_]{0,62}$` (lowercase letter first,
///        then up to 62 more of lowercase letter / digit / `-` / `_`; 63 chars
///        max, no colons, no uppercase — unlike AWS's freeform tag pairs).
[[nodiscard]] inline bool is_valid_gcp_label(std::string_view value) noexcept {
    if (value.empty() || value.size() > 63) {
        return false;
    }
    const char first = value.front();
    if (first < 'a' || first > 'z') {
        return false;
    }
    for (std::size_t i = 1; i < value.size(); ++i) {
        const char c = value[i];
        const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (!ok) {
            return false;
        }
    }
    return true;
}

/// @brief Returns `true` iff `value` is a valid GCE instance-name-shaped
///        DNS-1035 label: it matches `^[a-z]([-a-z0-9]{0,61}[a-z0-9])?$`
///        (lowercase letter first, an alphanumeric last, `-` and digits allowed
///        in between; 63 chars max — note underscores are NOT allowed here,
///        unlike `is_valid_gcp_label`).
[[nodiscard]] inline bool is_valid_gcp_resource_name(std::string_view value) noexcept {
    if (value.empty() || value.size() > 63) {
        return false;
    }
    const char first = value.front();
    if (first < 'a' || first > 'z') {
        return false;
    }
    // A single-character name is valid iff that character is a lowercase letter,
    // which the check above already established.
    if (value.size() == 1) {
        return true;
    }
    const char last = value.back();
    const bool last_alnum = (last >= 'a' && last <= 'z') || (last >= '0' && last <= '9');
    if (!last_alnum) {
        return false;
    }
    for (std::size_t i = 1; i + 1 < value.size(); ++i) {
        const char c = value[i];
        const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
        if (!ok) {
            return false;
        }
    }
    return true;
}

}  // namespace kythira
