# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

if(NOT folly_FOUND OR NOT Boost_FOUND OR NOT httplib_FOUND)
    return()
endif()

# ONE `run_node<Bundle>` INSTANTIATION FILE PER ONE OR TWO TYPE BUNDLES, and
# that is load-bearing rather than tidiness: see run_node.hpp. main.cpp used to
# instantiate kythira::node for all twelve bundles itself, which made it the
# last edge of every cold build and, by November 2026, the reason the
# `Full suite (stdexec future backend)` leg was reclaimed mid-build.
add_executable(chaos_node
    main.cpp
    run_node_plain.cpp
    run_node_tls_aws.cpp
    run_node_otlp_netdata.cpp
    run_node_prometheus.cpp
    run_node_victoriametrics.cpp
    run_node_telegraf.cpp
)

target_include_directories(chaos_node PRIVATE
    "${CMAKE_SOURCE_DIR}/include"
    "${CMAKE_SOURCE_DIR}/include/raft"
    "${CMAKE_SOURCE_DIR}/cmd/chaos_node"
)

target_link_libraries(chaos_node PRIVATE
    network_simulator   # brings in Folly::folly + boost_context + Boost::system/thread
    boost_json
    httplib::httplib
    kythira_host_node_id   # KYTHIRA_NODE_ID_KIND
)

# RPC_TLS_* (mutual TLS on Raft RPC) needs tls_tcp_rpc.hpp's OpenSSL side,
# whose trust checks live in certificate_authority, and httplib's TLS API,
# which tls_tcp_rpc.hpp calls unconditionally and which exists only under
# CONFIG_HTTP_TRANSPORT_TLS (KYTHIRA_HTTP_TLS). Without both the node still
# builds and refuses RPC_TLS_* at startup.
if(TARGET certificate_authority AND KYTHIRA_HTTP_TLS)
    target_link_libraries(chaos_node PRIVATE certificate_authority)
    target_compile_definitions(chaos_node PRIVATE KYTHIRA_CHAOS_NODE_RPC_TLS)
endif()

if(CHAOS_TESTS_ENABLED)
    target_compile_definitions(chaos_node PRIVATE
        FIU_ENABLE
        KYTHIRA_FAULT_INJECTION
    )
    target_include_directories(chaos_node PRIVATE ${FIU_INCLUDE_DIRS})
    target_link_libraries(chaos_node PRIVATE ${FIU_LIBRARIES})
    target_compile_options(chaos_node PRIVATE ${FIU_CFLAGS_OTHER})
    message(STATUS "chaos_node: fault injection enabled (custom fiu_tcp_rc)")
else()
    message(STATUS "chaos_node: fault injection disabled (libfiu not found)")
endif()

# The cairn M1 seeded bug, planted only here (top-level CMakeLists.txt
# explains the option and prints the banner).
if(KYTHIRA_CHAOS_SEEDED_BUG STREQUAL "vote_term")
    target_compile_definitions(chaos_node PRIVATE KYTHIRA_CHAOS_SEEDED_BUG_VOTE_TERM)
endif()

# The split alone is not the whole fix. One two-bundle run_node_*.cpp still
# measures 6,109 MiB under stdexec (g++-13, -O3), so three of them at once
# under CI's `-j3` would exhaust a 16 GiB runner again. `heavy_tu` (declared in
# the top-level CMakeLists.txt) admits one at a time. The property is
# per-target, so main.cpp (1,632 MiB, 58 s) is serialised with them too.
set_target_properties(chaos_node PROPERTIES
    JOB_POOL_COMPILE heavy_tu
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
