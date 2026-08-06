#pragma once

/// @file gcp_client_options.hpp
/// @brief The `google::cloud::Options` every GCP client in this codebase is
///        built from: credentials, endpoint override, and the transport-level
///        time bounds that keep a stalled call from blocking its caller
///        forever.
///
/// Extracted because `gcp_compute_detail`, `gcp_mig_detail` and
/// `gcp_privateca_detail` each had a byte-identical `make_options()`, and the
/// defect this header exists to fix was present in all three at once: none of
/// them set any retry, backoff or deadline policy, so every call inherited
/// whatever google-cloud-cpp's defaults happened to be and
/// `gcp_client_config::api_timeout` — documented as "maximum time allowed for a
/// single GCP API call" — was consulted only by `gcp_operation_wait`, never by
/// a client.
///
/// Only the service-agnostic half lives here. Retry/backoff/polling policies
/// are per-service types in google-cloud-cpp (`InstancesRetryPolicyOption`,
/// `InstanceGroupManagersRetryPolicyOption`, …) with no common option to set,
/// so each `make_*_client()` layers its own on top of `make_base_options()`.

#include <raft/gcp_client_config.hpp>

// Both GCP feature macros, not just `KYTHIRA_HAS_GCP_SDK`: CMake gates
// `KYTHIRA_HAS_GCP_PRIVATECA` independently of the compute components, so a
// privateca-only build must still get `gcp_detail`.
#if defined(KYTHIRA_HAS_GCP_SDK) || defined(KYTHIRA_HAS_GCP_PRIVATECA)

#include <google/cloud/backoff_policy.h>
#include <google/cloud/credentials.h>
#include <google/cloud/options.h>
#include <google/cloud/polling_policy.h>

// The two REST time-bound headers are probed rather than required, for the same
// reason the feature macros above are both accepted: a privateca-only build has
// no REST transport, and `internal/rest_options.h` is an internal header that a
// future SDK may relocate. Either absence costs the corresponding bound and
// leaves the build working.
#if __has_include(<google/cloud/rest_options.h>)
#include <google/cloud/rest_options.h>
#define KYTHIRA_HAS_GCP_REST_SERVER_TIMEOUT 1
#endif
#if __has_include(<google/cloud/internal/rest_options.h>)
#include <google/cloud/internal/rest_options.h>
#define KYTHIRA_HAS_GCP_REST_STALL_TIMEOUT 1
#endif

#include <chrono>
#include <memory>

namespace kythira::gcp_detail {

/// Backoff between retries of a failed GCP call: 200ms growing geometrically
/// to a 5s ceiling, with the jitter the SDK applies itself.
///
/// Shared by the retry loop and the LRO polling loop, and deliberately capped
/// well below `gcp_client_config::api_timeout` so a call that is going to be
/// retried at all gets several attempts inside its deadline rather than one.
inline auto make_backoff_policy() -> std::shared_ptr<google::cloud::BackoffPolicy> {
    return std::make_shared<google::cloud::ExponentialBackoffPolicy>(std::chrono::milliseconds{200},
                                                                     std::chrono::seconds{5}, 2.0);
}

/// Credentials and endpoint override — the part that is transport-agnostic,
/// and so the only part the gRPC-backed PrivateCA client can use.
inline auto make_credential_options(const gcp_client_config& gcp) -> google::cloud::Options {
    google::cloud::Options opts;
    if (!gcp.credentials_json.empty()) {
        opts.set<google::cloud::UnifiedCredentialsOption>(
            google::cloud::MakeServiceAccountCredentials(gcp.credentials_json));
    } else {
        opts.set<google::cloud::UnifiedCredentialsOption>(
            google::cloud::MakeGoogleDefaultCredentials());
    }
    if (!gcp.endpoint_override.empty()) {
        opts.set<google::cloud::EndpointOption>(gcp.endpoint_override);
    }
    return opts;
}

/// `make_credential_options` plus the two REST transport time bounds. For REST
/// clients only — google-cloud-cpp warns about options a connection does not
/// recognise, and both of these are curl-transport settings.
///
/// Two distinct bounds are set because they stop different things:
/// - `ServerTimeoutOption` caps how long the *server* spends on the request.
///   It is a request parameter, so it does nothing for a connection that hangs
///   before a response starts arriving.
/// - `TransferStallTimeoutOption` is what the curl transport turns into
///   `CURLOPT_LOW_SPEED_TIME`, so it is the only one of the two that aborts a
///   genuinely stalled socket client-side. It lives in the `rest_internal`
///   namespace, which is the trade-off accepted here: the public option set has
///   no client-side deadline at all, and an unbounded call is the worse risk.
///
/// Neither bounds the *retry loop* — that is the caller's per-service retry
/// policy. Note also that a stall bound is not a total-duration bound: a call
/// that keeps trickling bytes stays alive, by design, since that is what makes
/// the option safe to apply to large transfers.
inline auto make_base_options(const gcp_client_config& gcp) -> google::cloud::Options {
    auto opts = make_credential_options(gcp);
    // A non-positive `api_timeout` is the documented way to ask for no bound.
    if (gcp.api_timeout.count() > 0) {
#if defined(KYTHIRA_HAS_GCP_REST_SERVER_TIMEOUT)
        opts.set<google::cloud::ServerTimeoutOption>(
            std::chrono::duration_cast<std::chrono::milliseconds>(gcp.api_timeout));
#endif
#if defined(KYTHIRA_HAS_GCP_REST_STALL_TIMEOUT)
        opts.set<google::cloud::rest_internal::TransferStallTimeoutOption>(gcp.api_timeout);
        opts.set<google::cloud::rest_internal::DownloadStallTimeoutOption>(gcp.api_timeout);
#endif
    }
    return opts;
}

/// Sets the retry, backoff and LRO-polling options a generated client accepts,
/// given that service's option/policy types.
///
/// The polling policy is **not** optional, and must not be left to the SDK's
/// default. google-cloud-cpp derives that default from whichever retry policy
/// the client carries, so configuring a per-call retry bound at all silently
/// applies the same bound to the long-running-operation polling loop. With
/// `api_timeout` at its 30s default that killed `instances.delete` mid-flight
/// against live GCE. Setting an explicit policy from `operation_timeout`
/// decouples the two — which is the whole reason `operation_timeout` exists.
///
/// @tparam RetryOption   e.g. `InstancesRetryPolicyOption`
/// @tparam RetryPolicy   e.g. `InstancesLimitedTimeRetryPolicy`
/// @tparam BackoffOption e.g. `InstancesBackoffPolicyOption`
/// @tparam PollingOption e.g. `InstancesPollingPolicyOption`
template<typename RetryOption, typename RetryPolicy, typename BackoffOption, typename PollingOption>
auto with_retry_policies(google::cloud::Options opts, const gcp_client_config& gcp)
    -> google::cloud::Options {
    opts.set<RetryOption>(std::make_shared<RetryPolicy>(gcp.api_timeout));
    opts.set<BackoffOption>(make_backoff_policy());
    opts.set<PollingOption>(std::make_shared<google::cloud::GenericPollingPolicy<
                                std::shared_ptr<typename RetryOption::Type::element_type>,
                                std::shared_ptr<google::cloud::BackoffPolicy>>>(
        std::make_shared<RetryPolicy>(gcp.operation_timeout), make_backoff_policy()));
    return opts;
}

/// The subset for a client with no long-running operations — `ZoneOperations`
/// is itself the LRO-polling surface and has no `PollingPolicyOption`.
template<typename RetryOption, typename RetryPolicy, typename BackoffOption>
auto with_retry_policies_no_lro(google::cloud::Options opts, const gcp_client_config& gcp)
    -> google::cloud::Options {
    opts.set<RetryOption>(std::make_shared<RetryPolicy>(gcp.api_timeout));
    opts.set<BackoffOption>(make_backoff_policy());
    return opts;
}

}  // namespace kythira::gcp_detail

#endif  // KYTHIRA_HAS_GCP_SDK || KYTHIRA_HAS_GCP_PRIVATECA
