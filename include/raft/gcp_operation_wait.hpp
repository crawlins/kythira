// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file gcp_operation_wait.hpp
/// @brief `wait_for_zone_operation` — polls a Compute Engine zone operation to
///        completion. Shared by `gcp_compute_quorum_manager` and
///        `gcp_mig_quorum_manager` so the insert/delete/resize call sites don't
///        each reimplement polling and error-unwrapping.
///
/// Every mutating Compute Engine call (`instances.insert`/`.delete`,
/// `instanceGroupManagers.resize`/`.deleteInstances`) returns an `Operation`
/// that must be polled to `status == DONE` via `zoneOperations.get` before the
/// mutation is guaranteed visible — unlike AWS's `RunInstances`/
/// `TerminateInstances`, which take effect essentially synchronously from the
/// caller's perspective.

#include <raft/fault_injection.hpp>
#include <raft/future_default.hpp>

#ifdef KYTHIRA_HAS_GCP_SDK

#include <google/cloud/compute/zone_operations/v1/zone_operations_client.h>

#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>

namespace kythira {

/// @brief Thrown (via an exceptional Future) when a zone operation does not
///        reach `status == DONE` before the caller's timeout elapses.
///
/// Distinct from a `std::runtime_error` carrying a GCP-reported operation error
/// so callers can tell "GCP said no" apart from "we gave up waiting"
/// (Requirement 4 AC 4).
class gcp_operation_timeout : public std::runtime_error {
public:
    explicit gcp_operation_timeout(const std::string& what) : std::runtime_error(what) {}
};

/// @brief Polls `zoneOperations.get` for @p operation_name at @p poll_interval
///        until it reports `status == DONE` or @p timeout elapses.
///
/// - On `DONE` with an empty `error` field, resolves the returned Future.
/// - On `DONE` with a non-empty `error` field, the Future is exceptional with a
///   message built from the operation's error code(s) and message(s).
/// - On @p timeout elapsing first, the Future is exceptional with a
///   `gcp_operation_timeout` (distinguishable from a GCP-reported error).
///
/// Checks the fault injection point `"raft/gcp/zone_operation/poll"` on every
/// poll iteration (Requirement 4 AC 5, Requirement 20 AC 9).
[[nodiscard]] inline auto wait_for_zone_operation(
    google::cloud::compute_zone_operations_v1::ZoneOperationsClient& client, std::string project,
    std::string zone, std::string operation_name, std::chrono::seconds timeout,
    std::chrono::milliseconds poll_interval) -> kythira::future_default<void> {
    try {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        for (;;) {
            fiu_do_on("raft/gcp/zone_operation/poll",
                      throw std::runtime_error("fault: raft/gcp/zone_operation/poll"););

            auto op = client.GetOperation(project, zone, operation_name);
            if (!op) {
                throw std::runtime_error("gcp zoneOperations.get: " + op.status().message());
            }

            // Compute represents the operation status as a string enum
            // ("PENDING"/"RUNNING"/"DONE"), not a C++ enum.
            if (op->status() == "DONE") {
                if (op->has_error() && op->error().errors_size() > 0) {
                    std::string msg = "gcp zone operation '" + operation_name + "' failed:";
                    for (const auto& e : op->error().errors()) {
                        msg += " [" + e.code() + "] " + e.message() + ";";
                    }
                    throw std::runtime_error(msg);
                }
                return kythira::future_factory_default::makeFuture();
            }

            if (std::chrono::steady_clock::now() >= deadline) {
                throw gcp_operation_timeout("gcp zone operation '" + operation_name +
                                            "' did not reach DONE within the timeout");
            }
            std::this_thread::sleep_for(poll_interval);
        }
    } catch (const gcp_operation_timeout&) {
        return kythira::future_factory_default::makeExceptionalFuture<void>(
            std::current_exception());
    } catch (const std::exception&) {
        return kythira::future_factory_default::makeExceptionalFuture<void>(
            std::current_exception());
    }
}

}  // namespace kythira

#endif  // KYTHIRA_HAS_GCP_SDK
