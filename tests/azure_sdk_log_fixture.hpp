// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file azure_sdk_log_fixture.hpp
/// @brief Boost.Test global fixture that routes the Azure SDK's own
///        diagnostics to stderr.
///
/// Its own header rather than part of `azure_real_test_support.hpp`, because
/// `azure_key_vault_ca_provider_real_test` is the binary whose credential chain
/// actually failed undiagnosably and it shares no support header with the
/// quorum-manager suite -- it has no cost accumulator, no signal-handled
/// fixture and nothing to tear down. Pulling all of that in to get a log
/// listener would be the wrong trade.

#ifdef KYTHIRA_HAS_AZURE_SDK

#include <azure/core/diagnostics/logger.hpp>

#include <iostream>
#include <string>

namespace kythira::testing::azure_real {

/// Routes the Azure SDK's own diagnostics to stderr for the whole binary.
///
/// Exists because `ChainedTokenCredential`'s exception message is
/// content-free. A real failure (run 36612317090) surfaced only as
///
///     Failed to get token from ChainedTokenCredential.
///
/// which names neither the credential that failed nor why. The actual cause --
/// `AADSTS700024: Client assertion is not within its valid time range`, with
/// both timestamps and the exact five-minute validity window -- was in `az`'s
/// stderr in the same log, and was found by reading one layer below the
/// exception rather than by anything the test reported. A chain that tried
/// `EnvironmentCredential` (always unavailable under federation, no client
/// secret) and then `AzureCliCredential` records a reason per source at
/// Verbose; without a listener those reasons are discarded.
///
/// `Verbose`, not `Informational`: the per-credential reasons are logged at the
/// lowest level, so anything stricter drops exactly what this is for. The
/// volume is bounded -- these binaries authenticate a handful of times -- and
/// `run-real-cloud-suite.sh` already runs ctest with `-V` because a real-cloud
/// pass that shows nothing is not evidence.
struct AzureSdkLogFixture {
    AzureSdkLogFixture() {
        Azure::Core::Diagnostics::Logger::SetLevel(
            Azure::Core::Diagnostics::Logger::Level::Verbose);
        Azure::Core::Diagnostics::Logger::SetListener(
            [](Azure::Core::Diagnostics::Logger::Level level, const std::string& message) {
                const char* name = "unknown";
                switch (level) {
                    case Azure::Core::Diagnostics::Logger::Level::Error:
                        name = "error";
                        break;
                    case Azure::Core::Diagnostics::Logger::Level::Warning:
                        name = "warning";
                        break;
                    case Azure::Core::Diagnostics::Logger::Level::Informational:
                        name = "info";
                        break;
                    case Azure::Core::Diagnostics::Logger::Level::Verbose:
                        name = "verbose";
                        break;
                }
                std::cerr << "[azure-sdk " << name << "] " << message << "\n";
            });
    }

    /// Detaches the listener before the process tears down: it writes to
    /// `std::cerr`, and the SDK can log from its own threads during static
    /// destruction.
    ~AzureSdkLogFixture() { Azure::Core::Diagnostics::Logger::SetListener(nullptr); }
};

}  // namespace kythira::testing::azure_real

#endif  // KYTHIRA_HAS_AZURE_SDK
