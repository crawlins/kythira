// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

// The ledger's 32-byte hash type and SHA-256 (.kiro/specs/verifiable-ledger/).
// Every hash in the ledger (Ion hashes, block and revision hashes, Merkle
// nodes, digests) is a SHA-256 output held as a hash256.

#include <openssl/evp.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace kythira::ledger {

inline constexpr std::size_t hash_size = 32;

/// A SHA-256 output.
using hash256 = std::array<std::uint8_t, hash_size>;

/// SHA-256 over the concatenation of `parts`, in order.
inline auto sha256(std::initializer_list<std::span<const std::uint8_t>> parts) -> hash256 {
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (ctx == nullptr) {
        throw std::runtime_error("sha256: EVP_MD_CTX_new failed");
    }
    hash256 out{};
    unsigned int length = 0;
    bool ok = EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) == 1;
    for (const auto part : parts) {
        ok = ok && EVP_DigestUpdate(ctx, part.data(), part.size()) == 1;
    }
    ok = ok && EVP_DigestFinal_ex(ctx, out.data(), &length) == 1;
    EVP_MD_CTX_free(ctx);
    if (!ok || length != hash_size) {
        throw std::runtime_error("sha256: digest failed");
    }
    return out;
}

/// SHA-256 of one byte string.
inline auto sha256(std::span<const std::uint8_t> bytes) -> hash256 {
    return sha256({bytes});
}

/// Lower-case hex of a hash, for logs, errors and tests.
inline auto to_hex(std::span<const std::uint8_t> bytes) -> std::string {
    static constexpr std::string_view digits = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const std::uint8_t b : bytes) {
        out.push_back(digits[b >> 4U]);
        out.push_back(digits[b & 0x0FU]);
    }
    return out;
}

}  // namespace kythira::ledger
