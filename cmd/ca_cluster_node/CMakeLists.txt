# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

if(NOT TARGET certificate_authority)
    message(STATUS "ca_cluster_node: skipped (requires OpenSSL)")
    return()
endif()
if(NOT TARGET httplib::httplib OR NOT TARGET boost_json)
    message(STATUS "ca_cluster_node: skipped (requires httplib and boost_json)")
    return()
endif()
if(NOT folly_FOUND OR NOT Boost_FOUND)
    message(STATUS "ca_cluster_node: skipped (requires Folly and Boost)")
    return()
endif()

add_executable(ca_cluster_node main.cpp)

target_include_directories(ca_cluster_node PRIVATE
    "${CMAKE_SOURCE_DIR}/include"
    "${CMAKE_SOURCE_DIR}/cmd/ca_cluster_node"
)

target_link_libraries(ca_cluster_node PRIVATE
    certificate_authority   # brings in network_simulator -> Folly::folly + boost_context
    network_simulator
    boost_json
    httplib::httplib
    OpenSSL::SSL
    OpenSSL::Crypto
    kythira_host_node_id   # KYTHIRA_NODE_ID_KIND
)

# --oidc-publish-url (iam-compatible-auth Requirement 1.9) writes through the
# existing object-store clients. Each KYTHIRA_OIDC_PUBLISH_* macro means the
# provider's dependency was found AND its persistence Kconfig gate is on,
# exactly as cmd/raft_object_backup decides which providers it offers; a
# build with none still runs, and refuses --oidc-publish-url at startup
# naming what would enable the provider asked for.
set(_KYTHIRA_OIDC_PUBLISH_ENABLED "")
if(AWSSDK_FOUND AND _KYTHIRA_GATE_AWS_S3_PERSISTENCE)
    target_compile_definitions(ca_cluster_node PRIVATE
        KYTHIRA_OIDC_PUBLISH_S3 KYTHIRA_HAS_AWS_SDK)
    target_link_libraries(ca_cluster_node PRIVATE ${AWSSDK_LINK_LIBRARIES})
    list(APPEND _KYTHIRA_OIDC_PUBLISH_ENABLED s3)
endif()
if(azure-core-cpp_FOUND AND azure-identity-cpp_FOUND AND _KYTHIRA_GATE_AZURE_BLOB_PERSISTENCE)
    target_compile_definitions(ca_cluster_node PRIVATE
        KYTHIRA_OIDC_PUBLISH_AZURE KYTHIRA_HAS_AZURE_SDK)
    target_link_libraries(ca_cluster_node PRIVATE Azure::azure-core Azure::azure-identity)
    list(APPEND _KYTHIRA_OIDC_PUBLISH_ENABLED azure-blob)
endif()
if(TARGET google-cloud-cpp::storage AND _KYTHIRA_GATE_GCP_STORAGE_PERSISTENCE)
    target_compile_definitions(ca_cluster_node PRIVATE
        KYTHIRA_OIDC_PUBLISH_GCS KYTHIRA_HAS_GCP_STORAGE)
    target_link_libraries(ca_cluster_node PRIVATE google-cloud-cpp::storage)
    list(APPEND _KYTHIRA_OIDC_PUBLISH_ENABLED gcs)
endif()
if(KYTHIRA_OCI_SHARED AND _KYTHIRA_GATE_OCI_OBJECT_PERSISTENCE)
    target_compile_definitions(ca_cluster_node PRIVATE KYTHIRA_OIDC_PUBLISH_OCI)
    list(APPEND _KYTHIRA_OIDC_PUBLISH_ENABLED oci-objectstorage)
endif()
if(KYTHIRA_ALIBABA_SHARED AND _KYTHIRA_GATE_ALIBABA_OSS_PERSISTENCE)
    target_compile_definitions(ca_cluster_node PRIVATE KYTHIRA_OIDC_PUBLISH_OSS)
    list(APPEND _KYTHIRA_OIDC_PUBLISH_ENABLED oss)
endif()
if(_KYTHIRA_OIDC_PUBLISH_ENABLED)
    list(JOIN _KYTHIRA_OIDC_PUBLISH_ENABLED ", " _KYTHIRA_OIDC_PUBLISH_TEXT)
    message(STATUS "ca_cluster_node: --oidc-publish-url providers: ${_KYTHIRA_OIDC_PUBLISH_TEXT}")
else()
    message(STATUS "ca_cluster_node: --oidc-publish-url has no providers in this build")
endif()

target_compile_features(ca_cluster_node PRIVATE cxx_std_23)
set_target_properties(ca_cluster_node PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
message(STATUS "ca_cluster_node target: enabled")
