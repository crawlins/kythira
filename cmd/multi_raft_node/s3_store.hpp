// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file s3_store.hpp
/// @brief The startup preflight for `--persistence s3`
///        (`.kiro/specs/multi-raft-node-s3-persistence/`, Requirement 5.3).
///
/// Instantiates the object engine over a store with nothing in it, never over
/// `aws_s3_client`, so `main.cpp` and the unit tests can use it in a build
/// without the S3 arm. The engines over the real client are in
/// `s3_store.cpp`.

#include "config.hpp"
#include "s3_describe.hpp"

#include <raft/key_object_store.hpp>
#include <raft/object_store_persistence.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace kythira::bench {

namespace detail {

/// @brief A store with nothing in it that accepts every write and keeps none.
///
/// The preflight's store: an engine constructed over it runs the whole of its
/// option validation, loads an empty prefix, and under `cas` claims it, all
/// with no network. It lives here, not in `tests/`, because the host itself
/// uses it.
struct preflight_store {
    auto put_object(const std::string&, const std::string&, std::string_view) const
        -> kythira::put_result {
        return {"preflight"};
    }
    [[nodiscard]] auto get_object(const std::string&, const std::string&) const
        -> std::optional<kythira::get_result> {
        return std::nullopt;
    }
    auto delete_object(const std::string&, const std::string&) const -> void {}
    [[nodiscard]] auto list_keys(const std::string&, const std::string&) const
        -> std::vector<std::string> {
        return {};
    }
    [[nodiscard]] static auto provider_name() -> std::string_view { return "preflight"; }
    auto put_object_if(const std::string&, const std::string&, std::string_view,
                       const kythira::precondition&) const -> kythira::put_result {
        return {"preflight"};
    }
    auto delete_object_if(const std::string&, const std::string&,
                          const kythira::precondition&) const -> void {}
};

}  // namespace detail

/// @brief Refuse startup on any engine option the engine itself rejects,
///        with the engine's own message, before a server binds
///        (Requirement 5.3).
///
/// The parser does not repeat the engine's rules, so there is one rule and
/// one message. Without this a bad `--s3-max-segment-entries` would surface
/// inside `create_group` for group 1, after the transport was already up.
///
/// @throws std::invalid_argument as the engine throws it.
inline auto preflight_s3_options(const node_options& opt) -> void {
    const auto engine = s3_engine_options(opt);
    if (opt._s3.fencing == s3_fencing::cas) {
        kythira::fenced_object_store_persistence_engine<detail::preflight_store,
                                                        kythira::host_node_id>
            probe{detail::preflight_store{}, "preflight", "preflight", engine};
    } else {
        kythira::object_store_persistence_engine<detail::preflight_store, kythira::host_node_id>
            probe{detail::preflight_store{}, "preflight", "preflight", engine};
    }
}

}  // namespace kythira::bench
