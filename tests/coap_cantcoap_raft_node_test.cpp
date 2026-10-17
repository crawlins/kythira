// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file coap_cantcoap_raft_node_test.cpp
/// @brief `node<Types>` over the cantcoap CoAP backend: election, replication
///        and TimeoutNow leadership transfer on three loopback nodes
///        (.kiro/specs/coap-transport-multi-raft/ task 7). The cases live in
///        coap_alternate_raft_node_cases.hpp, shared with the libnyoci suite.

#include "test_timeout_scale.hpp"
#define BOOST_TEST_MODULE coap_cantcoap_raft_node_test
#include <boost/test/unit_test.hpp>

// Deliberately NOT raft/coap_transport.hpp: libcoap's and libnyoci's headers
// cannot share a translation unit (see coap_transport_libnyoci_impl.hpp).
#include <raft/coap_transport_cantcoap_impl.hpp>

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
#include <folly/init/Init.h>

#include <memory>

namespace {
struct folly_init_fixture {
    folly_init_fixture() {
        int argc = 1;
        char* argv_data[] = {const_cast<char*>("coap_cantcoap_raft_node_test"), nullptr};
        char** argv = argv_data;
        _init = std::make_unique<folly::Init>(&argc, &argv);
    }
    std::unique_ptr<folly::Init> _init;
};
}  // namespace
BOOST_GLOBAL_FIXTURE(folly_init_fixture);
#endif

#ifdef CANTCOAP_AVAILABLE

namespace raft_node_cases {

struct backend {
    template<typename Types> using client = kythira::coap_cantcoap_client<Types>;
    template<typename Types> using server = kythira::coap_cantcoap_server<Types>;
};

}  // namespace raft_node_cases

#include "coap_alternate_raft_node_cases.hpp"

#else  // CANTCOAP_AVAILABLE

BOOST_AUTO_TEST_CASE(test_cantcoap_raft_node_skipped_without_cantcoap) {
    BOOST_TEST_MESSAGE("cantcoap not available; the cantcoap raft node tests are skipped.");
}

#endif  // CANTCOAP_AVAILABLE
