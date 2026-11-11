// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#include "backup_s3.hpp"

#include <stdexcept>

#if defined(KYTHIRA_DYNAMODB_NODE_S3)
#include <raft/aws_client_config.hpp>
#include <raft/aws_s3_client.hpp>

#include <aws/core/Aws.h>

#include <mutex>
#endif

namespace dynamodb_gateway {

#if defined(KYTHIRA_DYNAMODB_NODE_S3)

namespace {

/// One Aws::InitAPI for the process while any S3 backup store exists, and
/// the matching ShutdownAPI after the last is gone.
class aws_sdk_scope {
public:
    aws_sdk_scope() {
        const std::lock_guard lock(mutex());
        if (users()++ == 0) {
            Aws::InitAPI(options());
        }
    }
    aws_sdk_scope(const aws_sdk_scope&) = delete;
    aws_sdk_scope(aws_sdk_scope&&) = delete;
    auto operator=(const aws_sdk_scope&) -> aws_sdk_scope& = delete;
    auto operator=(aws_sdk_scope&&) -> aws_sdk_scope& = delete;
    ~aws_sdk_scope() {
        const std::lock_guard lock(mutex());
        if (--users() == 0) {
            Aws::ShutdownAPI(options());
        }
    }

private:
    static auto mutex() -> std::mutex& {
        static std::mutex m;
        return m;
    }
    static auto users() -> int& {
        static int n = 0;
        return n;
    }
    static auto options() -> Aws::SDKOptions& {
        static Aws::SDKOptions o;
        return o;
    }
};

/// The SDK scope outlives the client it serves.
struct scoped_s3_client {
    aws_sdk_scope scope;
    kythira::aws_s3_client client;

    explicit scoped_s3_client(const kythira::aws_client_config& cfg) : client(cfg) {}

    [[nodiscard]] auto put_object(const std::string& bucket, const std::string& key,
                                  std::string_view bytes) const -> kythira::put_result {
        return client.put_object(bucket, key, bytes);
    }
    [[nodiscard]] auto get_object(const std::string& bucket, const std::string& key) const
        -> std::optional<kythira::get_result> {
        return client.get_object(bucket, key);
    }
    auto delete_object(const std::string& bucket, const std::string& key) const -> void {
        client.delete_object(bucket, key);
    }
    [[nodiscard]] auto list_keys(const std::string& bucket, const std::string& prefix) const
        -> std::vector<std::string> {
        return client.list_keys(bucket, prefix);
    }
    [[nodiscard]] static auto provider_name() -> std::string_view {
        return kythira::aws_s3_client::provider_name();
    }
};

static_assert(kythira::key_object_store<scoped_s3_client>);

}  // namespace

auto make_s3_backup_store(const kythira::dynamodb::gateway_config& cfg)
    -> std::shared_ptr<kythira::dynamodb::backup_store> {
    if (cfg.backup_s3_bucket.empty()) {
        return nullptr;
    }
    kythira::aws_client_config c;
    c.region = cfg.backup_s3_region;
    c.endpoint_override = cfg.backup_s3_endpoint;
    return std::make_shared<kythira::dynamodb::object_backup_store<scoped_s3_client>>(
        std::make_shared<scoped_s3_client>(c), cfg.backup_s3_bucket, cfg.backup_s3_prefix);
}

#else

auto make_s3_backup_store(const kythira::dynamodb::gateway_config& cfg)
    -> std::shared_ptr<kythira::dynamodb::backup_store> {
    if (cfg.backup_s3_bucket.empty()) {
        return nullptr;
    }
    throw std::invalid_argument(
        "KYTHIRA_DYNAMODB_BACKUP_S3_BUCKET is set, but this dynamodb_gateway_node was built "
        "without the AWS SDK's S3 client");
}

#endif

}  // namespace dynamodb_gateway
