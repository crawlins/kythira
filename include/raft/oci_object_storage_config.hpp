// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file oci_object_storage_config.hpp
/// @brief `oci_object_storage_config`, on its own so that code which only
///        builds the config (`oci_client_config_env.hpp`) need not pull in
///        `oci_object_storage_client.hpp` and, through it, httplib and
///        OpenSSL.

#include <raft/oci_client_config.hpp>

#include <string>

namespace kythira {

/// @brief Object Storage settings, alongside the shared OCI ones.
struct oci_object_storage_config {
    /// Region, auth material, endpoint override and timeout — the same struct
    /// the quorum manager and certificate provider take.
    oci_client_config oci;

    /// The tenancy's Object Storage namespace. When empty it is resolved once at
    /// construction via `GET /n/` and cached (task 0.8).
    std::string namespace_name;
};

}  // namespace kythira
