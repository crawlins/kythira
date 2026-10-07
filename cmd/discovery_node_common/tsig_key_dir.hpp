// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Reads the TSIG key the bind9 fixture publishes (docker/bind9/entrypoint.sh)
// for the dns and dns_sd discovery nodes. The fixture zone accepts only
// signed RFC 2136 updates, so a node started with TSIG_KEY_DIR signs every
// UPDATE with the key it finds there.
//
// The directory holds two files: `name` (the key name, e.g.
// "kythira-update.") and `secret` (its base64 HMAC-SHA256 secret), the same
// layout scripts/dns-test-server.sh writes for the non-container test.

#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace kythira::discovery_node {

struct tsig_key {
    std::string name;
    std::string secret_base64;
};

// Throws std::runtime_error naming the file when either is missing or empty:
// a node told to sign must not fall back to unsigned updates the server
// will refuse anyway, with a less useful error.
[[nodiscard]] inline auto read_tsig_key_dir(const std::filesystem::path& dir) -> tsig_key {
    auto read = [&](const char* leaf) {
        const auto path = dir / leaf;
        std::ifstream in(path);
        std::string value{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
        while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) {
            value.pop_back();
        }
        if (!in.good() && !in.eof()) {
            throw std::runtime_error("cannot read TSIG key file " + path.string());
        }
        if (value.empty()) {
            throw std::runtime_error("TSIG key file " + path.string() + " is missing or empty");
        }
        return value;
    };
    return tsig_key{read("name"), read("secret")};
}

}  // namespace kythira::discovery_node
