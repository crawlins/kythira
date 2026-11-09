// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file iam_auth.hpp
/// @brief The composition root's IAM side (.kiro/specs/iam-compatible-auth/
///        task 21): built only with CONFIG_REDIS_GATEWAY_IAM, and the only
///        place redis_gateway_node meets the IAM module.
///
/// `iam_auth.cpp` holds the embedded IAM replica and IAM's
/// `credential_verifier`; everything else in the daemon sees only core's
/// `credential_verifier_fn`. The gateway itself selects which users the
/// verifier handles from the ACL's `auth=` attribute.

#include <raft/credential_verifier.hpp>

#include <memory>
#include <string>

namespace kythira::redis_node {

class iam_auth {
public:
    iam_auth() = default;
    iam_auth(const iam_auth&) = delete;
    auto operator=(const iam_auth&) -> iam_auth& = delete;
    iam_auth(iam_auth&&) = delete;
    auto operator=(iam_auth&&) -> iam_auth& = delete;
    virtual ~iam_auth() = default;

    /// The verifier for `redis_gateway_config::_credential_verifier`. It
    /// refers to this object, which must outlive the gateway.
    [[nodiscard]] virtual auto verifier() -> credential_verifier_fn = 0;
};

/// Reads the KYTHIRA_IAM_* variables and starts the IAM replica. Returns
/// nullptr when KYTHIRA_IAM_VOTERS is unset: IAM authentication is off and
/// the daemon behaves as a build without it.
/// @throws std::invalid_argument naming a missing or malformed variable.
[[nodiscard]] auto start_iam_auth_from_env() -> std::unique_ptr<iam_auth>;

/// The KYTHIRA_IAM_* section of `--help`.
[[nodiscard]] auto iam_auth_usage() -> std::string;

}  // namespace kythira::redis_node
