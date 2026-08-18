#pragma once

/// @file object_store_backup_cli.hpp
/// @brief The verb dispatch, argument parsing and output formatting behind
///        `cmd/raft_object_backup` — everything about that binary except which
///        provider it talks to.
///
/// ## Why this is a header and not just a `main.cpp`
///
/// Requirement 10.6 exists because **operators do not have a C++ compiler in a
/// recovery window**, which means this tool runs on the worst day and must not
/// be the thing that surprises anyone. Task 14's verify bar asks for a full
/// create → list → verify → restore-clone cycle against `mock_object_store` in
/// a build with **zero** cloud providers, and that is only testable if the
/// logic is separable from provider selection.
///
/// So everything here is generic over `key_object_store` and writes to an
/// injected `std::ostream`. `main.cpp` does exactly two things this file does
/// not: it maps `--provider` onto a compiled-in client, and it reports by name
/// the providers that were not compiled in (Requirement 16.4).
///
/// ## Exit codes, and why `verify` gets its own
///
/// | Code | Meaning |
/// |---|---|
/// | 0 | the operation succeeded; for `verify`, the backup is clean |
/// | 1 | usage error — bad or missing arguments, unknown or uncompiled provider |
/// | 2 | the operation failed — the store threw, or a restore was refused |
/// | 3 | `verify` completed and **found problems** |
///
/// 3 is separated from 2 deliberately. "I could not check this backup" and
/// "I checked it and it is broken" call for different operator responses, and a
/// script that gates a restore on `verify` needs to tell them apart — the first
/// is worth retrying, the second never is.

#include <raft/key_object_store.hpp>
#include <raft/object_store_backup.hpp>

#include <exception>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace kythira {

/// @brief The verbs. Spelled as the operator types them.
enum class backup_verb {
    create,
    list,
    verify,
    restore_clone,
    restore_seed
};

struct backup_cli_args {
    backup_verb verb{backup_verb::list};
    std::string provider;

    std::string source_bucket;
    std::string source_prefix;
    std::string dest_bucket;
    std::string dest_prefix;
    std::string target_bucket;
    std::string target_prefix;
    std::string backup_id;
    std::vector<std::string> nodes;
    bool quiesced{false};
    bool force{false};
};

/// @brief Thrown by `parse_backup_cli_args` for anything the operator can fix
///        by retyping the command. Always exit code 1.
class backup_cli_usage_error : public std::runtime_error {
public:
    explicit backup_cli_usage_error(const std::string& what) : std::runtime_error(what) {}
};

namespace object_store_backup_cli_detail {

inline constexpr int k_exit_ok = 0;
inline constexpr int k_exit_usage = 1;
inline constexpr int k_exit_failed = 2;
inline constexpr int k_exit_verify_problems = 3;

[[nodiscard]] inline auto split_commas(std::string_view value) -> std::vector<std::string> {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start <= value.size()) {
        const auto comma = value.find(',', start);
        const auto piece = value.substr(
            start, comma == std::string_view::npos ? std::string_view::npos : comma - start);
        if (!piece.empty()) {
            out.emplace_back(piece);
        }
        if (comma == std::string_view::npos) {
            break;
        }
        start = comma + 1;
    }
    return out;
}

/// Names a missing option the way the operator will have to fix it — the flag,
/// not the field.
inline auto require(const std::string& value, const char* flag) -> void {
    if (value.empty()) {
        throw backup_cli_usage_error(std::string("missing required option ") + flag);
    }
}

}  // namespace object_store_backup_cli_detail

/// @brief The usage text. Both restore modes appear as **distinct verbs** with
///        their own descriptions, because the entire point of separating them
///        is that they cannot be confused for one another — a `--mode` flag on
///        one verb would undo that.
inline auto print_backup_cli_usage(std::ostream& out, std::string_view argv0,
                                   const std::vector<std::string>& available_providers,
                                   const std::vector<std::string>& unavailable_providers) -> void {
    out << "usage: " << argv0 << " <verb> --provider <name> [options]\n"
        << "\n"
        << "Back up, inspect and restore a Raft node's cloud-object state.\n"
        << "\n"
        << "verbs:\n"
        << "  create          copy a node's prefix into a new backup\n"
        << "  list            list the finished backups under a destination prefix\n"
        << "  verify          check a backup against itself and report every problem\n"
        << "  restore-clone   reproduce a backup into a target prefix, byte for byte.\n"
        << "                  For replacing storage or an instance under an UNCHANGED\n"
        << "                  node identity. Safe to start only if the original node is\n"
        << "                  definitively gone — starting both is a split-brain.\n"
        << "  restore-seed    build a NEW cluster's initial state from a backup's\n"
        << "                  snapshot. Keeps the state machine, REPLACES the cluster\n"
        << "                  configuration, empties the log and clears the vote.\n"
        << "\n"
        << "options:\n"
        << "  --provider <name>        which object store to talk to\n"
        << "  --source-bucket <name>   create: the bucket holding the node\n"
        << "  --source-prefix <path>   create: the node's prefix, no trailing slash\n"
        << "  --dest-bucket <name>     the bucket holding backups\n"
        << "  --dest-prefix <path>     the root prefix backups live under\n"
        << "  --backup-id <id>         verify/restore: which backup; create: override\n"
        << "                           the timestamp-derived default\n"
        << "  --target-bucket <name>   restore: where to write\n"
        << "  --target-prefix <path>   restore-clone: the node prefix to write.\n"
        << "                           restore-seed: the parent of one prefix per node\n"
        << "  --nodes <a,b,c>          restore-seed: the NEW cluster's node ids\n"
        << "  --quiesced               create: record that the source was not being\n"
        << "                           written. Recorded, not believed — verify checks\n"
        << "                           the copy against itself regardless\n"
        << "  --force                  restore: proceed into a non-empty target,\n"
        << "                           DELETING its engine-owned keys first. Never\n"
        << "                           merges\n"
        << "  --help                   this text\n"
        << "\n"
        << "exit codes:\n"
        << "  0  success; for verify, the backup is clean\n"
        << "  1  usage error, or a provider that was not compiled in\n"
        << "  2  the operation failed\n"
        << "  3  verify found problems (distinct from 2: checked and broken, rather\n"
        << "     than could not be checked)\n"
        << "\n"
        << "providers compiled into this binary:\n";
    if (available_providers.empty()) {
        out << "  (none — this build has every cloud provider disabled)\n";
    }
    for (const auto& name : available_providers) {
        out << "  " << name << "\n";
    }
    if (!unavailable_providers.empty()) {
        out << "\nnot compiled into this binary:\n";
        for (const auto& name : unavailable_providers) {
            out << "  " << name << "\n";
        }
    }
}

/// @brief Parse argv into `backup_cli_args`, or throw `backup_cli_usage_error`.
///
/// Per-verb requirements are checked here rather than at dispatch, so an
/// operator learns about a missing `--backup-id` before the tool has talked to
/// anything.
[[nodiscard]] inline auto parse_backup_cli_args(int argc, const char* const* argv)
    -> backup_cli_args {
    namespace detail = object_store_backup_cli_detail;

    if (argc < 2) {
        throw backup_cli_usage_error("no verb given");
    }

    backup_cli_args args;
    const std::string_view verb = argv[1];
    if (verb == "create") {
        args.verb = backup_verb::create;
    } else if (verb == "list") {
        args.verb = backup_verb::list;
    } else if (verb == "verify") {
        args.verb = backup_verb::verify;
    } else if (verb == "restore-clone") {
        args.verb = backup_verb::restore_clone;
    } else if (verb == "restore-seed") {
        args.verb = backup_verb::restore_seed;
    } else {
        throw backup_cli_usage_error("unknown verb \"" + std::string(verb) +
                                     "\" — expected create, list, verify, restore-clone or"
                                     " restore-seed");
    }

    for (int i = 2; i < argc; ++i) {
        const std::string_view arg = argv[i];
        const auto value = [&]() -> std::string {
            if (i + 1 >= argc) {
                throw backup_cli_usage_error(std::string(arg) + " needs a value");
            }
            return argv[++i];
        };
        if (arg == "--provider") {
            args.provider = value();
        } else if (arg == "--source-bucket") {
            args.source_bucket = value();
        } else if (arg == "--source-prefix") {
            args.source_prefix = value();
        } else if (arg == "--dest-bucket") {
            args.dest_bucket = value();
        } else if (arg == "--dest-prefix") {
            args.dest_prefix = value();
        } else if (arg == "--target-bucket") {
            args.target_bucket = value();
        } else if (arg == "--target-prefix") {
            args.target_prefix = value();
        } else if (arg == "--backup-id") {
            args.backup_id = value();
        } else if (arg == "--nodes") {
            args.nodes = detail::split_commas(value());
        } else if (arg == "--quiesced") {
            args.quiesced = true;
        } else if (arg == "--force") {
            args.force = true;
        } else {
            throw backup_cli_usage_error("unknown option \"" + std::string(arg) + "\"");
        }
    }

    detail::require(args.dest_bucket, "--dest-bucket");
    detail::require(args.dest_prefix, "--dest-prefix");
    switch (args.verb) {
        case backup_verb::create:
            detail::require(args.source_bucket, "--source-bucket");
            detail::require(args.source_prefix, "--source-prefix");
            break;
        case backup_verb::list:
            break;
        case backup_verb::verify:
            detail::require(args.backup_id, "--backup-id");
            break;
        case backup_verb::restore_seed:
            if (args.nodes.empty()) {
                throw backup_cli_usage_error(
                    "restore-seed needs --nodes: it is building a NEW cluster's membership, and"
                    " that is not something the tool can infer from the backup");
            }
            [[fallthrough]];
        case backup_verb::restore_clone:
            detail::require(args.backup_id, "--backup-id");
            detail::require(args.target_bucket, "--target-bucket");
            detail::require(args.target_prefix, "--target-prefix");
            break;
    }
    return args;
}

/// @brief Run one verb against @p store, printing to @p out.
///
/// Returns the process exit code. Throws nothing: a store failure or a refused
/// restore is caught and reported, because a stack trace is not what an
/// operator in a recovery window needs from a tool.
template<key_object_store Store>
auto run_backup_cli(Store store, const backup_cli_args& args, std::ostream& out, std::ostream& err)
    -> int {
    namespace detail = object_store_backup_cli_detail;

    const object_store_backup<Store> backup{std::move(store)};
    const backup_destination dst{args.dest_bucket, args.dest_prefix};

    try {
        switch (args.verb) {
            case backup_verb::create: {
                const backup_source src{args.source_bucket, args.source_prefix};
                const auto manifest = backup.create(
                    src, dst, {.backup_id = args.backup_id, .source_quiesced = args.quiesced});
                out << "created " << manifest.backup_id << "\n"
                    << "  objects   " << manifest.objects.size() << "\n"
                    << "  provider  " << manifest.provider << "\n"
                    << "  source    " << manifest.source_bucket << "/" << manifest.source_prefix
                    << "\n"
                    << "  quiesced  " << (manifest.source_quiesced ? "declared" : "not declared")
                    << "\n";
                if (!manifest.source_quiesced) {
                    out << "  note      the source was not declared quiesced, so this copy may be"
                           " a smear across the copy window — run verify\n";
                }
                return detail::k_exit_ok;
            }
            case backup_verb::list: {
                const auto listed = backup.list(dst);
                if (listed.empty()) {
                    out << "no finished backups under " << args.dest_bucket << "/"
                        << args.dest_prefix << "\n";
                    return detail::k_exit_ok;
                }
                for (const auto& manifest : listed) {
                    out << manifest.backup_id << "  " << manifest.created_at << "  "
                        << manifest.objects.size() << " objects  "
                        << (manifest.source_quiesced ? "quiesced" : "live") << "  from "
                        << manifest.source_bucket << "/" << manifest.source_prefix << "\n";
                }
                return detail::k_exit_ok;
            }
            case backup_verb::verify: {
                const auto report = backup.verify(dst, args.backup_id);
                if (report.ok) {
                    out << "verified " << report.manifest.backup_id << " — no problems\n";
                    return detail::k_exit_ok;
                }
                out << "verified " << report.manifest.backup_id << " — " << report.problems.size()
                    << " problem(s)\n";
                for (const auto& problem : report.problems) {
                    out << "  " << problem.kind << "  " << problem.subject << "\n"
                        << "      " << problem.detail << "\n";
                }
                return detail::k_exit_verify_problems;
            }
            case backup_verb::restore_clone: {
                const backup_source target{args.target_bucket, args.target_prefix};
                const auto report =
                    backup.restore_clone(dst, args.backup_id, target, {.force = args.force});
                out << "restored " << report.backup_id << " into " << args.target_bucket << "/"
                    << args.target_prefix << "\n"
                    << "  objects written  " << report.objects_written << "\n";
                if (!report.keys_deleted.empty()) {
                    out << "  keys deleted     " << report.keys_deleted.size() << " (--force)\n";
                }
                if (report.owner_epoch) {
                    out << "  owner epoch      " << *report.owner_epoch
                        << " — a returning original fences itself out on its next write\n";
                } else {
                    out << "  owner epoch      none; this backup was taken unfenced, so a"
                           " returning original will NOT fence itself out\n";
                }
                out << "\nStart this node only if the original is definitively gone. Running"
                       " both is a split-brain.\n";
                return detail::k_exit_ok;
            }
            case backup_verb::restore_seed: {
                const backup_source target{args.target_bucket, args.target_prefix};
                const auto report = backup.restore_seed(dst, args.backup_id, target, args.nodes,
                                                        {.force = args.force});
                out << "seeded a new cluster from " << report.backup_id << "\n"
                    << "  objects written  " << report.objects_written << "\n";
                if (!report.keys_deleted.empty()) {
                    out << "  keys deleted     " << report.keys_deleted.size() << " (--force)\n";
                }
                out << "\nConfigure the new cluster with exactly these nodes and prefixes:\n";
                for (const auto& node : report.nodes) {
                    out << "  " << node.node_id << "  " << args.target_bucket << "/" << node.prefix
                        << "\n";
                }
                out << "\nThese identities are unrelated to the old cluster's. The log is empty,"
                       " the vote is cleared,\nand the configuration is the node set above — not"
                       " whatever the backup recorded.\n";
                return detail::k_exit_ok;
            }
        }
        return detail::k_exit_usage;
    } catch (const std::exception& ex) {
        err << "raft_object_backup: " << ex.what() << "\n";
        return detail::k_exit_failed;
    }
}

}  // namespace kythira
