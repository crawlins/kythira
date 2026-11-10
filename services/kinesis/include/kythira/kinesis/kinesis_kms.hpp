// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file kinesis_kms.hpp
/// @brief Encryption at rest (`.kiro/specs/kinesis-compatible-streams/` task 14,
///        Requirement 14.1): the `kms_key_provider` concept, the local
///        provider sealed like the CA key, and sealing records.
///
/// An encrypted stream seals each record with an AES-256-GCM data key. The
/// gateway that takes the write seals it before proposing, so the Raft log,
/// the segments on every replica, peer fetches and tiered objects all carry
/// ciphertext, and the replicas stay byte-identical. The data key itself is
/// stored in the catalog only wrapped by a key provider; each node unwraps
/// it once and keeps it in memory.
///
/// A sealed stored record is:
///
///     u16 0xFFFF | u32 generation | 12-byte nonce | ciphertext | 16-byte tag
///
/// The ciphertext is the plaintext stored record (partition key and data).
/// The marker cannot start a plaintext record (partition keys are at most
/// 256 bytes), so one shard holds both kinds after encryption starts or
/// stops. The additional data is the shard's routing key and the
/// generation, so a record copied into another shard or relabelled with
/// another key does not open. The nonce is random: 96 bits leave the
/// chance of a repeat under one key negligible at any write rate a shard
/// takes before its stream is re-keyed.
///
/// A provider is anything with
///
/// - `generate_data_key(key_id, context)`: a fresh 256-bit key and that key
///   wrapped under `key_id`, and the provider's resolved id for it;
/// - `decrypt_data_key(key_id, wrapped, context)`: the key back.
///
/// Both throw `kms_error` with what went wrong, which the service answers
/// as the matching `KMS*Exception`. `key_provider` erases the type, and an
/// empty one means no provider is configured on this node.
///
/// `local_kms_provider` wraps data keys like `encrypt_ca_private_key` seals
/// the CA key: AES-256-GCM under a key derived with PBKDF2-HMAC-SHA256
/// (200 000 iterations, a fresh random salt per wrap) from the operator's
/// passphrase. Every node must have the same passphrase. Any key id names
/// a key; the id and the encryption context are bound to the wrapped key
/// as additional data, so it does not unwrap under another id.

#include <kythira/kinesis/kinesis_stream_state_machine.hpp>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace kythira::kinesis {

using data_key_bytes = std::array<unsigned char, 32>;
using encryption_context = std::map<std::string, std::string>;

/// What went wrong in a key provider; each maps to Kinesis's `KMS*`
/// exception of the same name, `unavailable` to `KMSNotFoundException`
/// with a message saying no provider is configured.
enum class kms_failure : std::uint8_t {
    not_found = 0,
    access_denied = 1,
    disabled = 2,
    invalid_state = 3,
    throttling = 4,
    opt_in_required = 5,
    unavailable = 6,
};

class kms_error : public std::runtime_error {
public:
    kms_error(kms_failure failure, const std::string& message)
        : std::runtime_error(message), _failure(failure) {}
    [[nodiscard]] auto failure() const noexcept -> kms_failure { return _failure; }

private:
    kms_failure _failure;
};

/// A sealed record that does not open: its tag fails, it is truncated, or
/// its generation has no data key. Data corruption, not a provider error.
class record_open_error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct generated_data_key {
    data_key_bytes _plaintext{};
    std::string _wrapped{};
    /// The provider's id for the key that wrapped it (AWS KMS: its ARN).
    std::string _key_id{};
};

template<typename P>
concept kms_key_provider = requires(P& p, const std::string& key_id, const std::string& wrapped,
                                    const encryption_context& context) {
    { p.generate_data_key(key_id, context) } -> std::same_as<generated_data_key>;
    { p.decrypt_data_key(key_id, wrapped, context) } -> std::same_as<data_key_bytes>;
};

/// Any `kms_key_provider`, shared. Calls may come from several threads at
/// once, so a provider must be thread-safe.
class key_provider {
public:
    key_provider() = default;

    template<typename P>
    requires kms_key_provider<P> && (!std::same_as<std::remove_cvref_t<P>, key_provider>)
    explicit key_provider(P p) : _impl(std::make_shared<model<P>>(std::move(p))) {}

    [[nodiscard]] explicit operator bool() const noexcept { return _impl != nullptr; }

    [[nodiscard]] auto generate_data_key(const std::string& key_id,
                                         const encryption_context& context) const
        -> generated_data_key {
        return impl().generate(key_id, context);
    }

    [[nodiscard]] auto decrypt_data_key(const std::string& key_id, const std::string& wrapped,
                                        const encryption_context& context) const -> data_key_bytes {
        return impl().decrypt(key_id, wrapped, context);
    }

private:
    struct base {
        base() = default;
        base(const base&) = delete;
        base(base&&) = delete;
        auto operator=(const base&) -> base& = delete;
        auto operator=(base&&) -> base& = delete;
        virtual ~base() = default;
        virtual auto generate(const std::string&, const encryption_context&)
            -> generated_data_key = 0;
        virtual auto decrypt(const std::string&, const std::string&, const encryption_context&)
            -> data_key_bytes = 0;
    };

    template<typename P> struct model final : base {
        explicit model(P p) : _p(std::move(p)) {}
        auto generate(const std::string& id, const encryption_context& c)
            -> generated_data_key override {
            return _p.generate_data_key(id, c);
        }
        auto decrypt(const std::string& id, const std::string& w, const encryption_context& c)
            -> data_key_bytes override {
            return _p.decrypt_data_key(id, w, c);
        }
        P _p;
    };

    [[nodiscard]] auto impl() const -> base& {
        if (!_impl) {
            throw kms_error(kms_failure::unavailable,
                            "No key provider is configured on this endpoint.");
        }
        return *_impl;
    }

    std::shared_ptr<base> _impl;
};

namespace kms_detail {

inline constexpr std::size_t k_nonce_len = 12;
inline constexpr std::size_t k_tag_len = 16;

inline void random_fill(std::span<unsigned char> out) {
    if (!out.empty() && RAND_bytes(out.data(), static_cast<int>(out.size())) != 1) {
        throw std::runtime_error("kinesis kms: RAND_bytes failed");
    }
}

struct cipher_ctx {
    EVP_CIPHER_CTX* _ctx = EVP_CIPHER_CTX_new();
    cipher_ctx() {
        if (_ctx == nullptr) {
            throw std::runtime_error("kinesis kms: EVP_CIPHER_CTX_new failed");
        }
    }
    cipher_ctx(const cipher_ctx&) = delete;
    cipher_ctx(cipher_ctx&&) = delete;
    auto operator=(const cipher_ctx&) -> cipher_ctx& = delete;
    auto operator=(cipher_ctx&&) -> cipher_ctx& = delete;
    ~cipher_ctx() { EVP_CIPHER_CTX_free(_ctx); }
};

/// AES-256-GCM of `plaintext`: ciphertext then tag, appended to `out`.
inline void gcm_seal(const data_key_bytes& key, std::span<const unsigned char> nonce,
                     std::string_view aad, std::span<const std::byte> plaintext,
                     std::vector<std::byte>& out) {
    cipher_ctx c;
    int len = 0;
    if (EVP_EncryptInit_ex(c._ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1 ||
        EVP_CIPHER_CTX_ctrl(c._ctx, EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(nonce.size()),
                            nullptr) != 1 ||
        EVP_EncryptInit_ex(c._ctx, nullptr, nullptr, key.data(), nonce.data()) != 1 ||
        (!aad.empty() && EVP_EncryptUpdate(c._ctx, nullptr, &len,
                                           reinterpret_cast<const unsigned char*>(aad.data()),
                                           static_cast<int>(aad.size())) != 1)) {
        throw std::runtime_error("kinesis kms: AES-GCM setup failed");
    }
    const auto at = out.size();
    out.resize(at + plaintext.size() + k_tag_len);
    auto* dst = reinterpret_cast<unsigned char*>(out.data() + at);
    int total = 0;
    if (!plaintext.empty() &&
        EVP_EncryptUpdate(c._ctx, dst, &len,
                          reinterpret_cast<const unsigned char*>(plaintext.data()),
                          static_cast<int>(plaintext.size())) != 1) {
        throw std::runtime_error("kinesis kms: AES-GCM encrypt failed");
    }
    total += plaintext.empty() ? 0 : len;
    if (EVP_EncryptFinal_ex(c._ctx, dst + total, &len) != 1 ||
        EVP_CIPHER_CTX_ctrl(c._ctx, EVP_CTRL_GCM_GET_TAG, static_cast<int>(k_tag_len),
                            dst + plaintext.size()) != 1) {
        throw std::runtime_error("kinesis kms: AES-GCM finish failed");
    }
}

/// The plaintext of ciphertext-then-tag `sealed`, or nullopt when the tag
/// does not match.
[[nodiscard]] inline auto gcm_open(const data_key_bytes& key, std::span<const unsigned char> nonce,
                                   std::string_view aad, std::span<const std::byte> sealed)
    -> std::optional<std::vector<std::byte>> {
    if (sealed.size() < k_tag_len) {
        return std::nullopt;
    }
    const auto body = sealed.size() - k_tag_len;
    cipher_ctx c;
    int len = 0;
    if (EVP_DecryptInit_ex(c._ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1 ||
        EVP_CIPHER_CTX_ctrl(c._ctx, EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(nonce.size()),
                            nullptr) != 1 ||
        EVP_DecryptInit_ex(c._ctx, nullptr, nullptr, key.data(), nonce.data()) != 1 ||
        (!aad.empty() && EVP_DecryptUpdate(c._ctx, nullptr, &len,
                                           reinterpret_cast<const unsigned char*>(aad.data()),
                                           static_cast<int>(aad.size())) != 1)) {
        throw std::runtime_error("kinesis kms: AES-GCM setup failed");
    }
    std::vector<std::byte> out(body);
    auto* dst = reinterpret_cast<unsigned char*>(out.data());
    int total = 0;
    if (body > 0) {
        if (EVP_DecryptUpdate(c._ctx, dst, &len,
                              reinterpret_cast<const unsigned char*>(sealed.data()),
                              static_cast<int>(body)) != 1) {
            return std::nullopt;
        }
        total = len;
    }
    std::array<unsigned char, k_tag_len> tag{};
    std::memcpy(tag.data(), sealed.data() + body, k_tag_len);
    if (EVP_CIPHER_CTX_ctrl(c._ctx, EVP_CTRL_GCM_SET_TAG, static_cast<int>(k_tag_len),
                            tag.data()) != 1 ||
        EVP_DecryptFinal_ex(c._ctx, dst + total, &len) != 1) {
        OPENSSL_cleanse(out.data(), out.size());
        return std::nullopt;
    }
    return out;
}

/// Length-prefixed, so no two (id, context) pairs give the same bytes.
[[nodiscard]] inline auto binding(std::string_view label, std::string_view key_id,
                                  const encryption_context& context) -> std::string {
    std::string out(label);
    const auto put = [&](std::string_view v) {
        const auto n = static_cast<std::uint32_t>(v.size());
        for (int shift = 24; shift >= 0; shift -= 8) {
            out.push_back(static_cast<char>((n >> shift) & 0xFFU));
        }
        out.append(v);
    };
    put(key_id);
    for (const auto& [k, v] : context) {
        put(k);
        put(v);
    }
    return out;
}

[[nodiscard]] inline auto record_aad(std::string_view routing_key, std::uint32_t generation)
    -> std::string {
    std::string out(routing_key);
    out.push_back('\0');
    for (int shift = 24; shift >= 0; shift -= 8) {
        out.push_back(static_cast<char>((generation >> shift) & 0xFFU));
    }
    return out;
}

}  // namespace kms_detail

// ---- sealed records -------------------------------------------------------

/// `partition_key` and `data` sealed under `key` (generation `generation`)
/// for the shard at `routing_key`.
[[nodiscard]] inline auto seal_record(const data_key_bytes& key, std::uint32_t generation,
                                      std::string_view routing_key, std::string_view partition_key,
                                      std::span<const std::byte> data) -> std::vector<std::byte> {
    auto plain = encode_stored_record(partition_key, data);
    std::array<unsigned char, kms_detail::k_nonce_len> nonce{};
    kms_detail::random_fill(nonce);
    std::vector<std::byte> out;
    out.reserve(6 + nonce.size() + plain.size() + kms_detail::k_tag_len);
    out.push_back(std::byte{0xFF});
    out.push_back(std::byte{0xFF});
    for (int shift = 24; shift >= 0; shift -= 8) {
        out.push_back(static_cast<std::byte>((generation >> shift) & 0xFFU));
    }
    for (const auto b : nonce) {
        out.push_back(static_cast<std::byte>(b));
    }
    kms_detail::gcm_seal(key, nonce, kms_detail::record_aad(routing_key, generation), plain, out);
    OPENSSL_cleanse(plain.data(), plain.size());
    return out;
}

/// The data key generation a sealed record names; nullopt for a plaintext
/// or truncated one.
[[nodiscard]] inline auto sealed_generation(std::span<const std::byte> bytes)
    -> std::optional<std::uint32_t> {
    if (!is_sealed_record(bytes) || bytes.size() < k_sealed_record_overhead) {
        return std::nullopt;
    }
    std::uint32_t g = 0;
    for (std::size_t i = 2; i < 6; ++i) {
        g = (g << 8) | std::to_integer<std::uint32_t>(bytes[i]);
    }
    return g;
}

/// The stored record inside sealed `bytes`. Throws `record_open_error` when
/// it does not open under `key` for `routing_key`.
[[nodiscard]] inline auto open_record(const data_key_bytes& key, std::string_view routing_key,
                                      std::span<const std::byte> bytes) -> stored_record {
    const auto generation = sealed_generation(bytes);
    if (!generation) {
        throw record_open_error("kinesis kms: not a sealed record");
    }
    std::array<unsigned char, kms_detail::k_nonce_len> nonce{};
    std::memcpy(nonce.data(), bytes.data() + 6, nonce.size());
    auto plain = kms_detail::gcm_open(key, nonce, kms_detail::record_aad(routing_key, *generation),
                                      bytes.subspan(6 + nonce.size()));
    if (!plain) {
        throw record_open_error("kinesis kms: a sealed record failed its tag");
    }
    try {
        auto out = decode_stored_record(*plain);
        OPENSSL_cleanse(plain->data(), plain->size());
        return out;
    } catch (const std::exception&) {
        throw record_open_error("kinesis kms: a sealed record holds no stored record");
    }
}

// ---- the local provider ---------------------------------------------------

/// Data keys wrapped under a passphrase-derived key (see the file comment).
/// Copies share one derived-key cache.
class local_kms_provider {
public:
    static constexpr int k_pbkdf2_iterations = 200'000;
    static constexpr std::size_t k_salt_len = 16;
    static constexpr std::size_t k_max_key_id = 2048;

    explicit local_kms_provider(std::string passphrase)
        : _state(std::make_shared<state>(std::move(passphrase))) {
        if (_state->_passphrase.empty()) {
            throw std::invalid_argument("local_kms_provider: empty passphrase");
        }
    }

    [[nodiscard]] auto generate_data_key(const std::string& key_id,
                                         const encryption_context& context) const
        -> generated_data_key {
        check_id(key_id);
        generated_data_key out;
        kms_detail::random_fill(out._plaintext);
        std::array<unsigned char, k_salt_len> salt{};
        std::array<unsigned char, kms_detail::k_nonce_len> nonce{};
        kms_detail::random_fill(salt);
        kms_detail::random_fill(nonce);
        const auto master = derived(salt);
        std::vector<std::byte> wrapped;
        wrapped.push_back(std::byte{k_version});
        for (const auto b : salt) {
            wrapped.push_back(static_cast<std::byte>(b));
        }
        for (const auto b : nonce) {
            wrapped.push_back(static_cast<std::byte>(b));
        }
        kms_detail::gcm_seal(master, nonce, kms_detail::binding(k_label, key_id, context),
                             std::as_bytes(std::span(out._plaintext)), wrapped);
        out._wrapped.assign(reinterpret_cast<const char*>(wrapped.data()), wrapped.size());
        out._key_id = key_id;
        return out;
    }

    [[nodiscard]] auto decrypt_data_key(const std::string& key_id, const std::string& wrapped,
                                        const encryption_context& context) const -> data_key_bytes {
        check_id(key_id);
        constexpr auto header = 1 + k_salt_len + kms_detail::k_nonce_len;
        if (wrapped.size() != header + 32 + kms_detail::k_tag_len ||
            static_cast<unsigned char>(wrapped[0]) != k_version) {
            throw kms_error(kms_failure::invalid_state, "The wrapped data key is malformed.");
        }
        const auto* raw = reinterpret_cast<const unsigned char*>(wrapped.data());
        std::array<unsigned char, k_salt_len> salt{};
        std::array<unsigned char, kms_detail::k_nonce_len> nonce{};
        std::memcpy(salt.data(), raw + 1, salt.size());
        std::memcpy(nonce.data(), raw + 1 + salt.size(), nonce.size());
        const auto master = derived(salt);
        auto plain =
            kms_detail::gcm_open(master, nonce, kms_detail::binding(k_label, key_id, context),
                                 std::as_bytes(std::span(wrapped)).subspan(header));
        if (!plain || plain->size() != 32) {
            throw kms_error(kms_failure::invalid_state, "The data key does not unwrap under key " +
                                                            key_id +
                                                            " with this endpoint's passphrase.");
        }
        data_key_bytes out{};
        std::memcpy(out.data(), plain->data(), out.size());
        OPENSSL_cleanse(plain->data(), plain->size());
        return out;
    }

private:
    static constexpr unsigned char k_version = 1;
    static constexpr std::string_view k_label = "kythira-kinesis-local-kms-v1";

    struct state {
        explicit state(std::string p) : _passphrase(std::move(p)) {}
        state(const state&) = delete;
        state(state&&) = delete;
        auto operator=(const state&) -> state& = delete;
        auto operator=(state&&) -> state& = delete;
        ~state() {
            OPENSSL_cleanse(_passphrase.data(), _passphrase.size());
            for (auto& [salt, key] : _keys) {
                OPENSSL_cleanse(key.data(), key.size());
            }
        }
        std::string _passphrase;
        std::mutex _mutex;
        std::map<std::array<unsigned char, k_salt_len>, data_key_bytes> _keys;
    };

    static void check_id(const std::string& key_id) {
        if (key_id.empty() || key_id.size() > k_max_key_id) {
            throw kms_error(kms_failure::not_found, "Invalid KeyId.");
        }
    }

    // PBKDF2 is slow on purpose: derive once per salt. A salt is one data
    // key's, so the cache grows with the data keys this node has seen.
    [[nodiscard]] auto derived(const std::array<unsigned char, k_salt_len>& salt) const
        -> data_key_bytes {
        const std::scoped_lock lock(_state->_mutex);
        if (const auto it = _state->_keys.find(salt); it != _state->_keys.end()) {
            return it->second;
        }
        data_key_bytes key{};
        if (PKCS5_PBKDF2_HMAC(_state->_passphrase.data(),
                              static_cast<int>(_state->_passphrase.size()), salt.data(),
                              static_cast<int>(salt.size()), k_pbkdf2_iterations, EVP_sha256(),
                              static_cast<int>(key.size()), key.data()) != 1) {
            throw std::runtime_error("local_kms_provider: PBKDF2 failed");
        }
        _state->_keys.emplace(salt, key);
        return key;
    }

    std::shared_ptr<state> _state;
};

static_assert(kms_key_provider<local_kms_provider>);

}  // namespace kythira::kinesis
