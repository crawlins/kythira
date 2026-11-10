// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#include "export_store.hpp"

#include <stdexcept>

#if defined(KYTHIRA_LEDGER_NODE_S3)
#include <raft/aws_client_config.hpp>
#include <raft/aws_s3_client.hpp>

#include <aws/core/Aws.h>

#include <algorithm>
#include <chrono>
#include <mutex>
#endif

namespace kythira::ledger_node {

#if defined(KYTHIRA_LEDGER_NODE_S3)

namespace {

/// One Aws::InitAPI for the process while any S3 export store exists, and
/// the matching ShutdownAPI after the last one is gone. ledger_node creates
/// one store, but a counted scope keeps a second (a test, a future reload)
/// from shutting the SDK down under the first.
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

class s3_export_store final : public export_store {
public:
    explicit s3_export_store(const node_options& opt) : _client(client_config(opt)) {}

    [[nodiscard]] auto put_object(const std::string& bucket, const std::string& key,
                                  std::string_view bytes) const -> kythira::put_result override {
        return _client.put_object(bucket, key, bytes);
    }
    [[nodiscard]] auto get_object(const std::string& bucket, const std::string& key) const
        -> std::optional<kythira::get_result> override {
        return _client.get_object(bucket, key);
    }
    auto delete_object(const std::string& bucket, const std::string& key) const -> void override {
        _client.delete_object(bucket, key);
    }
    [[nodiscard]] auto list_keys(const std::string& bucket, const std::string& prefix) const
        -> std::vector<std::string> override {
        return _client.list_keys(bucket, prefix);
    }
    [[nodiscard]] auto provider_name() const -> std::string_view override {
        return kythira::aws_s3_client::provider_name();
    }

private:
    static auto client_config(const node_options& opt) -> kythira::aws_client_config {
        kythira::aws_client_config cfg;
        cfg.region = opt.export_s3_region;
        cfg.endpoint_override = opt.export_s3_endpoint;
        // aws_client_config counts whole seconds; round a sub-second
        // timeout up rather than to zero (no timeout at all).
        cfg.api_timeout = std::max(std::chrono::seconds{1},
                                   std::chrono::ceil<std::chrono::seconds>(opt.export_s3_timeout));
        return cfg;
    }

    // Declared first, so the SDK outlives the client it backs.
    aws_sdk_scope _sdk;
    kythira::aws_s3_client _client;
};

}  // namespace

auto make_export_store(const node_options& opt) -> std::unique_ptr<export_store> {
    if (opt.export_s3_region.empty()) {
        return nullptr;
    }
    return std::make_unique<s3_export_store>(opt);
}

#else

auto make_export_store(const node_options& opt) -> std::unique_ptr<export_store> {
    if (!opt.export_s3_region.empty()) {
        throw std::invalid_argument(
            "ledger_node: exports to S3 need a build with the AWS SDK's S3 client");
    }
    return nullptr;
}

#endif

}  // namespace kythira::ledger_node
