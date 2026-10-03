// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file oci_client_config_env.hpp
/// @brief Builds an `oci_client_config` (and an `oci_object_storage_config`)
///        from `KYTHIRA_OCI_*` environment variables
///        (`.kiro/specs/object-backup-oci-oss-credentials/`, Requirements 1
///        and 3).
///
/// `oci_client_config.hpp` leaves sourcing its values to the caller. This is
/// that caller for any binary that takes OCI credentials from the
/// environment; `raft_object_backup` is the first. It is named for the config
/// it builds rather than for the CLI so the next `cmd/` binary can reuse it.
///
/// The variable names are the ones `tests/oci_real_test_support.hpp` and the
/// real-cloud CI workflow already export, so an environment that runs the real
/// suites also runs the tool.
///
/// **Presence, not content.** This checks that each variable a mode needs is
/// set and that the key file can be read. It does not parse the key or check
/// the fingerprint's format: `oci_signing::sign_request` already decides
/// whether material is usable (oci-cloud-provider Requirement 1.5), and a
/// second, slightly different check here is how the two would drift apart.
///
/// **Instance principal is never inferred.** Its first step is a metadata
/// fetch that, off OCI, fails only after a connect timeout and names
/// `169.254.169.254` rather than credentials. Inferring it whenever the
/// API-key variables are absent would turn "you forgot
/// `KYTHIRA_OCI_USER_ID`" into a slow, misleading failure.

#include <raft/env_config_result.hpp>
#include <raft/oci_client_config.hpp>
#include <raft/oci_object_storage_config.hpp>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kythira {

namespace oci_env_detail {

inline constexpr const char* k_region = "KYTHIRA_OCI_REGION";
inline constexpr const char* k_auth = "KYTHIRA_OCI_AUTH";
inline constexpr const char* k_tenancy = "KYTHIRA_OCI_TENANCY_ID";
inline constexpr const char* k_user = "KYTHIRA_OCI_USER_ID";
inline constexpr const char* k_fingerprint = "KYTHIRA_OCI_FINGERPRINT";
inline constexpr const char* k_key_pem = "KYTHIRA_OCI_PRIVATE_KEY_PEM";
inline constexpr const char* k_key_file = "KYTHIRA_OCI_PRIVATE_KEY_FILE";
inline constexpr const char* k_passphrase = "KYTHIRA_OCI_PRIVATE_KEY_PASSPHRASE";
inline constexpr const char* k_token = "KYTHIRA_OCI_SECURITY_TOKEN";
inline constexpr const char* k_namespace = "KYTHIRA_OCI_NAMESPACE";
inline constexpr const char* k_endpoint = "KYTHIRA_OCI_ENDPOINT_OVERRIDE";

/// The three modes `oci_client_config` supports.
enum class auth_mode {
    api_key,
    security_token,
    instance_principal
};

[[nodiscard]] inline auto mode_name(auth_mode mode) -> const char* {
    switch (mode) {
        case auth_mode::api_key:
            return "api_key";
        case auth_mode::security_token:
            return "security_token";
        case auth_mode::instance_principal:
            return "instance_principal";
    }
    return "api_key";
}

/// Reads the whole key file. On failure returns nullopt and sets @p error to a
/// message naming the variable, the path and the OS error — never the
/// contents. An empty file counts as unreadable (Requirement 3.5).
[[nodiscard]] inline auto read_key_file(const std::string& path, std::string& error)
    -> std::optional<std::string> {
    const auto fail = [&](const std::string& why) {
        error =
            std::string(k_key_file) + " names \"" + path + "\", which could not be read: " + why;
        return std::nullopt;
    };
    errno = 0;
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        return fail(std::strerror(errno));
    }
    std::string contents;
    char buffer[4096];
    std::size_t got = 0;
    while ((got = std::fread(buffer, 1, sizeof buffer, file)) > 0) {
        contents.append(buffer, got);
    }
    const bool read_failed = std::ferror(file) != 0;
    const int read_errno = errno;
    std::fclose(file);
    if (read_failed) {
        return fail(std::strerror(read_errno != 0 ? read_errno : EIO));
    }
    if (contents.empty()) {
        return fail("the file is empty");
    }
    return contents;
}

[[nodiscard]] inline auto join(const std::vector<std::string>& names) -> std::string {
    std::string out;
    for (std::size_t i = 0; i < names.size(); ++i) {
        out += (i == 0 ? "" : ", ") + names[i];
    }
    return out;
}

}  // namespace oci_env_detail

/// @brief Builds an `oci_client_config` from `KYTHIRA_OCI_*` variables.
///
/// The mode comes from `KYTHIRA_OCI_AUTH` or is inferred: `security_token`
/// when `KYTHIRA_OCI_SECURITY_TOKEN` is set, otherwise `api_key` (Requirement
/// 1.3). An unknown `KYTHIRA_OCI_AUTH` is an error, and validation still runs
/// under the inferred mode so the message lists everything else too. Every
/// problem is collected; none throws.
[[nodiscard]] inline auto oci_client_config_from_env(const env_lookup& env)
    -> env_config_result<oci_client_config> {
    using namespace oci_env_detail;
    env_config_result<oci_client_config> result;

    const auto region = env_value(env, k_region);
    const auto auth = env_value(env, k_auth);
    const auto tenancy = env_value(env, k_tenancy);
    const auto user = env_value(env, k_user);
    const auto fingerprint = env_value(env, k_fingerprint);
    const auto key_pem = env_value(env, k_key_pem);
    const auto key_file = env_value(env, k_key_file);
    const auto passphrase = env_value(env, k_passphrase);
    const auto token = env_value(env, k_token);
    const auto endpoint = env_value(env, k_endpoint);

    auth_mode mode = token ? auth_mode::security_token : auth_mode::api_key;
    if (auth) {
        if (*auth == "api_key") {
            mode = auth_mode::api_key;
        } else if (*auth == "security_token") {
            mode = auth_mode::security_token;
        } else if (*auth == "instance_principal") {
            mode = auth_mode::instance_principal;
        } else {
            // The value is not secret, so it is echoed: a typo is easier to
            // see than to infer.
            result.errors.push_back(std::string(k_auth) + "=\"" + *auth +
                                    "\" is not one of: api_key, security_token, "
                                    "instance_principal");
        }
    }

    std::vector<std::string> missing;
    if (!region) {
        missing.emplace_back(k_region);
    }

    auto& cfg = result.config;
    cfg.region = region.value_or("");
    cfg.endpoint_override = endpoint.value_or("");

    if (mode == auth_mode::instance_principal) {
        cfg.use_instance_principal = true;
        // Not an error: the operator may be switching modes on a host that
        // still exports the old ones. But silently ignoring a credential is
        // how one ends up debugging the wrong identity.
        for (const char* name :
             {k_tenancy, k_user, k_fingerprint, k_key_pem, k_key_file, k_passphrase, k_token}) {
            if (env_value(env, name)) {
                result.warnings.push_back(std::string(name) +
                                          " is set but ignored under instance_principal auth");
            }
        }
    } else {
        if (mode == auth_mode::api_key) {
            if (!tenancy) {
                missing.emplace_back(k_tenancy);
            }
            if (!user) {
                missing.emplace_back(k_user);
            }
            if (!fingerprint) {
                missing.emplace_back(k_fingerprint);
            }
            cfg.tenancy_id = tenancy.value_or("");
            cfg.user_id = user.value_or("");
            cfg.fingerprint = fingerprint.value_or("");
        } else {
            if (!token) {
                missing.emplace_back(k_token);
            }
            cfg.security_token = token.value_or("");
        }

        if (key_pem && key_file) {
            // Picking one would leave the operator unsure which key signed.
            result.errors.push_back(std::string("both ") + k_key_pem + " and " + k_key_file +
                                    " are set; set exactly one");
        } else if (key_pem) {
            cfg.private_key_pem = *key_pem;
        } else if (key_file) {
            std::string error;
            if (auto contents = read_key_file(*key_file, error)) {
                cfg.private_key_pem = std::move(*contents);
            } else {
                result.errors.push_back(std::move(error));
            }
        } else {
            missing.push_back(std::string(k_key_pem) + " or " + k_key_file);
        }
        cfg.private_key_passphrase = passphrase.value_or("");
    }

    if (!missing.empty()) {
        // One message for the whole mode, inserted first so it leads.
        result.errors.insert(result.errors.begin(),
                             std::string("missing required environment for OCI ") +
                                 mode_name(mode) + " auth: " + join(missing));
    }
    return result;
}

/// @brief `oci_client_config_from_env`, plus `KYTHIRA_OCI_NAMESPACE` into
///        `namespace_name`. Unset leaves it empty, so the client resolves it
///        with `GET /n/` as before (Requirement 1.6).
[[nodiscard]] inline auto oci_object_storage_config_from_env(const env_lookup& env)
    -> env_config_result<oci_object_storage_config> {
    auto base = oci_client_config_from_env(env);
    env_config_result<oci_object_storage_config> result;
    result.config.oci = std::move(base.config);
    result.config.namespace_name = env_value(env, oci_env_detail::k_namespace).value_or("");
    result.errors = std::move(base.errors);
    result.warnings = std::move(base.warnings);
    return result;
}

}  // namespace kythira
