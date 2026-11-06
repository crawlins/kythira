// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file credential_verifier.hpp
/// @brief The seam through which a service asks something outside itself
///        whether a non-password credential is valid
///        (.kiro/specs/redis-gateway-module/ Requirement 6, design
///        "Authentication seam").
///
/// A service (the Redis gateway's `AUTH`, the build cache's HTTP front end)
/// hands a `credential_request` to a verifier and gets back a future of
/// either a `credential_accepted` (a principal and when the acceptance
/// expires) or a `credential_denied`. The verifier may answer from local
/// state or over the network; the caller waits on the future off its I/O
/// strand, so neither case blocks other connections.
///
/// Nothing here names IAM, AWS or any token format. IAM's authorizer adapts
/// to it (`.kiro/specs/iam-compatible-auth/` task 21), and an OIDC/JWT or
/// mTLS verifier could too. Which users may authenticate this way is the
/// service's decision (for Redis, the ACL's `auth=` attribute), not the
/// verifier's: an accepted credential only says who the caller is, and the
/// service's own role and key-prefix rules still apply afterwards.

#include <concepts/future.hpp>
#include <raft/future_default.hpp>

#include <chrono>
#include <concepts>
#include <functional>
#include <string>
#include <string_view>
#include <variant>

namespace kythira {

/// What the service was handed. Every view is valid only for the duration
/// of the `verify()` call; a verifier that answers later copies what it
/// needs.
struct credential_request {
    /// Which service is asking: "redis", "cache-http".
    std::string_view service;
    /// The AUTH user or Basic user name. Empty for a bearer token.
    std::string_view user;
    /// The password slot, verbatim.
    std::string_view credential;
    /// The remote address, for audit and rate keys.
    std::string_view peer;
};

/// The credential is good. `principal` is for logs and metrics only; the
/// service's authorization still keys on its own user. `expires` bounds the
/// connection (or cached result) the credential opened.
struct credential_accepted {
    std::string principal;
    std::chrono::system_clock::time_point expires;
};

/// The credential is not good. `reason` goes to the service's audit log,
/// never to the client: a client is told only that authentication failed.
struct credential_denied {
    std::string reason;
};

using credential_result = std::variant<credential_accepted, credential_denied>;

/// Anything with a `verify(const credential_request&)` that returns a
/// future of a `credential_result`.
template<typename V>
concept credential_verifier = requires(V& v, const credential_request& r) {
    { v.verify(r) } -> kythira::future<credential_result>;
};

/// The type-erased form a service's configuration holds, so the service
/// does not become a template on its verifier. Empty means "no verifier":
/// the service behaves exactly as it does without this header.
using credential_verifier_fn =
    std::function<future_default<credential_result>(const credential_request&)>;

/// Erase a `credential_verifier`. The verifier is held by reference and must
/// outlive every service configured with the result.
template<credential_verifier V>
[[nodiscard]] auto erase_credential_verifier(V& verifier) -> credential_verifier_fn {
    return [&verifier](const credential_request& r) -> future_default<credential_result> {
        return verifier.verify(r);
    };
}

}  // namespace kythira
