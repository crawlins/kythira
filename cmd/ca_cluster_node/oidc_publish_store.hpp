// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

// The object-store client behind --oidc-publish-url
// (`.kiro/specs/iam-compatible-auth/` Requirement 1.9).
//
// Which providers exist is a build question, answered the way
// cmd/raft_object_backup answers it: each KYTHIRA_OIDC_PUBLISH_* macro is
// set by this directory's CMakeLists.txt only when the provider's
// dependency was found AND its persistence Kconfig gate is on. Credentials
// come from the same places the backup tool reads them: the SDK default
// chains for S3, Azure and GCS, and the KYTHIRA_OCI_* / KYTHIRA_ALIBABA_*
// variables for OCI and OSS.

#include <raft/oidc_publisher.hpp>

#ifdef KYTHIRA_OIDC_PUBLISH_S3
#include <raft/aws_s3_client.hpp>

#include <aws/core/Aws.h>
#endif
#ifdef KYTHIRA_OIDC_PUBLISH_AZURE
#include <raft/azure_blob_client.hpp>
#endif
#ifdef KYTHIRA_OIDC_PUBLISH_GCS
#include <raft/gcp_gcs_client.hpp>
#endif
#ifdef KYTHIRA_OIDC_PUBLISH_OCI
#include <raft/env_config_result.hpp>
#include <raft/oci_client_config_env.hpp>
#include <raft/oci_object_storage_client.hpp>
#endif
#ifdef KYTHIRA_OIDC_PUBLISH_OSS
#include <raft/alibaba_client_config_env.hpp>
#include <raft/alibaba_oss_client.hpp>
#include <raft/env_config_result.hpp>
#endif

#include <cstdlib>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

namespace ca_cluster_node {

/// One upload: (bucket, key, body). Throws on failure.
using oidc_put_fn =
    std::function<void(const std::string& bucket, const std::string& key, std::string_view body)>;

/// Adapts an oidc_put_fn to the store shape publish_documents() takes.
struct oidc_put_store {
    const oidc_put_fn& put;
    void put_object(const std::string& bucket, const std::string& key,
                    std::string_view body) const {
        put(bucket, key, body);
    }
};

/// Whether this binary can publish to `provider`, and if not, what would
/// enable it (for the startup error).
struct oidc_publish_provider_info {
    bool available;
    std::string_view needs;
};

[[nodiscard]] inline auto oidc_publish_provider(std::string_view provider)
    -> oidc_publish_provider_info {
    if (provider == "s3") {
#ifdef KYTHIRA_OIDC_PUBLISH_S3
        return {true, {}};
#else
        return {false, "CONFIG_AWS_S3_PERSISTENCE=y and the AWS SDK"};
#endif
    }
    if (provider == "azure-blob") {
#ifdef KYTHIRA_OIDC_PUBLISH_AZURE
        return {true, {}};
#else
        return {false, "CONFIG_AZURE_BLOB_PERSISTENCE=y and the Azure SDK"};
#endif
    }
    if (provider == "gcs") {
#ifdef KYTHIRA_OIDC_PUBLISH_GCS
        return {true, {}};
#else
        return {false, "CONFIG_GCP_STORAGE_PERSISTENCE=y and vcpkg's `gcp` feature"};
#endif
    }
    if (provider == "oci-objectstorage") {
#ifdef KYTHIRA_OIDC_PUBLISH_OCI
        return {true, {}};
#else
        return {false, "CONFIG_OCI_OBJECT_PERSISTENCE=y"};
#endif
    }
    if (provider == "oss") {
#ifdef KYTHIRA_OIDC_PUBLISH_OSS
        return {true, {}};
#else
        return {false, "CONFIG_ALIBABA_OSS_PERSISTENCE=y"};
#endif
    }
    return {false, "a known provider"};
}

namespace oidc_publish_detail {

template<typename Client, typename Config> [[nodiscard]] auto wrap(Config cfg) -> oidc_put_fn {
    auto client = std::make_shared<Client>(std::move(cfg));
    return [client](const std::string& bucket, const std::string& key, std::string_view body) {
        (void)client->put_object(bucket, key, body);
    };
}

#if defined(KYTHIRA_OIDC_PUBLISH_OCI) || defined(KYTHIRA_OIDC_PUBLISH_OSS)
template<typename Config>
[[nodiscard]] auto config_or_throw(kythira::env_config_result<Config> result,
                                   std::string_view provider) -> Config {
    if (!result.ok()) {
        std::string msg = std::string(provider) + " configuration:";
        for (const auto& e : result.errors) {
            msg += " " + e + ";";
        }
        throw std::runtime_error(msg);
    }
    return std::move(result.config);
}
#endif

#ifdef KYTHIRA_OIDC_PUBLISH_S3
/// aws_s3_client leaves Aws::InitAPI to its caller (see cmd/raft_object_backup).
/// Initialised once, on first use, and shut down at process exit.
inline void ensure_aws_sdk() {
    struct sdk {
        Aws::SDKOptions options;
        sdk() { Aws::InitAPI(options); }
        ~sdk() { Aws::ShutdownAPI(options); }
        sdk(const sdk&) = delete;
        auto operator=(const sdk&) -> sdk& = delete;
    };
    static sdk instance;
}
#endif

}  // namespace oidc_publish_detail

/// Builds the client for `target`. Throws when the provider is not compiled
/// in or its configuration is incomplete; the caller treats that as a
/// failed publish and retries, so a credential fixed later is picked up.
[[nodiscard]] inline auto make_oidc_publish_store(const kythira::oidc::publish_target& target)
    -> oidc_put_fn {
    namespace d = oidc_publish_detail;
#ifdef KYTHIRA_OIDC_PUBLISH_S3
    if (target.provider == "s3") {
        d::ensure_aws_sdk();
        return d::wrap<kythira::aws_s3_client>(kythira::aws_client_config{});
    }
#endif
#ifdef KYTHIRA_OIDC_PUBLISH_AZURE
    if (target.provider == "azure-blob") {
        const char* account = std::getenv("KYTHIRA_AZURE_STORAGE_ACCOUNT");
        if (account == nullptr) {
            throw std::runtime_error(
                "az:// publishing needs KYTHIRA_AZURE_STORAGE_ACCOUNT, the storage account name");
        }
        kythira::azure_blob_config cfg;
        cfg.account = account;
        return d::wrap<kythira::azure_blob_client>(std::move(cfg));
    }
#endif
#ifdef KYTHIRA_OIDC_PUBLISH_GCS
    if (target.provider == "gcs") {
        kythira::gcp_client_config cfg;
        if (const char* project = std::getenv("GOOGLE_CLOUD_PROJECT"); project != nullptr) {
            cfg.project_id = project;
        }
        return d::wrap<kythira::gcp_gcs_client>(std::move(cfg));
    }
#endif
#ifdef KYTHIRA_OIDC_PUBLISH_OCI
    if (target.provider == "oci-objectstorage") {
        return d::wrap<kythira::oci_object_storage_client>(d::config_or_throw(
            kythira::oci_object_storage_config_from_env(kythira::process_env_lookup()),
            "oci-objectstorage"));
    }
#endif
#ifdef KYTHIRA_OIDC_PUBLISH_OSS
    if (target.provider == "oss") {
        return d::wrap<kythira::alibaba_oss_client>(d::config_or_throw(
            kythira::alibaba_client_config_from_env(kythira::process_env_lookup()), "oss"));
    }
#endif
    throw std::runtime_error("this ca_cluster_node was built without " + target.provider +
                             " support (needs " +
                             std::string(oidc_publish_provider(target.provider).needs) + ")");
}

}  // namespace ca_cluster_node
