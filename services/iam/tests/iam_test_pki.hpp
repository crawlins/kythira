// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

// A throwaway PKI for iam_node tests: a CA, one certificate per node with its
// reserved peer SAN (raft/ca_http_helpers.hpp) and its host names, an admin
// certificate, an outsider certificate from the same CA whose URI SAN is not
// an admin's, and an unseal passphrase file. Used by iam_node_test in
// process and by the iam_test_certs tool for the compose scenario.

#include <raft/ca_http_helpers.hpp>
#include <raft/certificate_authority.hpp>
#include <raft/host_node_id.hpp>
#include <raft/private_file.hpp>

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace kythira::iam::testing {

inline constexpr const char* k_admin_uri = "spiffe://kythira.test/iam-admin";
inline constexpr const char* k_outsider_uri = "spiffe://kythira.test/someone-else";

struct test_pki_options {
    std::filesystem::path dir;
    std::vector<kythira::host_node_id> nodes;
    /// Extra DNS names per node, by node id text (compose service names).
    std::map<std::string, std::vector<std::string>> host_names;
    std::string unseal_passphrase = "kythira-test-unseal-passphrase";
    /// The root's common name. A test that also trusts another CA built
    /// from certificate_authority's defaults gives this one its own name:
    /// two roots with one subject and no key identifiers are told apart by
    /// trying the first, so a chain to the second fails its signature.
    std::string ca_common_name = "Kythira Test Root CA";
};

struct test_pki_paths {
    std::filesystem::path ca;
    std::filesystem::path admin_cert;
    std::filesystem::path admin_key;
    std::filesystem::path outsider_cert;
    std::filesystem::path outsider_key;
    std::filesystem::path unseal;

    [[nodiscard]] auto node_cert(const kythira::host_node_id& id) const -> std::filesystem::path {
        return ca.parent_path() / ("node-" + kythira::host_node_id_text(id) + ".pem");
    }
    [[nodiscard]] auto node_key(const kythira::host_node_id& id) const -> std::filesystem::path {
        return ca.parent_path() / ("node-" + kythira::host_node_id_text(id) + ".key");
    }
};

/// Writes the PKI into `o.dir`, which must exist. Keys are 0600.
inline auto write_test_pki(const test_pki_options& o) -> test_pki_paths {
    raft::testing::ca_options ca_opts;
    ca_opts.subject.common_name = o.ca_common_name;
    raft::testing::certificate_authority ca(ca_opts);
    test_pki_paths p;
    p.ca = o.dir / "ca.pem";
    raft::write_file_atomically(p.ca, ca.root_certificate_pem(), raft::file_visibility::standard);
    for (const auto& id : o.nodes) {
        raft::testing::leaf_certificate_options leaf;
        const auto name = raft::testing::peer_identity_name(id);
        leaf.subject.common_name = "kythira-iam-node";
        if constexpr (raft::testing::peer_identity_is_uri<kythira::host_node_id>) {
            leaf.uri_names = {name};
        } else {
            leaf.dns_names = {name};
        }
        leaf.dns_names.emplace_back("localhost");
        if (const auto it = o.host_names.find(kythira::host_node_id_text(id));
            it != o.host_names.end()) {
            leaf.dns_names.insert(leaf.dns_names.end(), it->second.begin(), it->second.end());
        }
        leaf.ip_addresses = {"127.0.0.1"};
        leaf.server_auth = true;
        leaf.client_auth = true;
        auto issued = ca.issue(leaf);
        raft::write_file_atomically(p.node_cert(id), issued.chain_pem,
                                    raft::file_visibility::standard);
        raft::write_private_file(p.node_key(id), issued.private_key_pem);
    }
    auto client = [&](const char* uri, const std::filesystem::path& cert,
                      const std::filesystem::path& key) {
        raft::testing::leaf_certificate_options leaf;
        leaf.subject.common_name = "kythira-iam-client";
        leaf.uri_names = {uri};
        leaf.server_auth = false;
        leaf.client_auth = true;
        auto issued = ca.issue(leaf);
        raft::write_file_atomically(cert, issued.chain_pem, raft::file_visibility::standard);
        raft::write_private_file(key, issued.private_key_pem);
    };
    p.admin_cert = o.dir / "admin.pem";
    p.admin_key = o.dir / "admin.key";
    client(k_admin_uri, p.admin_cert, p.admin_key);
    p.outsider_cert = o.dir / "outsider.pem";
    p.outsider_key = o.dir / "outsider.key";
    client(k_outsider_uri, p.outsider_cert, p.outsider_key);
    p.unseal = o.dir / "unseal.txt";
    raft::write_private_file(p.unseal, o.unseal_passphrase + "\n");
    return p;
}

}  // namespace kythira::iam::testing
