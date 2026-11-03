// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// kythira::node instantiation for chaos_node's PERSISTENCE=s3 bundle; see
// run_node.hpp for why instantiations are split out of main.cpp. On its own
// because the object-store engine is the heaviest persistence template in
// the tree, and pairing it with another bundle would undo the split.

#include "run_node.hpp"

#ifdef KYTHIRA_HAS_AWS_SDK
CHAOS_NODE_INSTANTIATE_RUN_NODE(tcp_raft_types_with_s3)
#endif
