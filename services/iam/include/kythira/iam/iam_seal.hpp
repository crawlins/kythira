// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file iam_seal.hpp
/// @brief Sealing for the IAM state machine's secrets
///        (`.kiro/specs/iam-compatible-auth/` design Component 4,
///        Requirement 4.5).
///
/// The key-encryption key (KEK) is PBKDF2-HMAC-SHA256 of the unseal
/// passphrase over a per-cluster salt, with the CA's parameters (200 000
/// iterations, a 32-byte key). The salt travels in the first
/// `create_account` command, so every replica derives the same KEK from the
/// same passphrase. A secret is sealed with AES-256-GCM under the KEK, a
/// 96-bit random nonce, and associated data naming what it belongs to (an
/// access key's id, an STS key's kid), so a sealed blob moved onto another
/// record fails to open.
///
/// Everything here runs on the leader (or on a replica opening a secret to
/// authenticate), never inside `apply()`: sealing draws random nonces, so it
/// must happen before the command is proposed.
///
/// Wire format, base64url without padding: `nonce (12) | tag (16) | ciphertext`.

#include <raft/jwt.hpp>  // kythira::jose::base64url_*

#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace kythira::iam {

/// A derived key-encryption key. Wiped on destruction.
class seal_key {
public:
    static constexpr std::size_t size = 32;

    seal_key() = default;
    explicit seal_key(const std::array<unsigned char, size>& bytes) : _bytes(bytes), _set(true) {}
    seal_key(const seal_key&) = default;
    auto operator=(const seal_key&) -> seal_key& = default;
    seal_key(seal_key&&) = default;
    auto operator=(seal_key&&) -> seal_key& = default;
    ~seal_key() { OPENSSL_cleanse(_bytes.data(), _bytes.size()); }

    [[nodiscard]] auto data() const noexcept -> const unsigned char* { return _bytes.data(); }
    [[nodiscard]] auto empty() const noexcept -> bool { return !_set; }

private:
    std::array<unsigned char, size> _bytes{};
    bool _set = false;
};

namespace seal_detail {

constexpr int pbkdf2_iterations = 200000;  // the CA's (ca_state_machine.hpp)
constexpr std::size_t salt_len = 16;
constexpr std::size_t nonce_len = 12;
constexpr std::size_t tag_len = 16;

[[noreturn]] inline void throw_openssl(std::string_view what) {
    ERR_clear_error();
    throw std::runtime_error("iam seal: " + std::string(what) + " failed");
}

struct ctx_guard {
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    ctx_guard() {
        if (ctx == nullptr) {
            throw_openssl("EVP_CIPHER_CTX_new");
        }
    }
    ctx_guard(const ctx_guard&) = delete;
    auto operator=(const ctx_guard&) -> ctx_guard& = delete;
    ctx_guard(ctx_guard&&) = delete;
    auto operator=(ctx_guard&&) -> ctx_guard& = delete;
    ~ctx_guard() { EVP_CIPHER_CTX_free(ctx); }
};

inline auto as_uchar(std::string_view s) -> const unsigned char* {
    return reinterpret_cast<const unsigned char*>(
        s.data());  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
}

}  // namespace seal_detail

/// Random bytes from OpenSSL's CSPRNG. Throws if the generator fails.
[[nodiscard]] inline auto random_bytes(std::size_t n) -> std::vector<unsigned char> {
    std::vector<unsigned char> out(n);
    if (n > 0 && RAND_bytes(out.data(), static_cast<int>(n)) != 1) {
        seal_detail::throw_openssl("RAND_bytes");
    }
    return out;
}

/// A fresh per-cluster KEK salt, base64url. The leader stamps it into the
/// first `create_account`.
[[nodiscard]] inline auto generate_kek_salt() -> std::string {
    return jose::base64url_encode(random_bytes(seal_detail::salt_len));
}

/// Derives the KEK from the unseal passphrase and the cluster's salt
/// (base64url, as `generate_kek_salt()` returns it).
[[nodiscard]] inline auto derive_seal_key(std::string_view passphrase, std::string_view salt_b64)
    -> seal_key {
    auto salt = jose::base64url_decode(salt_b64);
    if (!salt || salt->size() != seal_detail::salt_len) {
        throw std::invalid_argument("iam seal: the KEK salt is not 16 base64url bytes");
    }
    std::array<unsigned char, seal_key::size> key{};
    if (PKCS5_PBKDF2_HMAC(passphrase.data(), static_cast<int>(passphrase.size()), salt->data(),
                          static_cast<int>(salt->size()), seal_detail::pbkdf2_iterations,
                          EVP_sha256(), static_cast<int>(key.size()), key.data()) != 1) {
        seal_detail::throw_openssl("PBKDF2");
    }
    seal_key out(key);
    OPENSSL_cleanse(key.data(), key.size());
    return out;
}

/// Seals `plaintext` under `key`, binding it to `associated_data`.
[[nodiscard]] inline auto seal_secret(const seal_key& key, std::string_view plaintext,
                                      std::string_view associated_data) -> std::string {
    using namespace seal_detail;
    if (key.empty()) {
        throw std::invalid_argument("iam seal: no key");
    }
    const auto nonce = random_bytes(nonce_len);
    ctx_guard g;
    if (EVP_EncryptInit_ex(g.ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1 ||
        EVP_CIPHER_CTX_ctrl(g.ctx, EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(nonce_len), nullptr) !=
            1 ||
        EVP_EncryptInit_ex(g.ctx, nullptr, nullptr, key.data(), nonce.data()) != 1) {
        throw_openssl("AES-GCM init");
    }
    int len = 0;
    if (!associated_data.empty() &&
        EVP_EncryptUpdate(g.ctx, nullptr, &len, as_uchar(associated_data),
                          static_cast<int>(associated_data.size())) != 1) {
        throw_openssl("AES-GCM associated data");
    }
    std::vector<unsigned char> out(nonce_len + tag_len + plaintext.size());
    std::copy(nonce.begin(), nonce.end(), out.begin());
    unsigned char* ct = out.data() + nonce_len + tag_len;
    if (!plaintext.empty() && EVP_EncryptUpdate(g.ctx, ct, &len, as_uchar(plaintext),
                                                static_cast<int>(plaintext.size())) != 1) {
        throw_openssl("AES-GCM encrypt");
    }
    int fin = 0;
    if (EVP_EncryptFinal_ex(g.ctx, ct + len, &fin) != 1 ||
        EVP_CIPHER_CTX_ctrl(g.ctx, EVP_CTRL_GCM_GET_TAG, static_cast<int>(tag_len),
                            out.data() + nonce_len) != 1) {
        throw_openssl("AES-GCM finish");
    }
    return jose::base64url_encode(out);
}

/// Opens a blob from `seal_secret()`. Returns nothing when the key, the
/// associated data or the blob is wrong: callers fail closed.
[[nodiscard]] inline auto open_secret(const seal_key& key, std::string_view sealed,
                                      std::string_view associated_data)
    -> std::optional<std::string> {
    using namespace seal_detail;
    if (key.empty()) {
        return std::nullopt;
    }
    auto blob = jose::base64url_decode(sealed);
    if (!blob || blob->size() < nonce_len + tag_len) {
        return std::nullopt;
    }
    ctx_guard g;
    if (EVP_DecryptInit_ex(g.ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1 ||
        EVP_CIPHER_CTX_ctrl(g.ctx, EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(nonce_len), nullptr) !=
            1 ||
        EVP_DecryptInit_ex(g.ctx, nullptr, nullptr, key.data(), blob->data()) != 1) {
        ERR_clear_error();
        return std::nullopt;
    }
    int len = 0;
    if (!associated_data.empty() &&
        EVP_DecryptUpdate(g.ctx, nullptr, &len, as_uchar(associated_data),
                          static_cast<int>(associated_data.size())) != 1) {
        ERR_clear_error();
        return std::nullopt;
    }
    const std::size_t ct_len = blob->size() - nonce_len - tag_len;
    std::string plain(ct_len, '\0');
    auto* out = reinterpret_cast<unsigned char*>(
        plain.data());  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
    if (ct_len > 0 && EVP_DecryptUpdate(g.ctx, out, &len, blob->data() + nonce_len + tag_len,
                                        static_cast<int>(ct_len)) != 1) {
        ERR_clear_error();
        return std::nullopt;
    }
    if (EVP_CIPHER_CTX_ctrl(g.ctx, EVP_CTRL_GCM_SET_TAG, static_cast<int>(tag_len),
                            blob->data() + nonce_len) != 1) {
        ERR_clear_error();
        return std::nullopt;
    }
    int fin = 0;
    if (EVP_DecryptFinal_ex(g.ctx, out + len, &fin) != 1) {
        ERR_clear_error();
        OPENSSL_cleanse(plain.data(), plain.size());
        return std::nullopt;
    }
    return plain;
}

}  // namespace kythira::iam
