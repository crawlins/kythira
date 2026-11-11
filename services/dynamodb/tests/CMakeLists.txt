# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# The DynamoDB module's tests (.kiro/specs/dynamodb-compatible-api/).

include(kythira_test_helpers)

# add_raft_completion_test plus the module's headers. Binaries and working
# directories go to build/tests/, which the coverage job collects test
# binaries from, as the other modules' do.
function(kythira_dynamodb_test test_name source_file)
    add_raft_completion_test(${test_name} ${source_file})
    target_link_libraries(${test_name} PRIVATE kythira_dynamodb)
    target_compile_definitions(${test_name} PRIVATE
        KYTHIRA_DYNAMODB_SOURCE_DIR="${KYTHIRA_DYNAMODB_SOURCE_DIR}")
    set_target_properties(${test_name} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
    set_tests_properties(${test_name} PROPERTIES
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
endfunction()

# Every module test carries `dynamodb` and `dynamodb-module`;
# dynamodb-module.yml checks that no `dynamodb-module` test exists with the
# module off.

# Task 1: every operation in the pinned models is implemented or listed in
# doc/unsupported.md (Requirement 2.1, 2.2), and the generated shape tables
# load into the core codec.
kythira_dynamodb_test(dynamodb_model_coverage_test dynamodb_model_coverage_test.cpp)
set_tests_properties(dynamodb_model_coverage_test PROPERTIES
    LABELS "unit;dynamodb;dynamodb-module" TIMEOUT 60)

# Task 1: every request the recorded clients sent (evidence/) frames and
# validates against the pinned model, unknown members refused
# (Requirement 2.3).
kythira_dynamodb_test(dynamodb_wire_evidence_test dynamodb_wire_evidence_test.cpp)
set_tests_properties(dynamodb_wire_evidence_test PROPERTIES
    LABELS "unit;dynamodb;dynamodb-module" TIMEOUT 60)

# Task 3: the exact decimal (vector suite, Requirement 21.2; CI runs it on
# x86-64 and arm64) and the item codec (Requirement 6).
kythira_dynamodb_test(dynamodb_decimal_test dynamodb_decimal_test.cpp)
set_tests_properties(dynamodb_decimal_test PROPERTIES
    LABELS "unit;dynamodb;dynamodb-module" TIMEOUT 60)
kythira_dynamodb_test(dynamodb_item_codec_test dynamodb_item_codec_test.cpp)
set_tests_properties(dynamodb_item_codec_test PROPERTIES
    LABELS "unit;dynamodb;dynamodb-module" TIMEOUT 60)

# Task 4: the front end (SigV4, authorization, dispatch, DescribeEndpoints),
# the generated action table and static-key roles, and the compatibility
# profile.
foreach(_ddb_test IN ITEMS dynamodb_front_end_test dynamodb_actions_test
        dynamodb_compat_profile_test)
    kythira_dynamodb_test(${_ddb_test} ${_ddb_test}.cpp)
    set_tests_properties(${_ddb_test} PROPERTIES
        LABELS "unit;dynamodb;dynamodb-module" TIMEOUT 60)
endforeach()

# Task 5: the expression language. The corpus test replays every case of
# tests/data/expression_corpus.jsonl, recorded against DynamoDB Local by
# tools/expr-corpus/record.sh, and must answer as DynamoDB Local did
# (Requirement 21.3); the fuzz test runs seeded mutations of the same
# expressions and of their binary form.
foreach(_ddb_test IN ITEMS dynamodb_expression_corpus_test dynamodb_expression_fuzz_test)
    kythira_dynamodb_test(${_ddb_test} ${_ddb_test}.cpp)
    target_compile_definitions(${_ddb_test} PRIVATE
        KYTHIRA_EXPRESSION_CORPUS="${KYTHIRA_DYNAMODB_SOURCE_DIR}/tests/data/expression_corpus.jsonl")
    set_tests_properties(${_ddb_test} PROPERTIES
        LABELS "unit;dynamodb;dynamodb-module" TIMEOUT 300)
endforeach()

# Task 6: the item state machine (conditions in apply(), versions,
# ReturnValues, splits only between item collections, the bounded drop of a
# deleted table) and the catalog with the disk guard (Requirements 7.2-7.4,
# 8.2, 20.1, 20.2), on memory_kv_engine.
foreach(_ddb_test IN ITEMS dynamodb_item_state_machine_test dynamodb_catalog_test)
    kythira_dynamodb_test(${_ddb_test} ${_ddb_test}.cpp)
    set_tests_properties(${_ddb_test} PROPERTIES
        LABELS "unit;dynamodb;dynamodb-module" TIMEOUT 60)
endforeach()

# Task 7: the control plane and single-item handlers through the front end
# on local_backend (Requirements 7.1, 8.1, 8.6, 20.2).
kythira_dynamodb_test(dynamodb_api_test dynamodb_api_test.cpp)
set_tests_properties(dynamodb_api_test PROPERTIES
    LABELS "unit;dynamodb;dynamodb-module" TIMEOUT 60)

# Task 7: dynamodb_gateway_node's configuration from the environment, and
# the gateway serving signed requests over a socket on a free port.
find_package(Threads REQUIRED)
kythira_dynamodb_test(dynamodb_gateway_test dynamodb_gateway_test.cpp)
target_link_libraries(dynamodb_gateway_test PRIVATE Threads::Threads)
set_tests_properties(dynamodb_gateway_test PROPERTIES
    LABELS "unit;dynamodb;dynamodb-module" TIMEOUT 60)

# dynamodb_gateway_node as a three-node cluster in one process: the daemon's
# run_cluster() on the memory engine, Raft and gateways on free loopback
# ports, requests on every node crossing the forward link (task 7). Only
# where the daemon itself builds.
if(TARGET dynamodb_gateway_node)
    kythira_dynamodb_test(dynamodb_gateway_cluster_test dynamodb_gateway_cluster_test.cpp)
    target_include_directories(dynamodb_gateway_cluster_test PRIVATE
        "${KYTHIRA_DYNAMODB_SOURCE_DIR}/cmd/dynamodb_gateway_node")
    # kythira_test_support: tests/test_ports.hpp.
    target_link_libraries(dynamodb_gateway_cluster_test PRIVATE
        kythira_test_support kythira_host_node_id)
    set_target_properties(dynamodb_gateway_cluster_test PROPERTIES JOB_POOL_COMPILE heavy_tu)
    set_tests_properties(dynamodb_gateway_cluster_test PROPERTIES
        LABELS "integration;dynamodb;dynamodb-module" TIMEOUT 300)
endif()
# Task 8: Query, Scan and the batch operations at their limits (1 MB pages,
# segments, the 16 MB BatchGetItem response), and the operations corpus,
# tests/data/ops_corpus.jsonl, recorded against DynamoDB Local by
# tools/ops-corpus/record.sh, replayed through the handlers (Requirements
# 9.1, 9.2, 10.1, 10.2, 21.1).
kythira_dynamodb_test(dynamodb_query_test dynamodb_query_test.cpp)
set_tests_properties(dynamodb_query_test PROPERTIES
    LABELS "unit;dynamodb;dynamodb-module" TIMEOUT 120)
# Local secondary indexes beyond the corpus: engine entries, the item
# collection limit, ItemCollectionMetrics, drops and stored forms
# (Requirements 11.1, 11.5).
kythira_dynamodb_test(dynamodb_lsi_test dynamodb_lsi_test.cpp)
set_tests_properties(dynamodb_lsi_test PROPERTIES
    LABELS "unit;dynamodb;dynamodb-module" TIMEOUT 120)
# The Raft-hosted backend over three in-process multi_raft hosts: requests on
# any node, the catalog leader's follow-on work, range reads across a split,
# and an item group losing its leader (task 7). The in-process fabric comes
# from kythira_test_support.
kythira_dynamodb_test(dynamodb_raft_backend_test dynamodb_raft_backend_test.cpp)
target_link_libraries(dynamodb_raft_backend_test PRIVATE kythira_test_support)
set_tests_properties(dynamodb_raft_backend_test PROPERTIES
    LABELS "integration;dynamodb;dynamodb-module" TIMEOUT 300)
# The invariants services/dynamodb/doc/invariants.md collects from the AWS
# developer guide and API reference, one case per invariant, driven through
# the front end on the local backend.
kythira_dynamodb_test(dynamodb_invariants_test dynamodb_invariants_test.cpp)
set_tests_properties(dynamodb_invariants_test PROPERTIES
    LABELS "unit;dynamodb;dynamodb-module" TIMEOUT 120)
# Transactions below the API (task 10): the validate hook, one entry's
# writes staged into one batch, and a group under core's transaction wrapper.
kythira_dynamodb_test(dynamodb_txn_test dynamodb_txn_test.cpp)
set_tests_properties(dynamodb_txn_test PROPERTIES
    LABELS "unit;dynamodb;dynamodb-module" TIMEOUT 120)
# Global secondary indexes in the item machine (task 11): entry versions,
# pending changes and their acks, and the propagation plan.
kythira_dynamodb_test(dynamodb_gsi_test dynamodb_gsi_test.cpp)
set_tests_properties(dynamodb_gsi_test PROPERTIES
    LABELS "unit;dynamodb;dynamodb-module" TIMEOUT 120)
# Capacity (task 12): token buckets per table, index and partition key,
# throttling on by default and its switch, and AWS's consumed-capacity
# formulas.
kythira_dynamodb_test(dynamodb_capacity_test dynamodb_capacity_test.cpp)
set_tests_properties(dynamodb_capacity_test PROPERTIES
    LABELS "unit;dynamodb;dynamodb-module" TIMEOUT 120)
# Time to live (task 13): UpdateTimeToLive and DescribeTimeToLive, the
# hourly limit and its switch, and the sweeper.
kythira_dynamodb_test(dynamodb_ttl_test dynamodb_ttl_test.cpp)
set_tests_properties(dynamodb_ttl_test PROPERTIES
    LABELS "unit;dynamodb;dynamodb-module" TIMEOUT 120)
# PartiQL (task 14): ExecuteStatement, BatchExecuteStatement and
# ExecuteTransaction with DynamoDB Local's answers.
kythira_dynamodb_test(dynamodb_partiql_test dynamodb_partiql_test.cpp)
set_tests_properties(dynamodb_partiql_test PROPERTIES
    LABELS "unit;dynamodb;dynamodb-module" TIMEOUT 120)
# The change log (task 15): records, stream lineage across splits and
# merges, restart, and a snapshot install that fetches the active segment.
kythira_dynamodb_test(dynamodb_change_log_test dynamodb_change_log_test.cpp)
set_tests_properties(dynamodb_change_log_test PROPERTIES
    LABELS "unit;dynamodb;dynamodb-module" TIMEOUT 120)
# DynamoDB Streams (task 16): StreamSpecification, the four Streams
# actions with DynamoDB Local's answers, view types, retention.
kythira_dynamodb_test(dynamodb_streams_test dynamodb_streams_test.cpp)
set_tests_properties(dynamodb_streams_test PROPERTIES
    LABELS "unit;dynamodb;dynamodb-module" TIMEOUT 120)
# Backups and restore (task 18): the five actions, the worker, the object
# layout and the stores.
kythira_dynamodb_test(dynamodb_backup_test dynamodb_backup_test.cpp)
# kythira_test_support: tests/mock_object_store.hpp.
target_link_libraries(dynamodb_backup_test PRIVATE kythira_test_support)
set_tests_properties(dynamodb_backup_test PROPERTIES
    LABELS "unit;dynamodb;dynamodb-module" TIMEOUT 120)
# Kinesis streaming destinations (task 17): the four actions, the
# publisher's record format, order and resends, and the SigV4 sink.
kythira_dynamodb_test(dynamodb_kinesis_destination_test dynamodb_kinesis_destination_test.cpp)
set_tests_properties(dynamodb_kinesis_destination_test PROPERTIES
    LABELS "unit;dynamodb;dynamodb-module" TIMEOUT 120)
kythira_dynamodb_test(dynamodb_ops_corpus_test dynamodb_ops_corpus_test.cpp)
target_compile_definitions(dynamodb_ops_corpus_test PRIVATE
    KYTHIRA_OPS_CORPUS="${KYTHIRA_DYNAMODB_SOURCE_DIR}/tests/data/ops_corpus.jsonl")
set_tests_properties(dynamodb_ops_corpus_test PROPERTIES
    LABELS "unit;dynamodb;dynamodb-module" TIMEOUT 120)

# The checked-in shape tables are what core's scripts/gen_aws_json_shapes.py
# (KYTHIRA_AWS_JSON_SHAPES_GENERATOR) makes of the pinned models, so a model
# upgrade carries its regenerated tables and their diff (Requirement 2.4).
# tools/regen_models.sh writes them; these run the same commands with
# --check. Skipped when configure found no Python, like core's
# aws_json_fixture_model_fresh.
if(Python3_EXECUTABLE)
foreach(_ddb_model IN ITEMS dynamodb dynamodbstreams)
    add_test(NAME ${_ddb_model}_model_header_fresh
        COMMAND ${Python3_EXECUTABLE} "${KYTHIRA_AWS_JSON_SHAPES_GENERATOR}"
            model/${_ddb_model}-2012-08-10.json
            include/kythira/dynamodb/model/${_ddb_model}_model.hpp
            --namespace kythira::dynamodb::${_ddb_model}_model
            --label services/dynamodb/model/${_ddb_model}-2012-08-10.json
            --check
        WORKING_DIRECTORY "${KYTHIRA_DYNAMODB_SOURCE_DIR}")
    set_tests_properties(${_ddb_model}_model_header_fresh PROPERTIES
        LABELS "unit;dynamodb;dynamodb-module" TIMEOUT 60)
endforeach()
# The action table is what tools/gen_dynamodb_actions.py makes of the pinned
# service reference.
add_test(NAME dynamodb_actions_header_fresh
    COMMAND ${Python3_EXECUTABLE} tools/gen_dynamodb_actions.py
        tools/dynamodb-service-reference.json
        include/kythira/dynamodb/dynamodb_actions.hpp --check
    WORKING_DIRECTORY "${KYTHIRA_DYNAMODB_SOURCE_DIR}")
set_tests_properties(dynamodb_actions_header_fresh PROPERTIES
    LABELS "unit;dynamodb;dynamodb-module" TIMEOUT 60)
# The recorded expression corpus is the case list cases.py makes, so an
# edited case is re-recorded with it.
add_test(NAME dynamodb_expression_corpus_fresh
    COMMAND ${Python3_EXECUTABLE} tools/expr-corpus/cases.py --check
        tests/data/expression_corpus.jsonl
    WORKING_DIRECTORY "${KYTHIRA_DYNAMODB_SOURCE_DIR}")
set_tests_properties(dynamodb_expression_corpus_fresh PROPERTIES
    LABELS "unit;dynamodb;dynamodb-module" TIMEOUT 60)
endif()

kythira_scale_test_timeouts()
