# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# The Redis module's tests. Moved here from tests/CMakeLists.txt and
# tests/docker_chaos/CMakeLists.txt (.kiro/specs/redis-gateway-module/
# task 4) with their names, labels and timeouts unchanged.

include(kythira_test_helpers)

# add_raft_completion_test, plus the module's headers and the in-process test
# fabric (multi_raft_test_fabric.hpp and friends, by name). The binary and
# its working directory stay where they were before the move, build/tests/,
# which the coverage job collects test binaries from.
function(kythira_redis_test test_name source_file)
    add_raft_completion_test(${test_name} ${source_file})
    target_include_directories(${test_name} PRIVATE "${KYTHIRA_REDIS_INCLUDE_DIR}")
    target_link_libraries(${test_name} PRIVATE kythira_test_support)
    set_target_properties(${test_name} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
    set_tests_properties(${test_name} PROPERTIES
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
endfunction()

# redis-compatible-kv spec: the RESP gateway over multi_raft for sccache's
# redis backend. The two codecs and the state machine have no dependency the
# tree does not already require, but they are gated together with the gateway
# anyway so that CONFIG_REDIS_GATEWAY=n leaves no trace of the feature in the
# target list (Requirement 16.4). The ACL needs OpenSSL for PBKDF2; the gateway
# integration test needs the in-process fabric, folly and Boost.Asio.
kythira_redis_test(resp_protocol_unit_test resp_protocol_unit_test.cpp)
kythira_redis_test(redis_kv_commands_unit_test redis_kv_commands_unit_test.cpp)
kythira_redis_test(redis_kv_state_machine_unit_test redis_kv_state_machine_unit_test.cpp)
# persistent-state-machine task 2: the same command set on a kv_engine,
# checked against redis_kv_state_machine as the oracle.
kythira_redis_test(persistent_kv_state_machine_test persistent_kv_state_machine_test.cpp)
target_link_libraries(persistent_kv_state_machine_test PRIVATE OpenSSL::Crypto)
# multi-raft-transactions task 11: the multi-version variant.
kythira_redis_test(redis_kv_mvcc_state_machine_test redis_kv_mvcc_state_machine_test.cpp)
# Task 12: snapshot isolation over three hosts whose shards keep versions.
kythira_redis_test(multi_raft_txn_snapshot_test multi_raft_txn_snapshot_test.cpp)
# Task 14: the tier-2 bank workload (snapshot reads always sum to the total).
kythira_redis_test(multi_raft_txn_snapshot_bank_test
                   multi_raft_txn_snapshot_bank_test.cpp)
# Task 17: write skew breaks a two-account minimum balance under
# snapshot and never under serializable.
kythira_redis_test(multi_raft_txn_write_skew_test multi_raft_txn_write_skew_test.cpp)
kythira_redis_test(redis_acl_unit_test redis_acl_unit_test.cpp)
# redis-gateway-module task 1: the log-entry and snapshot bytes of every
# state machine the daemon can be built with, against golden/redis/.
# The RESP-reply goldens live in the integration and txn tests below.
set(KYTHIRA_REDIS_GOLDEN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/golden/redis")
kythira_redis_test(redis_golden_bytes_test redis_golden_bytes_test.cpp)
target_link_libraries(redis_golden_bytes_test PRIVATE OpenSSL::Crypto)
target_compile_definitions(redis_golden_bytes_test PRIVATE
    KYTHIRA_REDIS_GOLDEN_DIR="${KYTHIRA_REDIS_GOLDEN_DIR}")
set_tests_properties(redis_golden_bytes_test PROPERTIES LABELS "unit;redis;cache" TIMEOUT 120)
target_link_libraries(redis_acl_unit_test PRIVATE OpenSSL::Crypto)
kythira_redis_test(redis_auth_limiter_unit_test redis_auth_limiter_unit_test.cpp)
# redis_gateway_node's own environment parsing (the plaintext-Raft opt-in).
kythira_redis_test(redis_gateway_node_config_unit_test
                   redis_gateway_node_config_unit_test.cpp)
target_sources(redis_gateway_node_config_unit_test PRIVATE
               "${CMAKE_CURRENT_SOURCE_DIR}/../cmd/redis_gateway_node/config.cpp")
target_include_directories(redis_gateway_node_config_unit_test PRIVATE
                           "${CMAKE_CURRENT_SOURCE_DIR}/../cmd/redis_gateway_node")
# .kiro/specs/disk-backed-build-cache/ task 2: the per-node chunk store and
# its pack files. The crash and write-failure cases need libfiu's control
# API; without it they compile away and the rest still run.
kythira_redis_test(cache_chunk_store_unit_test cache_chunk_store_unit_test.cpp)
target_link_libraries(cache_chunk_store_unit_test PRIVATE OpenSSL::Crypto)
set_tests_properties(cache_chunk_store_unit_test PROPERTIES
    LABELS "unit;redis;cache" TIMEOUT 600)
if(FIU_FOUND)
    target_compile_definitions(cache_chunk_store_unit_test PRIVATE FIU_ENABLE)
    target_link_libraries(cache_chunk_store_unit_test PRIVATE ${FIU_LDFLAGS})
    target_include_directories(cache_chunk_store_unit_test PRIVATE ${FIU_INCLUDE_DIRS})
endif()
# Task 3: the disk-backed state machine over the chunk store, with the
# design's correctness properties 1-4 as randomized trials and the
# Requirement 2.2 index-memory bound.
kythira_redis_test(cache_kv_state_machine_unit_test cache_kv_state_machine_unit_test.cpp)
target_link_libraries(cache_kv_state_machine_unit_test PRIVATE OpenSSL::Crypto)
set_tests_properties(cache_kv_state_machine_unit_test PROPERTIES
    LABELS "unit;redis;cache" TIMEOUT 600)
# Task 5: the chunk fetcher, with in-process nodes whose stores the fetch
# callback reads directly.
kythira_redis_test(cache_chunk_fetcher_unit_test cache_chunk_fetcher_unit_test.cpp)
target_link_libraries(cache_chunk_fetcher_unit_test PRIVATE OpenSSL::Crypto)
set_tests_properties(cache_chunk_fetcher_unit_test PROPERTIES
    LABELS "unit;redis;cache" TIMEOUT 300)
# Task 7: the disk budget and the voters' free-space floors.
kythira_redis_test(cache_disk_guard_unit_test cache_disk_guard_unit_test.cpp)
set_tests_properties(cache_disk_guard_unit_test PROPERTIES
    LABELS "unit;redis;cache" TIMEOUT 60)
if(folly_FOUND)
    kythira_redis_test(redis_gateway_integration_test redis_gateway_integration_test.cpp)
    target_compile_definitions(redis_gateway_integration_test PRIVATE
        KYTHIRA_REDIS_GOLDEN_DIR="${KYTHIRA_REDIS_GOLDEN_DIR}")
    target_link_libraries(redis_gateway_integration_test PRIVATE OpenSSL::SSL OpenSSL::Crypto)
    set_tests_properties(redis_gateway_integration_test PROPERTIES LABELS "multi-raft;redis" TIMEOUT 600)
    # The same cases over the disk-backed cache_kv_state_machine
    # (.kiro/specs/disk-backed-build-cache/ Requirement 2.7).
    kythira_redis_test(redis_gateway_disk_store_integration_test
                       redis_gateway_disk_store_integration_test.cpp)
    target_compile_definitions(redis_gateway_disk_store_integration_test PRIVATE
        KYTHIRA_REDIS_GOLDEN_DIR="${KYTHIRA_REDIS_GOLDEN_DIR}")
    target_link_libraries(redis_gateway_disk_store_integration_test PRIVATE
                          OpenSSL::SSL OpenSSL::Crypto)
    set_tests_properties(redis_gateway_disk_store_integration_test PROPERTIES
        LABELS "multi-raft;redis;cache" TIMEOUT 600)
    # The same cases over the persistent persistent_kv_state_machine, one
    # memory_kv_engine per host (.kiro/specs/persistent-state-machine/ task 7).
    kythira_redis_test(redis_gateway_engine_store_integration_test
                       redis_gateway_engine_store_integration_test.cpp)
    target_compile_definitions(redis_gateway_engine_store_integration_test PRIVATE
        KYTHIRA_REDIS_GOLDEN_DIR="${KYTHIRA_REDIS_GOLDEN_DIR}")
    target_link_libraries(redis_gateway_engine_store_integration_test PRIVATE
                          OpenSSL::SSL OpenSSL::Crypto)
    set_tests_properties(redis_gateway_engine_store_integration_test PROPERTIES
        LABELS "multi-raft;redis" TIMEOUT 600)
    # Task 6: the cache's HTTP front end over three in-process hosts,
    # including a 1 GiB PUT whose peak RSS must stay under 64 MiB.
    kythira_redis_test(cache_http_front_end_test cache_http_front_end_test.cpp)
    target_link_libraries(cache_http_front_end_test PRIVATE OpenSSL::SSL OpenSSL::Crypto)
    set_tests_properties(cache_http_front_end_test PROPERTIES
        LABELS "multi-raft;redis;cache" TIMEOUT 900)
    # .kiro/specs/multi-raft-transactions/ task 9: the gateway's
    # transaction surface over a wrapped KV state machine.
    kythira_redis_test(redis_gateway_txn_test redis_gateway_txn_test.cpp)
    target_compile_definitions(redis_gateway_txn_test PRIVATE
        KYTHIRA_REDIS_GOLDEN_DIR="${KYTHIRA_REDIS_GOLDEN_DIR}")
    target_link_libraries(redis_gateway_txn_test PRIVATE OpenSSL::SSL OpenSSL::Crypto)
    set_tests_properties(redis_gateway_txn_test PROPERTIES LABELS "multi-raft;redis;txn" TIMEOUT 600)
    # Tier 2 (task 14): the gateway over a multi-versioned KV and an
    # oracle group, as CONFIG_REDIS_GATEWAY_SNAPSHOT_ISOLATION builds it.
    kythira_redis_test(redis_gateway_snapshot_test redis_gateway_snapshot_test.cpp)
    target_link_libraries(redis_gateway_snapshot_test PRIVATE OpenSSL::SSL OpenSSL::Crypto)
    set_tests_properties(redis_gateway_snapshot_test PROPERTIES LABELS "multi-raft;redis;txn" TIMEOUT 600)
else()
    message(STATUS "redis_gateway_integration_test: skipped (requires folly)")
endif()

# The transaction rows of the multi-Raft bench (.kiro/specs/multi-raft-
# transactions/ Requirement 13.6), one translation unit per wrapper tier.
# They run over the Redis KV state machines, so they live here and join
# core's multi_raft_bench_rows library; the catalog drops them by name when
# the module is off.
target_sources(multi_raft_bench_rows PRIVATE
    bench_rows/txn_fabric.cpp
    bench_rows/txn_snapshot_fabric.cpp
)
target_include_directories(multi_raft_bench_rows PRIVATE "${KYTHIRA_REDIS_INCLUDE_DIR}")
target_compile_definitions(multi_raft_bench_rows PUBLIC KYTHIRA_BENCH_HAS_TXN)

# The same rows at a fifth of their budget: a liveness check of 1PC, 2PC over
# two and four shards and snapshot reads, labelled like
# multi_raft_performance_report_smoke so it runs where the report runs and
# not on every CI leg.
add_test(NAME multi_raft_performance_report_transactions
    COMMAND multi_raft_performance_report --axis transactions --budget-scale 0.2
            --out-dir "${CMAKE_BINARY_DIR}/tests/report_transactions")
set_tests_properties(multi_raft_performance_report_transactions PROPERTIES
    LABELS "performance;benchmark;multi-raft;report;transactions"
    TIMEOUT 900
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
)

# The container scenarios, under the guard tests/docker_chaos/ is entered with.
if(TARGET Folly::folly AND Boost_FOUND AND httplib_FOUND)
    add_subdirectory(docker)
endif()

# What tests/CMakeLists.txt does for its own directory at its end.
kythira_scale_test_timeouts()
kythira_attach_test_reports()
