// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file alibaba_client_config.hpp
/// @brief Shared Alibaba Cloud connection settings for the ESS quorum manager
///        and the OSS persistence engine.
///
/// Deliberately the same shape as `oci_client_config`/`aws_client_config`: a
/// plain aggregate, no constructor, no validation in the type itself. Which
/// fields are required is the signer's question (`alibaba_signing.hpp`,
/// `.kiro/specs/alibaba-cloud-services/` Requirement 1.1): an aggregate with no
/// invariants can be built up field by field from a config file without passing
/// through an intermediate invalid state.
///
/// **Compiled unconditionally, with no SDK guard** — nothing here comes from an
/// Alibaba SDK, because there is no Alibaba SDK dependency (the spec's
/// load-bearing decision): both the RPC-style control plane and OSS are signed
/// by hand over OpenSSL and sent with httplib, exactly the OCI provider's
/// shape. That is what keeps `vcpkg.json` unchanged for this feature.

#include <chrono>
#include <string>

namespace kythira {

/// @brief Shared Alibaba Cloud connection settings consumed by every Alibaba
///        component (control-plane RPC client and OSS object client alike).
struct alibaba_client_config {
    /// Region ID (e.g. `"cn-hangzhou"`). Used to derive region-qualified
    /// service hosts and OSS's `oss-<region>` endpoint, and folded into the
    /// OSS V4 signing scope, when @ref endpoint_override is empty.
    std::string region;

    /// RAM AccessKeyId — either a long-lived user key or the temporary key
    /// from an STS exchange (in which case @ref security_token must carry the
    /// matching token).
    std::string access_key_id;

    /// The secret matching @ref access_key_id. The secret itself, not a path:
    /// reading files is the caller's job, which keeps this header free of
    /// filesystem access.
    std::string access_key_secret;

    /// STS security token. When non-empty, requests carry it as
    /// `x-acs-security-token` (control plane) / `x-oss-security-token` (OSS),
    /// included in the signed header set. This is how CI's
    /// AssumeRoleWithOIDC-federated credentials arrive (Requirement 17.2) —
    /// the exchange itself happens in the workflow, not in this library.
    std::string security_token;

    /// When non-empty, overrides every derived endpoint at once — control
    /// plane and OSS — pointing all components at one mock server during
    /// tests, the same way `oci_client_config::endpoint_override` does. The
    /// OSS client additionally switches from virtual-host to path-style
    /// addressing under it (Requirement 2.3), so a local mock needs no
    /// wildcard DNS.
    std::string endpoint_override;

    /// Maximum time allowed for a single API call, covering both the connect
    /// and the request phases.
    std::chrono::seconds api_timeout{30};
};

}  // namespace kythira
