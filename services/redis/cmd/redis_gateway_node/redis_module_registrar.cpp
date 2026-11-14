// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// The Redis module's registration in kythira_node
// (.kiro/specs/service-colocation/ task 7). kythira_node links this
// library whole-archive (KYTHIRA_NODE_MODULES), so the registrar below runs
// and KYTHIRA_SERVICES=redis finds the module; nothing names it.

#include "node_serving_factories.hpp"
#include "redis_module.hpp"

#include <raft/kythira_node_host.hpp>
#include <raft/module_registry.hpp>

#include <memory>

namespace {

namespace sn = kythira::service_node;
namespace rn = kythira::redis_node;

const sn::module_registrar<sn::kythira_node_host> k_redis_registrar("redis", [] {
    return std::make_unique<rn::redis_module<sn::kythira_node_host>>(
        false,
        rn::redis_module<sn::kythira_node_host>::factories{
            .memory = rn::node_memory_serving(),
            .file = rn::node_file_serving(),
#if defined(KYTHIRA_REDIS_GATEWAY_ENGINE)
            .engine = rn::node_engine_serving(),
#else
            .engine = {},
#endif
        },
        sn::kythira_node_stack::k_max_body_bytes);
});

}  // namespace
