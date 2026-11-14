// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// The Kinesis module's registration in kythira_node
// (.kiro/specs/service-colocation/ task 5). kythira_node links this
// library whole-archive (KYTHIRA_NODE_MODULES), so the registrar below runs
// and KYTHIRA_SERVICES=kinesis finds the module; nothing names it.

#include "kinesis_module.hpp"

#include <raft/kythira_node_host.hpp>
#include <raft/module_registry.hpp>

#include <memory>

namespace {

namespace sn = kythira::service_node;

const sn::module_registrar<sn::kythira_node_host> k_kinesis_registrar("kinesis", [] {
    return std::make_unique<kythira::kinesis_node::kinesis_module<sn::kythira_node_host>>(false);
});

}  // namespace
