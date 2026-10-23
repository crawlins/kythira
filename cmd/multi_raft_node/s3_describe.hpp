// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file s3_describe.hpp
/// @brief Where `--persistence s3` puts a host's state, the options each group
///        is built with, and how both are spelled at startup and on the status
///        surface (`.kiro/specs/multi-raft-node-s3-persistence/`).
///
/// SDK-free and engine-free, so `control_server.hpp`, which every `run_*.cpp`
/// includes, adds nothing heavy to those translation units.

#include "config.hpp"

#include <raft/group_storage.hpp>
#include <raft/object_persistence_options.hpp>

#include <chrono>
#include <sstream>
#include <string>
#include <string_view>

namespace kythira::bench {

/// `<prefix>/nodes/<node>`: where this host's groups live (Requirement 2.3).
/// The node id is made key-safe: in a text-id build a slash or colon in it
/// becomes an underscore.
[[nodiscard]] inline auto s3_host_prefix(const node_options& opt) -> std::string {
    return opt._s3.prefix + "/nodes/" +
           kythira::group_path_component(kythira::host_node_id_text(opt._node_id));
}

/// @brief Who this host says it is in each group's `<prefix>/owner` object
///        under `cas`: `<node-id>@<own Raft URL>`.
///
/// **Not the bare node id, which is what the spec first said, and the
/// difference is the whole of the duplicate-host guarantee.** The engine
/// treats a recorded owner equal to its own `owner_id` as a restart and claims
/// the next epoch, by design: a crash-restart and a duplicate with the same id
/// are indistinguishable at construction. With the bare node id a second host
/// started under a reused id would therefore start, and the fence would fall
/// on whichever of the two wrote second, possibly the original. Adding the
/// host's own Raft URL, which peers dial and so is unique per live process,
/// keeps a restart with the same flags a restart (same owner, next epoch) and
/// makes a second process under the same id a different owner, refused at
/// construction with the engine's message (Requirement 4.4). The cost: a host
/// that comes back on a different address under the same id and prefix is
/// refused too, and needs a fresh prefix. Measured rows use run-scoped
/// prefixes, so that is the case already.
///
/// Falls back to the bare id when this host is not yet in its own peer map,
/// which happens only in the preflight before discovery has run.
[[nodiscard]] inline auto s3_owner_id(const node_options& opt) -> std::string {
    auto id = kythira::host_node_id_text(opt._node_id);
    if (const auto self = opt._peers.find(opt._node_id); self != opt._peers.end()) {
        id += "@" + self->second;
    }
    return id;
}

/// The engine options a group is built with: the parsed ones, plus
/// `owner_id` under `cas` (see `s3_owner_id`).
[[nodiscard]] inline auto s3_engine_options(const node_options& opt)
    -> kythira::object_persistence_options {
    auto engine = opt._s3.engine;
    if (opt._s3.fencing == s3_fencing::cas) {
        engine.owner_id = s3_owner_id(opt);
    }
    return engine;
}

/// `aws_client_config::api_timeout` is whole seconds; `--s3-timeout` is
/// milliseconds like every other duration flag. Rounded up, so a short
/// timeout never becomes zero.
[[nodiscard]] inline auto s3_api_timeout(std::chrono::milliseconds timeout)
    -> std::chrono::seconds {
    return std::chrono::ceil<std::chrono::seconds>(timeout);
}

/// The startup line and the status surface's spellings (Requirement 5.4).
[[nodiscard]] inline auto s3_fencing_name(s3_fencing fencing) -> std::string_view {
    return fencing == s3_fencing::cas ? "cas" : "none";
}
[[nodiscard]] inline auto s3_hard_state_name(kythira::hard_state_layout layout)
    -> std::string_view {
    return layout == kythira::hard_state_layout::combined ? "combined" : "separate";
}
[[nodiscard]] inline auto s3_log_name(kythira::log_layout layout) -> std::string_view {
    return layout == kythira::log_layout::segmented ? "segmented" : "per-entry";
}

/// One line naming where this host's state lives and every option it runs
/// with, printed once at startup so a row's log records both
/// (Requirements 2.4 and 5.4).
[[nodiscard]] inline auto describe_s3(const node_options& opt, std::string_view host_prefix)
    -> std::string {
    const auto& s3 = opt._s3;
    const auto& e = s3.engine;
    std::ostringstream out;
    out << "multi_raft_node: persistence s3: s3://" << s3.bucket << "/" << host_prefix
        << " endpoint=" << (s3.endpoint.empty() ? "aws" : s3.endpoint)
        << " region=" << (s3.region.empty() ? "(sdk chain)" : s3.region)
        << " timeout_ms=" << s3.timeout.count() << " fencing=" << s3_fencing_name(s3.fencing)
        << " hard_state=" << s3_hard_state_name(e.hard_state) << " log=" << s3_log_name(e.log)
        << " max_segment_entries=" << e.max_segment_entries
        << " max_segment_bytes=" << e.max_segment_bytes
        << " write_behind=" << (e.write_behind ? 1 : 0)
        << " append_concurrency=" << e.append_concurrency << " write_retries=" << e.write_retries
        << " snapshot_retention=" << e.snapshot_retention
        << " verify_checksums=" << (e.verify_checksums ? 1 : 0)
        << " max_object_bytes=" << e.max_object_bytes;
    return out.str();
}

}  // namespace kythira::bench
