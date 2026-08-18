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
#include <raft/oci_object_storage_client.hpp>
#endif
#ifdef KYTHIRA_BACKUP_PROVIDER_OSS
#include <raft/alibaba_oss_client.hpp>
#endif

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

/// Every provider this design targets, with whether *this* build carries it.
/// Listed unconditionally and filtered at run time, which is the whole point:
/// the absent ones have to be nameable.
struct provider_entry {
    const char* name;
    bool compiled_in;
    const char* enable_hint;
};

// clang-format off
constexpr provider_entry k_providers[] = {
    {"s3",
#ifdef KYTHIRA_BACKUP_PROVIDER_S3
     true,
#else
     false,
#endif
     "CONFIG_AWS_S3_PERSISTENCE=y, and the AWS SDK must be found"},
    {"azure-blob",
#ifdef KYTHIRA_BACKUP_PROVIDER_AZURE
     true,
#else
     false,
#endif
     "CONFIG_AZURE_BLOB_PERSISTENCE=y, and the Azure SDK must be found"},
    {"gcs",
#ifdef KYTHIRA_BACKUP_PROVIDER_GCS
     true,
#else
     false,
#endif
     "CONFIG_GCP_STORAGE_PERSISTENCE=y, and vcpkg's opt-in `gcp` feature (--x-feature=gcp)"},
    {"oci-objectstorage",
#ifdef KYTHIRA_BACKUP_PROVIDER_OCI
     true,
#else
     false,
#endif
     "CONFIG_OCI_OBJECT_PERSISTENCE=y (and HTTP_TRANSPORT_TLS, for https)"},
    {"oss",
#ifdef KYTHIRA_BACKUP_PROVIDER_OSS
     true,
#else
     false,
#endif
     "CONFIG_ALIBABA_OSS_PERSISTENCE=y (and HTTP_TRANSPORT_TLS, for https)"},
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

[[nodiscard]] auto find_provider(std::string_view name) -> const provider_entry* {
    for (const auto& entry : k_providers) {
        if (name == entry.name) {
            return &entry;
        }
    }
    return nullptr;
}

/// Dispatches to the client `--provider` names.
///
/// Each arm constructs its client from the same configuration struct the
/// corresponding engine takes, so credentials come from wherever that provider
/// already reads them (Requirement 10.6) and nothing about authentication is
/// special-cased for this tool.
auto run_for_provider(const kythira::backup_cli_args& args) -> int {
#ifdef KYTHIRA_BACKUP_PROVIDER_S3
    if (args.provider == "s3") {
        kythira::aws_client_config cfg;
        return kythira::run_backup_cli(kythira::aws_s3_client{cfg}, args, std::cout, std::cerr);
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
        return kythira::run_backup_cli(kythira::azure_blob_client{std::move(cfg)}, args, std::cout,
                                       std::cerr);
    }
#endif
#ifdef KYTHIRA_BACKUP_PROVIDER_GCS
    if (args.provider == "gcs") {
        kythira::gcp_client_config cfg;
        if (const char* project = std::getenv("GOOGLE_CLOUD_PROJECT"); project != nullptr) {
            cfg.project_id = project;
        }
        return kythira::run_backup_cli(kythira::gcp_gcs_client{cfg}, args, std::cout, std::cerr);
    }
#endif
#ifdef KYTHIRA_BACKUP_PROVIDER_OCI
    if (args.provider == "oci-objectstorage") {
        // `namespace_name` left empty on purpose: the client resolves it once
        // at construction via `GET /n/` (task 0.8), so an operator does not have
        // to know their tenancy's namespace to run a restore.
        kythira::oci_object_storage_config cfg;
        return kythira::run_backup_cli(kythira::oci_object_storage_client{std::move(cfg)}, args,
                                       std::cout, std::cerr);
    }
#endif
#ifdef KYTHIRA_BACKUP_PROVIDER_OSS
    if (args.provider == "oss") {
        kythira::alibaba_client_config cfg;
        return kythira::run_backup_cli(kythira::alibaba_oss_client{std::move(cfg)}, args, std::cout,
                                       std::cerr);
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
            kythira::print_backup_cli_usage(std::cout, argv[0], available, unavailable);
            return 0;
        }
    }

    kythira::backup_cli_args args;
    try {
        args = kythira::parse_backup_cli_args(argc, argv);
    } catch (const kythira::backup_cli_usage_error& err) {
        std::cerr << "raft_object_backup: " << err.what() << "\n\n";
        kythira::print_backup_cli_usage(std::cerr, argv[0], available, unavailable);
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

    return run_for_provider(args);
}
