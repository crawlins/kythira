// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// kythira::node instantiations for chaos_node; see run_node.hpp for why they
// are split out of main.cpp.

#include "run_node.hpp"

CHAOS_NODE_INSTANTIATE_RUN_NODE(tcp_raft_types_joinable)
CHAOS_NODE_INSTANTIATE_RUN_NODE(tcp_raft_types_with_docker_qm)
