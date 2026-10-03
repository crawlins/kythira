// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Design §2's capability table (.kiro/specs/coap-transport-multi-raft/), as
// code. One row per CoAP backend, one column per optional transport extension
// concept in raft/network.hpp. The three coap_*_concept_conformance_test.cpp
// files each check their backend against its row, in both directions, so a
// backend that quietly gains or loses an extension fails a test instead of
// silently changing which multi-Raft features work on it (Requirement 2.5).
//
// The backend headers carry the same answers as static_asserts against
// coap_detail::conformance_types; these rows are checked against each test's
// own Types bundle, which is what a caller would actually instantiate.
//
// Keep this file, those header blocks and the table in design.md in step. A
// change to one that is not a change to all three is the drift this exists
// to catch.

#include <raft/network.hpp>

namespace kythira::testing {

struct coap_capabilities {
    bool pre_vote;
    bool log_fetch;
    bool cluster_join;
    bool cluster_leave;
    bool timeout_now;
};

inline constexpr coap_capabilities libcoap_capabilities{
    .pre_vote = false,
    .log_fetch = true,
    .cluster_join = false,
    .cluster_leave = false,
    .timeout_now = true,
};

inline constexpr coap_capabilities libnyoci_capabilities{
    .pre_vote = false,
    .log_fetch = true,
    .cluster_join = false,
    .cluster_leave = false,
    .timeout_now = true,
};

inline constexpr coap_capabilities cantcoap_capabilities{
    .pre_vote = false,
    .log_fetch = true,
    .cluster_join = false,
    .cluster_leave = false,
    .timeout_now = true,
};

/// Fails to compile unless Client and Server satisfy the base concepts and
/// exactly the extensions `expected` names, client and server alike.
template<typename Client, typename Server, coap_capabilities expected>
constexpr auto assert_coap_capabilities() -> bool {
    static_assert(kythira::network_client<Client>);
    static_assert(kythira::network_server<Server>);

    static_assert(kythira::network_client_with_pre_vote<Client> == expected.pre_vote);
    static_assert(kythira::network_server_with_pre_vote<Server> == expected.pre_vote);
    static_assert(kythira::network_client_with_log_fetch<Client> == expected.log_fetch);
    static_assert(kythira::network_server_with_log_fetch<Server> == expected.log_fetch);
    static_assert(kythira::network_client_with_cluster_join<Client> == expected.cluster_join);
    static_assert(kythira::network_server_with_cluster_join<Server> == expected.cluster_join);
    static_assert(kythira::network_client_with_cluster_leave<Client> == expected.cluster_leave);
    static_assert(kythira::network_server_with_cluster_leave<Server> == expected.cluster_leave);
    static_assert(kythira::network_client_with_timeout_now<Client> == expected.timeout_now);
    static_assert(kythira::network_server_with_timeout_now<Server> == expected.timeout_now);
    return true;
}

}  // namespace kythira::testing
