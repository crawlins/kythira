// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file aws_real_ec2_test_support.hpp
/// @brief Shared real-EC2 integration test infrastructure: AWS cost
/// estimation/reporting, signal-driven cleanup, and teardown helpers that
/// wait for instance termination and retry network deletes.
///
/// Originally implemented once, only in aws_quorum_manager_real_ec2_test.cpp
/// (aws-quorum-manager spec Requirements 20/21). Extracted here
/// (.kiro/specs/ca-cluster-rpc-mtls-real-aws/, Requirements 6/7) so every
/// real-EC2 test binary in this project gets both, not just the first one
/// that needed them — ca_cluster_node_real_ec2_test.cpp had neither and
/// would otherwise leak a VPC and running EC2 instances if killed mid-run.
///
/// The cost, signal and generic teardown-delete pieces live in
/// aws_real_test_support.hpp (included below) so suites that use no EC2 can
/// share them; this header adds the EC2-specific teardown wait.
///
/// Header-only, included directly by each real-EC2 test .cpp (no separate
/// translation unit). `g_cost_accumulator` and `g_active_aws_fixture` are
/// declared `inline` (not `static`) so each including *binary* gets exactly
/// one definition, even though several separate test binaries each include
/// this same header — this is a header-only, multi-binary library, not a
/// single shared translation unit.

#include "aws_real_test_support.hpp"

#include <chrono>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <aws/ec2/EC2Client.h>
#include <aws/ec2/model/DescribeInstancesRequest.h>
#include <aws/ec2/model/Filter.h>

namespace kythira::testing::aws_real_ec2 {

// ── Teardown helpers ────────────────────────────────────────────────────────
//
// A terminated instance's network interface detaches asynchronously, and
// until it does EC2 rejects DeleteSecurityGroup and DeleteSubnet with
// DependencyViolation, and DetachInternetGateway while it still maps a
// public address. Fixtures that slept a fixed interval after
// TerminateInstances and then issued each delete once, ignoring the result,
// leaked their whole network shell whenever that interval was too short:
// the subnet and security group survived, so the retried DeleteVpc failed
// for its whole budget too. These helpers wait on the real instance state
// and retry every network delete, logging whatever is still failing.

// Waits until no instance in `vpc_id` is in any state but `terminated`.
// Returns false (after logging the survivors) when `budget` runs out.
inline auto wait_vpc_instances_terminated(Aws::EC2::EC2Client& ec2, const std::string& vpc_id,
                                          std::chrono::seconds budget, std::string_view log_tag)
    -> bool {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    std::vector<std::string> alive;
    std::string last_error;
    for (;;) {
        Aws::EC2::Model::DescribeInstancesRequest req;
        Aws::EC2::Model::Filter vpc_filter;
        vpc_filter.SetName("vpc-id");
        vpc_filter.AddValues(vpc_id);
        req.AddFilters(vpc_filter);
        Aws::EC2::Model::Filter state_filter;
        state_filter.SetName("instance-state-name");
        for (const char* st : {"pending", "running", "shutting-down", "stopping", "stopped"}) {
            state_filter.AddValues(st);
        }
        req.AddFilters(state_filter);
        auto out = ec2.DescribeInstances(req);
        if (out.IsSuccess()) {
            alive.clear();
            last_error.clear();
            for (const auto& res : out.GetResult().GetReservations()) {
                for (const auto& inst : res.GetInstances()) {
                    alive.emplace_back(inst.GetInstanceId());
                }
            }
            if (alive.empty()) {
                return true;
            }
        } else {
            last_error = std::string(out.GetError().GetExceptionName()) + ": " +
                         std::string(out.GetError().GetMessage());
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::seconds{5});
    }
    std::cerr << "[" << log_tag << "] teardown: instances in " << vpc_id
              << " not terminated within " << budget.count() << "s:";
    for (const auto& id : alive) {
        std::cerr << " " << id;
    }
    if (!last_error.empty()) {
        std::cerr << " (last DescribeInstances error: " << last_error << ")";
    }
    std::cerr << "\n";
    return false;
}

}  // namespace kythira::testing::aws_real_ec2
