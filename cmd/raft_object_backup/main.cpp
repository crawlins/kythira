// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file main.cpp
/// @brief `raft_object_backup` — create, inspect and restore backups of a Raft
///        node's cloud-object state (Requirement 10.6).
///
/// This binary exists because **operators do not have a C++ compiler in a
/// recovery window**. Everything about it except provider selection lives in
/// `include/raft/object_store_backup_cli.hpp`, which is generic over
/// `key_object_store` and therefore testable — including a full
/// create → list → verify → restore-clone cycle against the in-memory mock in
/// a build with zero cloud providers.
///
/// What is left here is the one thing that cannot be generic: mapping
/// `--provider` onto a client that this build actually contains, and
/// **reporting by name** the ones it does not (Requirement 16.4). A tool that
/// answered "unknown provider: gcs" on a build where GCS was merely switched
/// off would send an operator hunting for a typo during an outage.
///
/// Providers are gated by a uniform `KYTHIRA_BACKUP_PROVIDER_*` macro set that
/// this binary's `CMakeLists.txt` defines, rather than by each provider's own
/// macro. That indirection is load-bearing: `KYTHIRA_HAS_AWS_SDK` answers "was
/// the SDK found?", while the Kconfig symbols answer "was this backend wanted?",
/// and the CLI has to report the *conjunction* — an operator who turned
/// `CONFIG_AWS_S3_PERSISTENCE` off should be told that, not told the SDK is
/// missing. OCI and Alibaba have no dependency to find at all, so they have no
/// macro of their own and would otherwise be unconditionally present here even
/// in a build that switched them off.

#include <raft/env_config_result.hpp>
#include <raft/object_store_backup_cli.hpp>

#ifdef KYTHIRA_BACKUP_PROVIDER_S3
#include <raft/aws_s3_client.hpp>
#endif
#ifdef KYTHIRA_BACKUP_PROVIDER_AZURE
#include <raft/azure_blob_client.hpp>
#endif
#ifdef KYTHIRA_BACKUP_PROVIDER_GCS
#include <raft/gcp_gcs_client.hpp>
#endif
#ifdef KYTHIRA_BACKUP_PROVIDER_OCI
#include <raft/oci_client_config_env.hpp>
#include <raft/oci_object_storage_client.hpp>
#endif
#ifdef KYTHIRA_BACKUP_PROVIDER_OSS
#include <raft/alibaba_client_config_env.hpp>
#include <raft/alibaba_oss_client.hpp>
#endif

#include <cstdlib>
#include <exception>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

/// Every provider this design targets, with whether *this* build carries it.
/// Listed unconditionally and filtered at run time, which is the whole point:
/// the absent ones have to be nameable.
struct provider_entry {
    const char* name;
    bool compiled_in;
    const char* enable_hint;
    /// What `--help` lists under "credentials": the variables this provider's
    /// arm reads. One line per `\n`; secrets are only ever read from the
    /// environment, never from flags, because `argv` is readable by every
    /// local user through `ps`.
    const char* credentials_hint;
};

// clang-format off
constexpr provider_entry k_providers[] = {
    {"s3",
#ifdef KYTHIRA_BACKUP_PROVIDER_S3
     true,
#else
     false,
#endif
     "CONFIG_AWS_S3_PERSISTENCE=y, and the AWS SDK must be found",
     "the AWS SDK default chain (AWS_PROFILE, AWS_ACCESS_KEY_ID, ...)"},
    {"azure-blob",
#ifdef KYTHIRA_BACKUP_PROVIDER_AZURE
     true,
#else
     false,
#endif
     "CONFIG_AZURE_BLOB_PERSISTENCE=y, and the Azure SDK must be found",
     "KYTHIRA_AZURE_STORAGE_ACCOUNT, plus the Azure SDK default chain"},
    {"gcs",
#ifdef KYTHIRA_BACKUP_PROVIDER_GCS
     true,
#else
     false,
#endif
     "CONFIG_GCP_STORAGE_PERSISTENCE=y, and vcpkg's opt-in `gcp` feature (--x-feature=gcp)",
     "GOOGLE_CLOUD_PROJECT, plus Application Default Credentials"},
    {"oci-objectstorage",
#ifdef KYTHIRA_BACKUP_PROVIDER_OCI
     true,
#else
     false,
#endif
     "CONFIG_OCI_OBJECT_PERSISTENCE=y (and HTTP_TRANSPORT_TLS, for https)",
     "KYTHIRA_OCI_REGION, and\n"
     "KYTHIRA_OCI_AUTH=api_key|security_token|instance_principal\n"
     "(unset: security_token if KYTHIRA_OCI_SECURITY_TOKEN is set, else api_key)\n"
     "api_key: KYTHIRA_OCI_TENANCY_ID, KYTHIRA_OCI_USER_ID,\n"
     "  KYTHIRA_OCI_FINGERPRINT, and a key\n"
     "security_token: KYTHIRA_OCI_SECURITY_TOKEN, and its session key\n"
     "instance_principal: nothing more\n"
     "a key: KYTHIRA_OCI_PRIVATE_KEY_PEM or KYTHIRA_OCI_PRIVATE_KEY_FILE,\n"
     "  [KYTHIRA_OCI_PRIVATE_KEY_PASSPHRASE]\n"
     "optional: KYTHIRA_OCI_NAMESPACE (skips the GET /n/ lookup),\n"
     "  KYTHIRA_OCI_ENDPOINT_OVERRIDE"},
    {"oss",
#ifdef KYTHIRA_BACKUP_PROVIDER_OSS
     true,
#else
     false,
#endif
     "CONFIG_ALIBABA_OSS_PERSISTENCE=y (and HTTP_TRANSPORT_TLS, for https)",
     "KYTHIRA_ALIBABA_REGION, KYTHIRA_ALIBABA_ACCESS_KEY_ID,\n"
     "KYTHIRA_ALIBABA_ACCESS_KEY_SECRET, [KYTHIRA_ALIBABA_SECURITY_TOKEN]\n"
     "optional: KYTHIRA_ALIBABA_ENDPOINT_OVERRIDE"},
};
// clang-format on

auto names_where(bool compiled_in) -> std::vector<std::string> {
    std::vector<std::string> out;
    for (const auto& entry : k_providers) {
        if (entry.compiled_in == compiled_in) {
            out.emplace_back(entry.name);
        }
    }
    return out;
}

/// The credentials section of `--help`: compiled-in providers only. The
/// absent ones are already listed, with what would enable them.
auto credential_hints() -> std::vector<std::pair<std::string, std::string>> {
    std::vector<std::pair<std::string, std::string>> out;
    for (const auto& entry : k_providers) {
        if (entry.compiled_in) {
            out.emplace_back(entry.name, entry.credentials_hint);
        }
    }
    return out;
}

[[nodiscard]] auto find_provider(std::string_view name) -> const provider_entry* {
    for (const auto& entry : k_providers) {
        if (name == entry.name) {
            return &entry;
        }
    }
    return nullptr;
}

/// Constructs the client and hands it to `run_backup_cli`.
///
/// Construction is inside a `try` because a constructor can do I/O: the OCI
/// client resolves its namespace with `GET /n/` there, and an SDK client can
/// throw on a malformed SDK config. Before this, such a failure escaped `main`
/// and ended in `std::terminate`, so the process died by `SIGABRT` instead of
/// exiting 2 with a message (object-backup-oci-oss-credentials Requirement 4).
template<typename Client, typename Config>
auto run_with_client(Config cfg, std::string_view service, std::string_view hint,
                     const kythira::backup_cli_args& args) -> int {
    std::optional<Client> client;
    try {
        client.emplace(std::move(cfg));
    } catch (const std::exception& err) {
        std::cerr << "raft_object_backup: could not reach " << service << ": " << err.what()
                  << "\n";
        if (!hint.empty()) {
            std::cerr << "  " << hint << "\n";
        }
        return 2;
    }
    return kythira::run_backup_cli(std::move(*client), args, std::cout, std::cerr);
}

/// Prints an environment-built config's warnings and errors. Returns whether
/// the arm may go on; on `false` the caller exits 1, a configuration error.
template<typename Config>
auto report(const kythira::env_config_result<Config>& result, std::string_view provider) -> bool {
    for (const auto& warning : result.warnings) {
        std::cerr << "raft_object_backup: warning: " << warning << "\n";
    }
    for (const auto& error : result.errors) {
        std::cerr << "raft_object_backup: " << error << "\n";
    }
    if (!result.ok()) {
        std::cerr << "  see `raft_object_backup --help` for " << provider << "'s variables\n";
    }
    return result.ok();
}

/// Dispatches to the client `--provider` names.
///
/// S3, Azure and GCS take their credentials from their SDKs' default chains.
/// OCI and Alibaba have no SDK, so nothing fills their configs in unless this
/// tool does: those arms read `KYTHIRA_OCI_*` / `KYTHIRA_ALIBABA_*`, the names
/// the real-cloud suites and CI already export, and report everything missing
/// before any network call.
auto run_for_provider(const kythira::backup_cli_args& args) -> int {
#ifdef KYTHIRA_BACKUP_PROVIDER_S3
    if (args.provider == "s3") {
        return run_with_client<kythira::aws_s3_client>(kythira::aws_client_config{}, "Amazon S3",
                                                       {}, args);
    }
#endif
#ifdef KYTHIRA_BACKUP_PROVIDER_AZURE
    if (args.provider == "azure-blob") {
        // Azure is the one provider whose client needs more than a bucket name:
        // the storage account is part of the origin. The bucket fields carry
        // the *container*, so the account comes from the environment, named the
        // same way the Azure components already read their settings.
        kythira::azure_blob_config cfg;
        const char* account = std::getenv("KYTHIRA_AZURE_STORAGE_ACCOUNT");
        if (account == nullptr) {
            std::cerr << "raft_object_backup: --provider azure-blob needs"
                         " KYTHIRA_AZURE_STORAGE_ACCOUNT set to the storage account name —"
                         " the bucket options name the container, which is not enough to build"
                         " the endpoint\n";
            return 1;
        }
        cfg.account = account;
        return run_with_client<kythira::azure_blob_client>(std::move(cfg), "Azure Blob Storage", {},
                                                           args);
    }
#endif
#ifdef KYTHIRA_BACKUP_PROVIDER_GCS
    if (args.provider == "gcs") {
        kythira::gcp_client_config cfg;
        if (const char* project = std::getenv("GOOGLE_CLOUD_PROJECT"); project != nullptr) {
            cfg.project_id = project;
        }
        return run_with_client<kythira::gcp_gcs_client>(std::move(cfg), "Google Cloud Storage", {},
                                                        args);
    }
#endif
#ifdef KYTHIRA_BACKUP_PROVIDER_OCI
    if (args.provider == "oci-objectstorage") {
        auto env = kythira::oci_object_storage_config_from_env(kythira::process_env_lookup());
        if (!report(env, "oci-objectstorage")) {
            return 1;
        }
        // An unset namespace is resolved at construction via `GET /n/` (task
        // 0.8), so an operator need not know it. That lookup is then the first
        // network call, so its failure is where a bad endpoint or credential
        // shows up, and the hint says how to skip it.
        const bool namespace_unset = env.config.namespace_name.empty();
        return run_with_client<kythira::oci_object_storage_client>(
            std::move(env.config), "OCI Object Storage",
            namespace_unset ? "the tenancy namespace lookup (GET /n/) runs first; set"
                              " KYTHIRA_OCI_NAMESPACE to skip it"
                            : "",
            args);
    }
#endif
#ifdef KYTHIRA_BACKUP_PROVIDER_OSS
    if (args.provider == "oss") {
        auto env = kythira::alibaba_client_config_from_env(kythira::process_env_lookup());
        if (!report(env, "oss")) {
            return 1;
        }
        return run_with_client<kythira::alibaba_oss_client>(std::move(env.config),
                                                            "Alibaba Cloud OSS", {}, args);
    }
#endif
    // Unreachable: `main` has already established that the provider is both
    // known and compiled in. Kept as a loud failure rather than a fallthrough,
    // because silently doing nothing here would look like success.
    std::cerr << "raft_object_backup: internal error — provider \"" << args.provider
              << "\" passed the compiled-in check but has no dispatch arm\n";
    return 2;
}

}  // namespace

auto main(int argc, char** argv) -> int {
    const auto available = names_where(true);
    const auto unavailable = names_where(false);

    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--help" || std::string_view(argv[i]) == "-h") {
            kythira::print_backup_cli_usage(std::cout, argv[0], available, unavailable,
                                            credential_hints());
            return 0;
        }
    }

    kythira::backup_cli_args args;
    try {
        args = kythira::parse_backup_cli_args(argc, argv);
    } catch (const kythira::backup_cli_usage_error& err) {
        std::cerr << "raft_object_backup: " << err.what() << "\n\n";
        kythira::print_backup_cli_usage(std::cerr, argv[0], available, unavailable,
                                        credential_hints());
        return 1;
    }

    if (args.provider.empty()) {
        std::cerr << "raft_object_backup: missing required option --provider\n";
        return 1;
    }

    const auto* entry = find_provider(args.provider);
    if (entry == nullptr) {
        std::cerr << "raft_object_backup: unknown provider \"" << args.provider << "\"\n"
                  << "  known providers: ";
        for (std::size_t i = 0; i < std::size(k_providers); ++i) {
            std::cerr << (i == 0 ? "" : ", ") << k_providers[i].name;
        }
        std::cerr << "\n";
        return 1;
    }
    if (!entry->compiled_in) {
        // Requirement 16.4. The distinction this message draws is the whole
        // reason it exists: the provider is real and spelled correctly, and
        // *this binary* was built without it.
        std::cerr << "raft_object_backup: provider \"" << entry->name
                  << "\" was not compiled into this binary\n"
                  << "  to enable it, rebuild with: " << entry->enable_hint << "\n"
                  << "  compiled into this binary: ";
        if (available.empty()) {
            std::cerr << "(none)";
        }
        for (std::size_t i = 0; i < available.size(); ++i) {
            std::cerr << (i == 0 ? "" : ", ") << available[i];
        }
        std::cerr << "\n";
        return 1;
    }

    // Each arm catches its own construction failure with a targeted message.
    // This is the backstop that keeps a future arm which forgets to from
    // aborting instead of exiting 2 (Requirement 4.2).
    try {
        return run_for_provider(args);
    } catch (const std::exception& err) {
        std::cerr << "raft_object_backup: " << err.what() << "\n";
        return 2;
    }
}
