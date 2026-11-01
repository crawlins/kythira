// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// kythira::node instantiations for chaos_node; see run_node.hpp for why they
// are split out of main.cpp.

#include "run_node.hpp"

#ifdef KYTHIRA_CHAOS_NODE_RPC_TLS
CHAOS_NODE_INSTANTIATE_RUN_NODE(tcp_raft_types_with_tls)
#endif
#ifdef KYTHIRA_HAS_AWS_SDK
CHAOS_NODE_INSTANTIATE_RUN_NODE(tcp_raft_types_with_aws_qm)
#endif
