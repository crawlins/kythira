// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// iam_test_certs: writes the throwaway PKI of iam_test_pki.hpp into a
// directory, for the iam_node compose scenario (services/iam/tests/docker/).
// Test use only: the CA key is discarded, and the unseal passphrase is a
// fixed test value.
//
//   iam_test_certs --out <dir> [--if-absent] --node <id>[=<dns>[,<dns>...]] ...
//
// --if-absent leaves an existing PKI alone (it exits 0 when <dir>/ca.pem
// exists). Compose re-runs a one-shot service that a starting node depends
// on, so without it a `compose start` of one node would mint a new CA that
// the running nodes do not trust.

#include "iam_test_pki.hpp"

#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>

int main(int argc, char** argv) {
    kythira::iam::testing::test_pki_options o;
    bool if_absent = false;
    try {
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--if-absent") {
                if_absent = true;
                continue;
            }
            if (i + 1 >= argc) {
                throw std::invalid_argument("missing value for " + arg);
            }
            const std::string value = argv[++i];
            if (arg == "--out") {
                o.dir = value;
            } else if (arg == "--node") {
                const auto eq = value.find('=');
                auto id = kythira::parse_host_peer_node_id(value.substr(0, eq));
                if (!id) {
                    throw std::invalid_argument("--node " + value + ": " + id.error());
                }
                if (eq != std::string::npos) {
                    std::stringstream names(value.substr(eq + 1));
                    std::string name;
                    while (std::getline(names, name, ',')) {
                        if (!name.empty()) {
                            o.host_names[kythira::host_node_id_text(*id)].push_back(name);
                        }
                    }
                }
                o.nodes.push_back(*id);
            } else {
                throw std::invalid_argument("unknown option " + arg);
            }
        }
        if (o.dir.empty() || o.nodes.empty()) {
            throw std::invalid_argument("--out and at least one --node are required");
        }
        if (if_absent && std::filesystem::exists(o.dir / "ca.pem")) {
            std::cout << "iam_test_certs: " << o.dir << " already holds a PKI; left as it is\n";
            return 0;
        }
        std::filesystem::create_directories(o.dir);
        (void)kythira::iam::testing::write_test_pki(o);
        std::cout << "iam_test_certs: wrote " << o.nodes.size() << " node certificates to " << o.dir
                  << "\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr
            << "iam_test_certs: " << e.what()
            << "\nUsage: iam_test_certs --out <dir> [--if-absent] --node <id>[=<dns>,...] ...\n";
        return 1;
    }
}
