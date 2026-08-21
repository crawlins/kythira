// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

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

    /// Maximum time the SDK's own long-running-operation polling loop may spend
    /// waiting for a Compute operation (`instances.insert`/`delete`, MIG
    /// `resize`, CA-pool operations) to finish.
    ///
    /// Deliberately a separate, much larger value than `api_timeout`, and not
    /// derived from it. google-cloud-cpp builds its *default* polling policy
    /// from whatever retry policy the client is configured with, so setting
    /// `api_timeout` as the retry bound silently caps LRO polling at the same
    /// 30s — which is far below what GCE takes to delete an instance. That
    /// exact mistake made `decommission_node` fail against live GCE with
    /// "DeleteInstance() - polling loop terminated by polling policy" on 2 of 3
    /// runs, where the unmodified client passed 2 of 2. The two bounds measure
    /// different things and must be configured separately.
    std::chrono::seconds operation_timeout{900};

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

/// @brief Returns the region a zone belongs to: `us-central1-a` → `us-central1`.
///
/// GCE zone names are the region name plus a `-<suffix>`, so the region is the
/// zone with its final `-`-delimited component removed. A value with no `-` is
/// returned unchanged — it is already region-shaped (or malformed, in which case
/// the API rejects it with a clearer message than anything we could synthesize).
[[nodiscard]] inline std::string gcp_zone_to_region(std::string_view zone) {
    const auto dash = zone.rfind('-');
    if (dash == std::string_view::npos) {
        return std::string{zone};
    }
    return std::string{zone.substr(0, dash)};
}

/// @brief Returns `true` iff `value` is already a resource *reference* — a full
///        URL, a `projects/…` partial URL, or a relative `global/…`-style path —
///        rather than a bare short name.
///
/// GCE accepts any of those reference forms wherever a resource is named, but a
/// bare short name only in some contexts. `/` is the discriminator: no resource
/// short name may contain one (see `is_valid_gcp_resource_name`), so a value
/// containing `/` is a reference and must be passed through untouched.
[[nodiscard]] inline bool is_gcp_resource_reference(std::string_view value) noexcept {
    return value.find('/') != std::string_view::npos;
}

/// @brief Expands a bare VPC network name to the relative reference
///        `global/networks/<name>`; passes any existing reference through.
///
/// `instances.insert` rejects a bare name in `networkInterfaces[].network` with
/// "Invalid value … The URL is malformed", so a short name — which the
/// `network` config field documents as acceptable, and which its `"default"`
/// default value is — must be qualified before the request is built. The
/// relative form (rather than `projects/<p>/global/networks/<n>`) matches how
/// this codebase already references machine types, and resolves against the
/// project in the request URL. Cross-project Shared VPC callers pass a full
/// `projects/<host-project>/global/networks/<n>` reference, which is preserved.
[[nodiscard]] inline std::string gcp_qualify_network(std::string_view network) {
    if (network.empty() || is_gcp_resource_reference(network)) {
        return std::string{network};
    }
    return "global/networks/" + std::string{network};
}

/// @brief Expands a bare subnetwork name to the relative reference
///        `regions/<region>/subnetworks/<name>`, deriving the region from
///        @p zone; passes any existing reference through.
///
/// Subnetworks are regional, so unlike `gcp_qualify_network` this needs the zone
/// the instance is being created in to name the region. See
/// `gcp_qualify_network` for why bare names must be expanded at all.
[[nodiscard]] inline std::string gcp_qualify_subnetwork(std::string_view subnetwork,
                                                        std::string_view zone) {
    if (subnetwork.empty() || is_gcp_resource_reference(subnetwork)) {
        return std::string{subnetwork};
    }
    return "regions/" + gcp_zone_to_region(zone) + "/subnetworks/" + std::string{subnetwork};
}

}  // namespace kythira
