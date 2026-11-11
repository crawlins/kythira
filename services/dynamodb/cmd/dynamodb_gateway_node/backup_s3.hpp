// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file backup_s3.hpp
/// @brief The S3 backup store of dynamodb_gateway_node
///        (.kiro/specs/dynamodb-compatible-api/ task 18).
///
/// backup_s3.cpp is the one translation unit that parses an AWS SDK
/// header, compiled with the SDK's S3 client when the build has it
/// (KYTHIRA_DYNAMODB_NODE_S3), as ledger_node's export store is.

#include <kythira/dynamodb/backup.hpp>
#include <kythira/dynamodb/gateway.hpp>

#include <memory>

namespace dynamodb_gateway {

/// The S3 store `cfg` names (KYTHIRA_DYNAMODB_BACKUP_S3_BUCKET), or null
/// when it names none. Throws std::invalid_argument when one is named in a
/// build without the S3 client.
[[nodiscard]] auto make_s3_backup_store(const kythira::dynamodb::gateway_config& cfg)
    -> std::shared_ptr<kythira::dynamodb::backup_store>;

}  // namespace dynamodb_gateway
