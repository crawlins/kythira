# cmd/raft_object_backup — the backup/restore CLI (Requirement 10.6).
#
# Built unconditionally, and that is the requirement rather than an oversight:
# Requirement 16.4 says this binary builds with *whichever* providers are
# enabled and reports the unavailable ones **by name**. A build with every
# cloud gate off still produces a working tool — one that can print its usage,
# name all five providers as absent, and say for each what would enable it.
# Gating the target on "at least one provider" would replace that message with
# "command not found", which is exactly the wrong thing to hand an operator in
# a recovery window.
#
# It links no provider it does not have, so a zero-provider build is also a
# very small binary.
#
# It does link network_simulator, and not for futures — this tool uses none.
# `object_store_backup.hpp` includes `object_store_persistence.hpp`, which
# includes `persistence.hpp` → `future.hpp` → Folly, so the header chain needs a
# future backend's include paths whatever this binary does with them. That is
# why the root CMakeLists gates this subdirectory on
# KYTHIRA_FUTURE_BACKEND_AVAILABLE, exactly as it gates tests/. Being gated on a
# future backend is orthogonal to being gated on a cloud provider: the
# zero-provider build Requirement 16.4 asks for is still produced.
#
# Each KYTHIRA_BACKUP_PROVIDER_* macro is the conjunction of two separate
# questions the CLI has to report on together: was the dependency *found*
# (KYTHIRA_HAS_*, from a find_package), and was the backend *wanted* (the
# Kconfig gate). Neither alone is the right answer to "can I use --provider s3
# with this binary?".
#
# The provider macros are set here rather than taken from network_simulator's
# INTERFACE definitions deliberately. Those answer only "was the dependency
# found?" — they know nothing about the Kconfig gates — so inheriting them would
# make this tool offer `--provider s3` on a build that had switched S3 off.

add_executable(raft_object_backup main.cpp)

target_include_directories(raft_object_backup PRIVATE "${CMAKE_SOURCE_DIR}/include")
target_compile_features(raft_object_backup PRIVATE cxx_std_23)
target_link_libraries(raft_object_backup PRIVATE network_simulator)
if(TARGET Folly::folly)
    target_link_libraries(raft_object_backup PRIVATE Folly::folly)
endif()

set(_KYTHIRA_BACKUP_ENABLED "")

# ── S3 ────────────────────────────────────────────────────────────────────────
if(AWSSDK_FOUND AND _KYTHIRA_GATE_AWS_S3_PERSISTENCE)
    target_compile_definitions(raft_object_backup PRIVATE
        KYTHIRA_BACKUP_PROVIDER_S3 KYTHIRA_HAS_AWS_SDK)
    target_link_libraries(raft_object_backup PRIVATE ${AWSSDK_LINK_LIBRARIES})
    list(APPEND _KYTHIRA_BACKUP_ENABLED s3)
endif()

# ── Azure Blob ────────────────────────────────────────────────────────────────
if(azure-core-cpp_FOUND AND azure-identity-cpp_FOUND AND _KYTHIRA_GATE_AZURE_BLOB_PERSISTENCE
        AND TARGET OpenSSL::SSL AND TARGET httplib::httplib)
    target_compile_definitions(raft_object_backup PRIVATE
        KYTHIRA_BACKUP_PROVIDER_AZURE KYTHIRA_HAS_AZURE_SDK)
    target_link_libraries(raft_object_backup PRIVATE
        Azure::azure-core Azure::azure-identity OpenSSL::SSL OpenSSL::Crypto httplib::httplib)
    list(APPEND _KYTHIRA_BACKUP_ENABLED azure-blob)
endif()

# ── GCS ───────────────────────────────────────────────────────────────────────
if(TARGET google-cloud-cpp::storage AND _KYTHIRA_GATE_GCP_STORAGE_PERSISTENCE)
    target_compile_definitions(raft_object_backup PRIVATE
        KYTHIRA_BACKUP_PROVIDER_GCS KYTHIRA_HAS_GCP_STORAGE)
    target_link_libraries(raft_object_backup PRIVATE google-cloud-cpp::storage)
    list(APPEND _KYTHIRA_BACKUP_ENABLED gcs)
endif()

# ── OCI Object Storage ────────────────────────────────────────────────────────
# No find_package to consult: there is no OCI SDK. KYTHIRA_OCI_SHARED is what
# says the signing/HTTP layer this client is built on was wanted at all.
if(KYTHIRA_OCI_SHARED AND _KYTHIRA_GATE_OCI_OBJECT_PERSISTENCE
        AND TARGET OpenSSL::SSL AND TARGET httplib::httplib)
    target_compile_definitions(raft_object_backup PRIVATE KYTHIRA_BACKUP_PROVIDER_OCI)
    target_link_libraries(raft_object_backup PRIVATE
        OpenSSL::SSL OpenSSL::Crypto httplib::httplib)
    list(APPEND _KYTHIRA_BACKUP_ENABLED oci-objectstorage)
endif()

# ── Alibaba OSS ───────────────────────────────────────────────────────────────
if(KYTHIRA_ALIBABA_SHARED AND _KYTHIRA_GATE_ALIBABA_OSS_PERSISTENCE
        AND TARGET OpenSSL::SSL AND TARGET httplib::httplib)
    target_compile_definitions(raft_object_backup PRIVATE KYTHIRA_BACKUP_PROVIDER_OSS)
    target_link_libraries(raft_object_backup PRIVATE
        OpenSSL::SSL OpenSSL::Crypto httplib::httplib)
    list(APPEND _KYTHIRA_BACKUP_ENABLED oss)
endif()

# boost::json is used by object_store_backup.hpp for the manifest, and by the
# OCI and Alibaba clients for their error bodies and listings. Linked
# unconditionally because the manifest path needs it whatever providers are on.
if(TARGET boost_json)
    target_link_libraries(raft_object_backup PRIVATE boost_json)
elseif(TARGET Boost::json)
    target_link_libraries(raft_object_backup PRIVATE Boost::json)
endif()

if(_KYTHIRA_BACKUP_ENABLED)
    list(JOIN _KYTHIRA_BACKUP_ENABLED ", " _KYTHIRA_BACKUP_ENABLED_TEXT)
    message(STATUS "raft_object_backup: enabled (providers: ${_KYTHIRA_BACKUP_ENABLED_TEXT})")
else()
    message(STATUS "raft_object_backup: enabled with NO providers — it will build and run, and "
        "report every provider as not compiled in (Requirement 16.4)")
endif()
