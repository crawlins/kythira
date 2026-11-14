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

    # IAM as a module of kythira_node (.kiro/specs/service-colocation/
    # task 8): its registrar, linked whole-archive through
    # KYTHIRA_NODE_MODULES (cmd/kythira_node/CMakeLists.txt). It
    # instantiates the IAM group on the node's multi-raft host over the
    # node's Beast transport, so it is built where kythira_node is.
    if(KYTHIRA_BUILD_BOOST_BEAST_TRANSPORT AND KYTHIRA_FUTURE_BACKEND_AVAILABLE
       AND NOT ENABLE_COVERAGE)
        add_library(kythira_iam_module STATIC iam_node/iam_module_registrar.cpp)
        target_include_directories(kythira_iam_module PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/iam_node")
        target_link_libraries(kythira_iam_module PUBLIC
            kythira_iam
            certificate_authority
            boost_json
            OpenSSL::SSL
            OpenSSL::Crypto
            kythira_host_node_id
        )
        target_compile_features(kythira_iam_module PRIVATE cxx_std_23)
        set_target_properties(kythira_iam_module PROPERTIES JOB_POOL_COMPILE heavy_tu)
        set_property(GLOBAL APPEND PROPERTY KYTHIRA_NODE_MODULES kythira_iam_module)
        set_property(GLOBAL APPEND PROPERTY KYTHIRA_NODE_MODULE_NAMES iam)
    endif()
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
