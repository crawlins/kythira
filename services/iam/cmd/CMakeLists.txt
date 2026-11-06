# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# The IAM module's composition roots (.kiro/specs/iam-compatible-auth/
# task 9): the iam_node daemon and the kythira-iam admin CLI. Their core
# *_impl.hpp includes, if any, are listed in ../boundary-allow.txt.
#
# Like redis_gateway_node they are run from the build tree and from the
# container image under ../docker/, and are in no install target.

# iam_node hosts a Raft group over TLS-TCP RPC. certificate_authority is
# what carries both halves of that, as for ca_cluster_node: the future
# backend through network_simulator, and KYTHIRA_HAS_OPENSSL, without which
# raft/tls_tcp_rpc.hpp and raft/ca_http_helpers.hpp compile to nothing. The
# target exists only with OpenSSL and a future backend.
if(TARGET certificate_authority)
    add_executable(iam_node iam_node/main.cpp)
    target_include_directories(iam_node PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/iam_node")
    target_link_libraries(iam_node PRIVATE
        kythira_iam            # <kythira/iam/...> and the core headers
        certificate_authority  # KYTHIRA_HAS_OPENSSL, network_simulator (future backend)
        boost_json
        OpenSSL::SSL
        OpenSSL::Crypto
        kythira_host_node_id   # KYTHIRA_NODE_ID_KIND
    )
    target_compile_features(iam_node PRIVATE cxx_std_23)
    set_target_properties(iam_node PROPERTIES
        JOB_POOL_COMPILE heavy_tu
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
else()
    message(STATUS "iam_node: skipped (needs the certificate_authority target: OpenSSL and a future backend)")
endif()

# kythira-iam is a plain HTTPS client: Boost.Beast, Boost.JSON and OpenSSL.
find_package(Threads REQUIRED)
add_executable(kythira-iam kythira-iam/main.cpp)
target_link_libraries(kythira-iam PRIVATE
    kythira_core_headers   # <raft/private_file.hpp>
    boost_json
    OpenSSL::SSL
    OpenSSL::Crypto
    Threads::Threads
)
target_compile_features(kythira-iam PRIVATE cxx_std_23)
set_target_properties(kythira-iam PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
