#pragma once

/// @file oci_client_config.hpp
/// @brief Shared OCI connection settings for the Instance Pool quorum manager
///        and the Certificates Management certificate provider.
///
/// Deliberately the same shape as `aws_client_config`: a plain aggregate, no
/// constructor, no validation in the type itself. A reader who knows the AWS
/// config should recognise the OCI equivalent field for field, and the two
/// providers that consume this should be able to take it by value.
///
/// **Compiled unconditionally, with no SDK guard** (Requirement 1.2-1.3), which
/// is the one place this is *not* symmetric with `aws_client_config` — that
/// header wraps its credentials member in `#ifdef KYTHIRA_HAS_AWS_SDK` because
/// the type comes from the AWS SDK. Nothing here comes from an OCI SDK, because
/// there is no OCI SDK dependency: the provider signs its own requests
/// (`oci_signing.hpp`) and sends them over the `httplib` client the tree already
/// has. That is what keeps `vcpkg.json` unchanged for this feature.

#include <chrono>
#include <string>

namespace kythira {

/// @brief Shared OCI connection settings consumed by both OCI providers.
///
/// Which fields are required depends on the auth mode, and that check lives in
/// `oci_signing::sign_request` rather than here (Requirement 1.5): an aggregate
/// with no invariants can be built up field by field from a config file without
/// passing through an intermediate invalid state, and there is exactly one place
/// — the point of signing — where "is this usable?" has a single correct answer.
struct oci_client_config {
    /// OCI region identifier (e.g. `"us-phoenix-1"`). Used to derive the
    /// per-service host (`iaas.{region}.oraclecloud.com` and friends) when
    /// @ref endpoint_override is empty.
    std::string region;

    /// Tenancy OCID. Required unless @ref use_instance_principal.
    std::string tenancy_id;

    /// User OCID. Required unless @ref use_instance_principal.
    std::string user_id;

    /// API-key fingerprint, as shown in the OCI console (colon-separated hex).
    /// Required unless @ref use_instance_principal.
    std::string fingerprint;

    /// PEM-encoded RSA private key matching @ref fingerprint. Required unless
    /// @ref use_instance_principal.
    ///
    /// The key itself, not a path to it. Reading the file is the caller's job,
    /// which keeps this header free of filesystem access and lets a deployment
    /// source the key from somewhere other than a file.
    std::string private_key_pem;

    /// Passphrase for @ref private_key_pem. Empty means the key is unencrypted.
    std::string private_key_passphrase;

    /// Sign with an Instance Principal certificate obtained from the local
    /// instance metadata service instead of the API-key fields above.
    ///
    /// **Not yet implemented** — `oci_signing::instance_principal_signer`
    /// throws. The metadata-service and federation contract is now confirmed
    /// (`.kiro/specs/oci-cloud-provider/spike-notes.md` Finding 16); what is
    /// missing is the federation client itself, not the knowledge to write it.
    bool use_instance_principal{false};

    /// When non-empty, overrides the derived service endpoint host. Points the
    /// providers at a mock server during tests, the same way
    /// `aws_client_config::endpoint_override` points them at LocalStack.
    std::string endpoint_override;

    /// Maximum time allowed for a single OCI API call, covering both the
    /// connect and the request phases.
    std::chrono::seconds api_timeout{30};
};

}  // namespace kythira
