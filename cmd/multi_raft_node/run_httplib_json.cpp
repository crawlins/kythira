// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file run_httplib_json.cpp
/// @brief The single `multi_raft` instantiation for `--transport httplib
///        --serializer json`, alone in its own translation unit.
///
/// One of four. Splitting them is what keeps any one of these under the
/// compiler-memory ceiling a 16 GiB CI runner sets; `host_stacks.hpp`
/// carries the measurement and the CI failure that motivated it.

///
/// It is also the one stack elastic capacity is compiled into
/// (`--capacity-role`, `.kiro/specs/elastic-shard-capacity/` task 16). One
/// instantiation serves both: with the role off the extension below adds
/// nothing, so the measurement host is unchanged.

#include "capacity_plane.hpp"
#include "host_runners.hpp"
#include "host_stacks.hpp"

#include <cstddef>
#include <memory>
#include <vector>

namespace kythira::bench::host {

namespace {

/// Owns whichever side of the control plane this host is, for as long as
/// `run_host` runs.
struct capacity_extension {
    std::shared_ptr<capacity::capacity_service> _service;
    std::shared_ptr<capacity::capacity_client> _client;

    template<typename Config> auto configure(Config& cfg, const node_options& opt) -> void {
        switch (opt._capacity_role) {
            case capacity_role::off:
                return;
            case capacity_role::controller:
                _service = std::make_shared<capacity::capacity_service>(opt);
                _service->start();
                capacity::wire_hooks(cfg, opt, *_service);
                return;
            case capacity_role::member:
                _client = std::make_shared<capacity::capacity_client>(opt._capacity_controller,
                                                                      opt._capacity_token);
                capacity::wire_hooks(cfg, opt, *_client);
                return;
        }
    }
};

}  // namespace

auto run_httplib_json(const node_options& opt) -> int {
    return run_host<httplib_stack<kythira::json_rpc_serializer<std::vector<std::byte>>>>(
        opt, capacity_extension{});
}

}  // namespace kythira::bench::host
