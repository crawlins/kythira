// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file export_store.hpp
/// @brief Where ledger_node's ExportJournal jobs write their objects
///        (.kiro/specs/verifiable-ledger/ task 7, the export runner).
///
/// `export_store` is a key_object_store behind a virtual interface, so
/// run.cpp can hand it to `export_journal()` without parsing an AWS SDK
/// header: the S3 implementation, and the SDK's InitAPI/ShutdownAPI, live
/// in export_store.cpp, which is compiled only when the SDK's S3 client is
/// part of the build (KYTHIRA_LEDGER_NODE_S3).

#include "config.hpp"

#include <raft/key_object_store.hpp>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace kythira::ledger_node {

class export_store {
public:
    export_store() = default;
    export_store(const export_store&) = delete;
    export_store(export_store&&) = delete;
    auto operator=(const export_store&) -> export_store& = delete;
    auto operator=(export_store&&) -> export_store& = delete;
    virtual ~export_store() = default;

    [[nodiscard]] virtual auto put_object(const std::string& bucket, const std::string& key,
                                          std::string_view bytes) const -> kythira::put_result = 0;
    [[nodiscard]] virtual auto get_object(const std::string& bucket, const std::string& key) const
        -> std::optional<kythira::get_result> = 0;
    virtual auto delete_object(const std::string& bucket, const std::string& key) const -> void = 0;
    [[nodiscard]] virtual auto list_keys(const std::string& bucket, const std::string& prefix) const
        -> std::vector<std::string> = 0;
    [[nodiscard]] virtual auto provider_name() const -> std::string_view = 0;
};

static_assert(kythira::key_object_store<export_store>);

/// The store `opt` configures, or null when exports are not configured
/// (KYTHIRA_LEDGER_EXPORT_S3_REGION unset). Throws std::invalid_argument if
/// they are configured in a build without the S3 client (config.cpp refuses
/// that first; this is the backstop).
[[nodiscard]] auto make_export_store(const node_options& opt) -> std::unique_ptr<export_store>;

}  // namespace kythira::ledger_node
