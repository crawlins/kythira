// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file alibaba_client_config_env.hpp
/// @brief Builds an `alibaba_client_config` from `KYTHIRA_ALIBABA_*`
///        environment variables
///        (`.kiro/specs/object-backup-oci-oss-credentials/`, Requirements 2
///        and 3).
///
/// The names are the ones `tests/alibaba_real_test_support.hpp` and
/// `scripts/ci-cloud-credentials/alibaba/README.md` already use. As with the
/// OCI equivalent, presence is checked here and content is the signer's
/// question.

#include <raft/alibaba_client_config.hpp>
#include <raft/env_config_result.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace kythira {

namespace alibaba_env_detail {

inline constexpr const char* k_region = "KYTHIRA_ALIBABA_REGION";
inline constexpr const char* k_access_key_id = "KYTHIRA_ALIBABA_ACCESS_KEY_ID";
inline constexpr const char* k_access_key_secret = "KYTHIRA_ALIBABA_ACCESS_KEY_SECRET";
inline constexpr const char* k_security_token = "KYTHIRA_ALIBABA_SECURITY_TOKEN";
inline constexpr const char* k_endpoint = "KYTHIRA_ALIBABA_ENDPOINT_OVERRIDE";

}  // namespace alibaba_env_detail

/// @brief Builds an `alibaba_client_config` from `KYTHIRA_ALIBABA_*` variables.
///
/// The region and the AccessKey pair are required and reported together in
/// one error. An `STS.` key with no token is a warning, not an error: the
/// prefix is Alibaba's convention, not a documented contract (Requirement
/// 2.2).
[[nodiscard]] inline auto alibaba_client_config_from_env(const env_lookup& env)
    -> env_config_result<alibaba_client_config> {
    using namespace alibaba_env_detail;
    env_config_result<alibaba_client_config> result;

    const auto region = env_value(env, k_region);
    const auto key_id = env_value(env, k_access_key_id);
    const auto secret = env_value(env, k_access_key_secret);
    const auto token = env_value(env, k_security_token);

    std::vector<const char*> missing;
    if (!region) {
        missing.push_back(k_region);
    }
    if (!key_id) {
        missing.push_back(k_access_key_id);
    }
    if (!secret) {
        missing.push_back(k_access_key_secret);
    }
    if (!missing.empty()) {
        std::string message = "missing required environment for Alibaba OSS: ";
        for (std::size_t i = 0; i < missing.size(); ++i) {
            message += std::string(i == 0 ? "" : ", ") + missing[i];
        }
        result.errors.push_back(std::move(message));
    }

    if (key_id && key_id->starts_with("STS.") && !token) {
        result.warnings.push_back(std::string(k_access_key_id) +
                                  " looks like an STS key (it starts with \"STS.\"), and STS keys "
                                  "need " +
                                  k_security_token + ", which is not set");
    }

    auto& cfg = result.config;
    cfg.region = region.value_or("");
    cfg.access_key_id = key_id.value_or("");
    cfg.access_key_secret = secret.value_or("");
    cfg.security_token = token.value_or("");
    cfg.endpoint_override = env_value(env, k_endpoint).value_or("");
    return result;
}

}  // namespace kythira
