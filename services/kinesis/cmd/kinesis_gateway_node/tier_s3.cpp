// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#include "tier_s3.hpp"

#include <raft/aws_client_config.hpp>
#include <raft/aws_s3_client.hpp>

#include <aws/core/Aws.h>

#include <memory>
#include <utility>

namespace kythira::kinesis_node {

namespace {

class s3_tier_impl final : public s3_tier {
public:
    explicit s3_tier_impl(const node_options& opt) {
        Aws::InitAPI(_options);
        kythira::aws_client_config cfg;
        cfg.region = opt._tier_region;
        cfg.endpoint_override = opt._tier_endpoint;
        _store = kinesis::make_tier_store<kythira::aws_s3_client>(
            std::make_shared<const kythira::aws_s3_client>(std::move(cfg)), opt._tier_bucket,
            opt._tier_prefix);
    }

    ~s3_tier_impl() override {
        // The client goes before the SDK does.
        _store = {};
        Aws::ShutdownAPI(_options);
    }

    s3_tier_impl(const s3_tier_impl&) = delete;
    auto operator=(const s3_tier_impl&) -> s3_tier_impl& = delete;
    s3_tier_impl(s3_tier_impl&&) = delete;
    auto operator=(s3_tier_impl&&) -> s3_tier_impl& = delete;

    [[nodiscard]] auto store() const -> const kinesis::tier_store& override { return _store; }

private:
    Aws::SDKOptions _options;
    kinesis::tier_store _store;
};

}  // namespace

auto make_s3_tier(const node_options& opt) -> std::unique_ptr<s3_tier> {
    return std::make_unique<s3_tier_impl>(opt);
}

}  // namespace kythira::kinesis_node
