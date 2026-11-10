// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file tier_s3.hpp
/// @brief The S3 tier store for `kinesis_gateway_node` (task 10,
///        Requirement 11.3), behind an interface so no other translation
///        unit parses an AWS SDK header.
///
/// `tier_s3.cpp` is compiled only when the AWS SDK's s3 component is found
/// (`KYTHIRA_KINESIS_HAS_S3_TIER`); `from_env` refuses the tier settings in
/// a build without it, so `make_s3_tier` is never called there.

#include "config.hpp"

#include <kythira/kinesis/kinesis_tiering.hpp>

#include <memory>

namespace kythira::kinesis_node {

/// The S3 tier and the SDK initialisation it needs, alive together.
class s3_tier {
public:
    virtual ~s3_tier() = default;
    s3_tier() = default;
    s3_tier(const s3_tier&) = delete;
    auto operator=(const s3_tier&) -> s3_tier& = delete;
    s3_tier(s3_tier&&) = delete;
    auto operator=(s3_tier&&) -> s3_tier& = delete;

    /// Valid while this object lives.
    [[nodiscard]] virtual auto store() const -> const kinesis::tier_store& = 0;
};

/// The bucket, prefix, region and endpoint from `opt`.
/// @throws std::runtime_error when this build has no S3 tier.
[[nodiscard]] auto make_s3_tier(const node_options& opt) -> std::unique_ptr<s3_tier>;

}  // namespace kythira::kinesis_node
