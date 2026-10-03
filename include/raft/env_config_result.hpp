// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file env_config_result.hpp
/// @brief The shared shape for turning environment variables into a client
///        config: an injectable lookup, and a result that collects every
///        problem instead of throwing on the first
///        (`.kiro/specs/object-backup-oci-oss-credentials/`).
///
/// The lookup is injected so a test can drive every case from a map, without
/// `setenv` and without touching the process environment. Production passes
/// `process_env_lookup()`.

#include <cstdlib>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace kythira {

/// Looks up one environment variable. Empty values are returned as nullopt,
/// so every caller treats "set to empty" as unset (Requirement 3.3).
using env_lookup = std::function<std::optional<std::string>(std::string_view)>;

/// The process environment, through `std::getenv`.
[[nodiscard]] inline auto process_env_lookup() -> env_lookup {
    return [](std::string_view name) -> std::optional<std::string> {
        const char* value = std::getenv(std::string(name).c_str());
        if (value == nullptr || *value == '\0') {
            return std::nullopt;
        }
        return std::string(value);
    };
}

/// One variable through @p env, with an empty value read as unset whatever
/// the lookup itself does (Requirement 3.3). A test's map can hold `""`;
/// `process_env_lookup()` already filters it.
[[nodiscard]] inline auto env_value(const env_lookup& env, std::string_view name)
    -> std::optional<std::string> {
    auto value = env(name);
    if (value && value->empty()) {
        return std::nullopt;
    }
    return value;
}

/// A config built from the environment, or the reasons it could not be.
///
/// Every error is collected rather than thrown on the first, so an operator
/// sees everything missing in one message and fixes the environment once
/// (Requirement 3.2). Errors and warnings name variables and never carry their
/// values: some of them are secrets (Requirement 3.4).
template<typename Config> struct env_config_result {
    Config config;                      ///< Meaningful only when ok().
    std::vector<std::string> errors;    ///< Each names variables, never values.
    std::vector<std::string> warnings;  ///< Same rule.

    [[nodiscard]] auto ok() const -> bool { return errors.empty(); }
};

}  // namespace kythira
