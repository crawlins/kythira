// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file oscore.hpp
/// @brief OSCORE (RFC 8613) — Object Security for Constrained RESTful
///        Environments — implemented against CoAP *message bytes*, with no
///        dependency on any CoAP library.
///
/// WHY THIS EXISTS
/// ---------------
/// kythira's `coap_security_provider` (coap_security.hpp) offers OSCORE only by
/// asking libcoap to do it: `oscore_provider` is a wrapper over
/// `coap_context_oscore_server()` / `coap_new_client_session_oscore()`, and its
/// `protect`/`unprotect` hooks are identity passthroughs because libcoap
/// applies the transform below its own PDU API. That leaves any *other* CoAP
/// backend — the libnyoci one in `coap_transport_libnyoci_impl.hpp`, or a
/// future one — with no way to speak OSCORE at all, since libcoap's
/// implementation is inseparable from libcoap's session and PDU types.
///
/// So this is the "acquire an OSCORE implementation" half of the follow-up
/// recorded in doc/TODO.md. It is deliberately transport-neutral: it takes a
/// serialized CoAP message in, and gives a serialized CoAP message out. It
/// needs OpenSSL (HMAC-SHA-256 for HKDF, AES-128-CCM for the AEAD) and nothing
/// else, so it can be included from either backend — including the libnyoci one,
/// which cannot include a single libcoap header.
///
/// SCOPE
/// -----
/// RFC 8613's core: security-context derivation (Section 3.2.1), the AEAD nonce
/// (5.2), plaintext (5.3) and AAD (5.4) constructions, OSCORE option
/// compression (6.1), and the request/response protect and verify procedures
/// (8.1-8.4), for the mandatory-to-implement algorithms — HKDF SHA-256 and
/// AES-CCM-16-64-128.
///
/// Deliberately out of scope, and each of these is *refused* rather than
/// silently mishandled: Observe (8.3.1, 8.4.2), block-wise over OSCORE
/// (8.2.1, 8.4.1), and OSCORE-in-OSCORE. The transports that use this do not
/// offer Observe, and the libnyoci backend has no Block1 to begin with.
///
/// CONFORMANCE
/// -----------
/// Verified against every test vector in RFC 8613 Appendix C — key derivation
/// (C.1-C.3), protected requests (C.4-C.6) and protected responses (C.7-C.8) —
/// byte for byte, in tests/oscore_rfc8613_vectors_test.cpp. Those vectors are
/// the reason this is worth trusting; do not change the crypto here without
/// re-running them.

#include <raft/coap_exceptions.hpp>
#include <raft/coap_security.hpp>

#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace kythira::oscore {

/// Raised when an inbound message cannot be verified: a failed AEAD tag, a
/// replayed sequence number, a malformed OSCORE option. Distinct from
/// `coap_security_config_error` (a local misconfiguration) because this one is
/// attacker-triggerable and must never be treated as a reason to fall back.
class verification_error : public coap_security_error {
public:
    explicit verification_error(const std::string& message)
        : coap_security_error("OSCORE verification failed: " + message) {}
};

/// Raised for the parts of RFC 8613 this implementation does not cover, so an
/// unsupported message is rejected rather than silently mis-protected.
class unsupported_feature_error : public coap_security_error {
public:
    explicit unsupported_feature_error(const std::string& message)
        : coap_security_error("OSCORE: " + message) {}
};

// ── Algorithm parameters (RFC 8613 Section 3.1, RFC 8152 Section 10.2) ─────
// Only AES-CCM-16-64-128 is implemented; it is the mandatory-to-implement
// algorithm and the default in kythira's oscore_credentials.
inline constexpr int aead_alg_aes_ccm_16_64_128 = 10;
inline constexpr std::size_t aead_key_length = 16;
inline constexpr std::size_t aead_nonce_length = 13;
inline constexpr std::size_t aead_tag_length = 8;
/// RFC 8613 Section 5.4: implementations MUST set this to 1.
inline constexpr std::uint8_t oscore_version = 1;
/// RFC 7252 Section 5.10: the OSCORE option number (RFC 8613 Section 13.1).
inline constexpr std::uint16_t coap_option_oscore = 9;

// ── CBOR ───────────────────────────────────────────────────────────────────
// RFC 8613 needs only a handful of CBOR shapes -- small arrays, byte strings,
// text strings and small unsigned integers -- so these are hand-rolled rather
// than pulling in a CBOR library. Every encoding below is checked against the
// `info`, `aad_array` and `AAD` hex strings printed in Appendix C.

namespace detail {

inline auto cbor_append_head(std::vector<std::byte>& out, std::uint8_t major, std::uint64_t value)
    -> void {
    const auto tag = static_cast<std::uint8_t>(major << 5U);
    if (value < 24) {
        out.push_back(static_cast<std::byte>(tag | static_cast<std::uint8_t>(value)));
    } else if (value <= 0xFF) {
        out.push_back(static_cast<std::byte>(tag | 24U));
        out.push_back(static_cast<std::byte>(value));
    } else if (value <= 0xFFFF) {
        out.push_back(static_cast<std::byte>(tag | 25U));
        out.push_back(static_cast<std::byte>((value >> 8U) & 0xFFU));
        out.push_back(static_cast<std::byte>(value & 0xFFU));
    } else {
        // OSCORE never needs anything larger here (ids, salts and option
        // blocks are all small), so refuse rather than grow the encoder.
        throw unsupported_feature_error("CBOR value too large for this encoder");
    }
}

inline auto cbor_append_uint(std::vector<std::byte>& out, std::uint64_t value) -> void {
    cbor_append_head(out, 0, value);
}

inline auto cbor_append_bstr(std::vector<std::byte>& out, std::span<const std::byte> bytes)
    -> void {
    cbor_append_head(out, 2, bytes.size());
    out.insert(out.end(), bytes.begin(), bytes.end());
}

inline auto cbor_append_tstr(std::vector<std::byte>& out, std::string_view text) -> void {
    cbor_append_head(out, 3, text.size());
    for (const char c : text) {
        out.push_back(static_cast<std::byte>(c));
    }
}

inline auto cbor_append_array_head(std::vector<std::byte>& out, std::size_t count) -> void {
    cbor_append_head(out, 4, count);
}

inline auto cbor_append_null(std::vector<std::byte>& out) -> void {
    out.push_back(std::byte{0xF6});
}

}  // namespace detail

/// The `info` structure of RFC 8613 Section 3.2.1:
/// `[ id, id_context, alg_aead, type, L ]`.
///
/// `type` is "Key" when deriving a Sender/Recipient Key and "IV" for the Common
/// IV; for the Common IV, `id` is the empty byte string.
[[nodiscard]] inline auto build_info(std::span<const std::byte> id,
                                     std::span<const std::byte> id_context, int alg_aead,
                                     std::string_view type, std::size_t length)
    -> std::vector<std::byte> {
    std::vector<std::byte> info;
    detail::cbor_append_array_head(info, 5);
    detail::cbor_append_bstr(info, id);
    if (id_context.empty()) {
        detail::cbor_append_null(info);
    } else {
        detail::cbor_append_bstr(info, id_context);
    }
    detail::cbor_append_uint(info, static_cast<std::uint64_t>(alg_aead));
    detail::cbor_append_tstr(info, type);
    detail::cbor_append_uint(info, length);
    return info;
}

/// HKDF-SHA-256 (RFC 5869) as OSCORE uses it. An absent Master Salt is the
/// empty byte string, which HKDF-Extract turns into a string of zeros
/// (RFC 8613 Section 3.2.1's closing note).
[[nodiscard]] inline auto hkdf_sha256(std::span<const std::byte> salt,
                                      std::span<const std::byte> ikm,
                                      std::span<const std::byte> info, std::size_t length)
    -> std::vector<std::byte> {
    if (length > 32) {
        // A single HMAC invocation covers every length OSCORE asks for (16 or
        // 13); multi-block expand is unnecessary and would be untested code.
        throw unsupported_feature_error("HKDF output longer than one SHA-256 block");
    }

    std::array<unsigned char, EVP_MAX_MD_SIZE> prk{};
    unsigned int prk_len = 0;
    // HKDF-Extract: PRK = HMAC-SHA-256(salt, IKM).
    const std::array<unsigned char, 1> empty_salt{};
    if (HMAC(EVP_sha256(),
             salt.empty() ? empty_salt.data() : reinterpret_cast<const unsigned char*>(salt.data()),
             salt.empty() ? 0 : static_cast<int>(salt.size()),
             reinterpret_cast<const unsigned char*>(ikm.data()), ikm.size(), prk.data(),
             &prk_len) == nullptr) {
        throw coap_security_error("OSCORE: HKDF-Extract failed");
    }

    // HKDF-Expand, single round: T(1) = HMAC-SHA-256(PRK, info || 0x01).
    std::vector<std::byte> expand_input(info.begin(), info.end());
    expand_input.push_back(std::byte{0x01});

    std::array<unsigned char, EVP_MAX_MD_SIZE> okm{};
    unsigned int okm_len = 0;
    if (HMAC(EVP_sha256(), prk.data(), static_cast<int>(prk_len),
             reinterpret_cast<const unsigned char*>(expand_input.data()), expand_input.size(),
             okm.data(), &okm_len) == nullptr) {
        throw coap_security_error("OSCORE: HKDF-Expand failed");
    }
    if (okm_len < length) {
        throw coap_security_error("OSCORE: HKDF produced too few bytes");
    }

    std::vector<std::byte> out(length);
    std::memcpy(out.data(), okm.data(), length);
    return out;
}

/// The AEAD nonce of RFC 8613 Section 5.2: the Partial IV left-padded to 5
/// bytes, the generating endpoint's ID left-padded to `nonce_length - 6`, a
/// leading length byte, all XORed with the Common IV.
[[nodiscard]] inline auto compute_nonce(std::span<const std::byte> common_iv,
                                        std::span<const std::byte> id_piv,
                                        std::span<const std::byte> partial_iv)
    -> std::vector<std::byte> {
    const std::size_t nonce_length = common_iv.size();
    if (nonce_length < 7) {
        throw unsupported_feature_error("AEAD nonces shorter than 7 bytes are not supported");
    }
    const std::size_t id_field = nonce_length - 6;
    if (id_piv.size() > id_field) {
        throw verification_error("sender ID is too long for the AEAD nonce");
    }
    if (partial_iv.size() > 5) {
        throw verification_error("Partial IV is longer than 5 bytes");
    }

    std::vector<std::byte> nonce(nonce_length, std::byte{0});
    nonce[0] = static_cast<std::byte>(id_piv.size());
    std::copy(id_piv.begin(), id_piv.end(), nonce.begin() + 1 + (id_field - id_piv.size()));
    std::copy(partial_iv.begin(), partial_iv.end(),
              nonce.begin() + static_cast<std::ptrdiff_t>(nonce_length - partial_iv.size()));
    for (std::size_t i = 0; i < nonce_length; ++i) {
        nonce[i] ^= common_iv[i];
    }
    return nonce;
}

/// The AAD of RFC 8613 Section 5.4:
/// `[ "Encrypt0", h'', bstr .cbor [ version, [alg], request_kid, request_piv, options ] ]`.
[[nodiscard]] inline auto build_aad(int alg_aead, std::span<const std::byte> request_kid,
                                    std::span<const std::byte> request_piv,
                                    std::span<const std::byte> class_i_options)
    -> std::vector<std::byte> {
    std::vector<std::byte> aad_array;
    detail::cbor_append_array_head(aad_array, 5);
    detail::cbor_append_uint(aad_array, oscore_version);
    detail::cbor_append_array_head(aad_array, 1);
    detail::cbor_append_uint(aad_array, static_cast<std::uint64_t>(alg_aead));
    detail::cbor_append_bstr(aad_array, request_kid);
    detail::cbor_append_bstr(aad_array, request_piv);
    detail::cbor_append_bstr(aad_array, class_i_options);

    std::vector<std::byte> external_aad;
    detail::cbor_append_bstr(external_aad, aad_array);

    std::vector<std::byte> aad;
    detail::cbor_append_array_head(aad, 3);
    detail::cbor_append_tstr(aad, "Encrypt0");
    detail::cbor_append_bstr(aad, {});
    aad.insert(aad.end(), external_aad.begin(), external_aad.end());
    return aad;
}

// ── AES-CCM-16-64-128 ──────────────────────────────────────────────────────

namespace detail {

struct evp_ctx_deleter {
    auto operator()(EVP_CIPHER_CTX* ctx) const -> void { EVP_CIPHER_CTX_free(ctx); }
};
using evp_ctx = std::unique_ptr<EVP_CIPHER_CTX, evp_ctx_deleter>;

}  // namespace detail

/// AEAD encryption. Returns ciphertext || tag, which is what OSCORE calls the
/// ciphertext and carries in the CoAP payload.
[[nodiscard]] inline auto aead_encrypt(std::span<const std::byte> key,
                                       std::span<const std::byte> nonce,
                                       std::span<const std::byte> aad,
                                       std::span<const std::byte> plaintext)
    -> std::vector<std::byte> {
    if (key.size() != aead_key_length || nonce.size() != aead_nonce_length) {
        throw coap_security_config_error("OSCORE: wrong key or nonce length for AES-CCM-16-64-128");
    }
    detail::evp_ctx ctx{EVP_CIPHER_CTX_new()};
    if (!ctx) {
        throw coap_security_error("OSCORE: failed to allocate a cipher context");
    }
    const auto* k = reinterpret_cast<const unsigned char*>(key.data());
    const auto* n = reinterpret_cast<const unsigned char*>(nonce.data());

    int len = 0;
    if (EVP_EncryptInit_ex(ctx.get(), EVP_aes_128_ccm(), nullptr, nullptr, nullptr) != 1 ||
        EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_AEAD_SET_IVLEN, static_cast<int>(aead_nonce_length),
                            nullptr) != 1 ||
        EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_AEAD_SET_TAG, static_cast<int>(aead_tag_length),
                            nullptr) != 1 ||
        EVP_EncryptInit_ex(ctx.get(), nullptr, nullptr, k, n) != 1) {
        throw coap_security_error("OSCORE: AES-CCM encryption setup failed");
    }
    // CCM requires the total plaintext length up front, before any AAD.
    if (EVP_EncryptUpdate(ctx.get(), nullptr, &len, nullptr, static_cast<int>(plaintext.size())) !=
        1) {
        throw coap_security_error("OSCORE: AES-CCM length declaration failed");
    }
    if (!aad.empty() && EVP_EncryptUpdate(ctx.get(), nullptr, &len,
                                          reinterpret_cast<const unsigned char*>(aad.data()),
                                          static_cast<int>(aad.size())) != 1) {
        throw coap_security_error("OSCORE: AES-CCM AAD failed");
    }

    std::vector<std::byte> out(plaintext.size() + aead_tag_length);
    auto* out_ptr = reinterpret_cast<unsigned char*>(out.data());
    if (EVP_EncryptUpdate(ctx.get(), out_ptr, &len,
                          reinterpret_cast<const unsigned char*>(plaintext.data()),
                          static_cast<int>(plaintext.size())) != 1) {
        throw coap_security_error("OSCORE: AES-CCM encryption failed");
    }
    int final_len = 0;
    if (EVP_EncryptFinal_ex(ctx.get(), out_ptr + len, &final_len) != 1) {
        throw coap_security_error("OSCORE: AES-CCM finalization failed");
    }
    if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_AEAD_GET_TAG, static_cast<int>(aead_tag_length),
                            out_ptr + plaintext.size()) != 1) {
        throw coap_security_error("OSCORE: AES-CCM tag extraction failed");
    }
    return out;
}

/// AEAD decryption of ciphertext || tag. A failed tag check raises
/// `verification_error` -- never a silent empty result.
[[nodiscard]] inline auto aead_decrypt(std::span<const std::byte> key,
                                       std::span<const std::byte> nonce,
                                       std::span<const std::byte> aad,
                                       std::span<const std::byte> ciphertext)
    -> std::vector<std::byte> {
    if (key.size() != aead_key_length || nonce.size() != aead_nonce_length) {
        throw coap_security_config_error("OSCORE: wrong key or nonce length for AES-CCM-16-64-128");
    }
    if (ciphertext.size() < aead_tag_length) {
        throw verification_error("ciphertext is shorter than the authentication tag");
    }
    const std::size_t body = ciphertext.size() - aead_tag_length;

    detail::evp_ctx ctx{EVP_CIPHER_CTX_new()};
    if (!ctx) {
        throw coap_security_error("OSCORE: failed to allocate a cipher context");
    }
    const auto* k = reinterpret_cast<const unsigned char*>(key.data());
    const auto* n = reinterpret_cast<const unsigned char*>(nonce.data());
    // EVP_CTRL_AEAD_SET_TAG takes a non-const pointer even though it only reads.
    std::array<unsigned char, aead_tag_length> tag{};
    std::memcpy(tag.data(), ciphertext.data() + body, aead_tag_length);

    int len = 0;
    if (EVP_DecryptInit_ex(ctx.get(), EVP_aes_128_ccm(), nullptr, nullptr, nullptr) != 1 ||
        EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_AEAD_SET_IVLEN, static_cast<int>(aead_nonce_length),
                            nullptr) != 1 ||
        EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_AEAD_SET_TAG, static_cast<int>(aead_tag_length),
                            tag.data()) != 1 ||
        EVP_DecryptInit_ex(ctx.get(), nullptr, nullptr, k, n) != 1) {
        throw coap_security_error("OSCORE: AES-CCM decryption setup failed");
    }
    if (EVP_DecryptUpdate(ctx.get(), nullptr, &len, nullptr, static_cast<int>(body)) != 1) {
        throw coap_security_error("OSCORE: AES-CCM length declaration failed");
    }
    if (!aad.empty() && EVP_DecryptUpdate(ctx.get(), nullptr, &len,
                                          reinterpret_cast<const unsigned char*>(aad.data()),
                                          static_cast<int>(aad.size())) != 1) {
        throw coap_security_error("OSCORE: AES-CCM AAD failed");
    }

    std::vector<std::byte> out(body);
    // For CCM, EVP_DecryptUpdate itself reports the tag check; there is no
    // EVP_DecryptFinal_ex step.
    if (EVP_DecryptUpdate(ctx.get(), reinterpret_cast<unsigned char*>(out.data()), &len,
                          reinterpret_cast<const unsigned char*>(ciphertext.data()),
                          static_cast<int>(body)) != 1) {
        throw verification_error("AEAD tag mismatch (wrong key, replayed nonce, or tampering)");
    }
    return out;
}

// ── OSCORE option (RFC 8613 Section 6.1) ───────────────────────────────────

/// The decompressed contents of an OSCORE option value.
struct option_fields {
    std::vector<std::byte> partial_iv;  //!< empty when n == 0
    std::vector<std::byte> kid;         //!< empty and absent are distinguished by `has_kid`
    std::vector<std::byte> kid_context;
    bool has_kid{false};
    bool has_kid_context{false};
};

[[nodiscard]] inline auto encode_option(const option_fields& fields) -> std::vector<std::byte> {
    if (fields.partial_iv.size() > 5) {
        throw unsupported_feature_error("Partial IV longer than 5 bytes");
    }
    if (fields.kid_context.size() > 255) {
        throw unsupported_feature_error("kid context longer than 255 bytes");
    }
    std::vector<std::byte> value;
    auto flags = static_cast<std::uint8_t>(fields.partial_iv.size());
    if (fields.has_kid) {
        flags |= 0x08U;
    }
    if (fields.has_kid_context) {
        flags |= 0x10U;
    }
    // RFC 8613 Section 6.1: an all-zero first byte carries no information, and
    // the option is then sent empty rather than as a single 0x00 byte.
    if (flags == 0) {
        return value;
    }
    value.push_back(static_cast<std::byte>(flags));
    value.insert(value.end(), fields.partial_iv.begin(), fields.partial_iv.end());
    if (fields.has_kid_context) {
        value.push_back(static_cast<std::byte>(fields.kid_context.size()));
        value.insert(value.end(), fields.kid_context.begin(), fields.kid_context.end());
    }
    if (fields.has_kid) {
        value.insert(value.end(), fields.kid.begin(), fields.kid.end());
    }
    return value;
}

[[nodiscard]] inline auto decode_option(std::span<const std::byte> value) -> option_fields {
    option_fields fields;
    if (value.empty()) {
        return fields;  // No flags: no PIV, no kid, no kid context.
    }
    const auto flags = static_cast<std::uint8_t>(value[0]);
    if ((flags & 0xE0U) != 0) {
        // RFC 8613 Section 6.1: the three most significant bits are reserved
        // and a message setting any of them is malformed.
        throw verification_error("reserved bits set in the OSCORE option");
    }
    const std::size_t piv_length = flags & 0x07U;
    if (piv_length == 6 || piv_length == 7) {
        throw verification_error("reserved Partial IV length in the OSCORE option");
    }
    std::size_t offset = 1;
    if (value.size() < offset + piv_length) {
        throw verification_error("truncated Partial IV in the OSCORE option");
    }
    fields.partial_iv.assign(value.begin() + static_cast<std::ptrdiff_t>(offset),
                             value.begin() + static_cast<std::ptrdiff_t>(offset + piv_length));
    offset += piv_length;

    if ((flags & 0x10U) != 0) {
        if (value.size() < offset + 1) {
            throw verification_error("truncated kid context length in the OSCORE option");
        }
        const auto context_length = static_cast<std::size_t>(value[offset]);
        ++offset;
        if (value.size() < offset + context_length) {
            throw verification_error("truncated kid context in the OSCORE option");
        }
        fields.has_kid_context = true;
        fields.kid_context.assign(
            value.begin() + static_cast<std::ptrdiff_t>(offset),
            value.begin() + static_cast<std::ptrdiff_t>(offset + context_length));
        offset += context_length;
    }
    if ((flags & 0x08U) != 0) {
        fields.has_kid = true;
        fields.kid.assign(value.begin() + static_cast<std::ptrdiff_t>(offset), value.end());
    } else if (offset != value.size()) {
        throw verification_error("trailing bytes in the OSCORE option with no kid flag");
    }
    return fields;
}

// ── CoAP message codec (RFC 7252 Section 3) ────────────────────────────────
// OSCORE has to take a CoAP message apart and put it back together, so a codec
// is unavoidable. It lives here rather than being borrowed from either CoAP
// library for the same reason everything else in this file does: this code has
// to work for backends that cannot include libcoap.

struct coap_option {
    std::uint16_t number{0};
    std::vector<std::byte> value;
};

struct coap_message {
    std::uint8_t version{1};
    std::uint8_t type{0};  //!< 0 CON, 1 NON, 2 ACK, 3 RST
    std::uint8_t code{0};
    std::uint16_t message_id{0};
    std::vector<std::byte> token;
    /// Ascending by number, as on the wire. Repeats of one number are allowed
    /// (Uri-Path is the common case) and their relative order is preserved.
    std::vector<coap_option> options;
    std::vector<std::byte> payload;
    bool has_payload{false};

    [[nodiscard]] auto is_request() const -> bool { return code >= 1 && code <= 31; }
};

/// RFC 8613 Figure 5: the options that stay in the *outer* message. Everything
/// not listed is Class E and moves inside the ciphertext, which is the
/// spec's default ("options not listed are Class E") and the safe direction to
/// err in -- an unknown option gets protected rather than leaked.
[[nodiscard]] inline constexpr auto is_outer_option(std::uint16_t number) -> bool {
    switch (number) {
        case 3:   // Uri-Host
        case 7:   // Uri-Port
        case 9:   // OSCORE
        case 35:  // Proxy-Uri
        case 39:  // Proxy-Scheme
            return true;
        default:
            return false;
    }
}

/// Observe (6) and the Block options (23, 27) are Class E here, which is the
/// *inner* half of RFC 8613 Figure 5's "E and U" entry for them.
///
/// Inner block options give end-to-end fragmentation: the blocks are formed
/// before protection, so each one is individually authenticated and a proxy
/// splitting the outer message cannot reach them (Section 4.1.3.4.1). Inner
/// Observe likewise travels inside the ciphertext (Section 4.1.3.5).
///
/// The *outer* forms of both are hop-by-hop hints for proxies, and this
/// implementation emits neither: it has no proxy to talk to, and an outer block
/// option would leak the message's fragmentation structure for nothing.
[[nodiscard]] inline constexpr auto is_inner_only_option(std::uint16_t number) -> bool {
    switch (number) {
        case 6:   // Observe
        case 23:  // Block2
        case 27:  // Block1
            return true;
        default:
            return false;
    }
}

namespace detail {

inline auto append_option_header(std::vector<std::byte>& out, std::uint16_t delta,
                                 std::size_t length) -> void {
    const auto nibble = [](std::size_t v) -> std::uint8_t {
        if (v < 13) {
            return static_cast<std::uint8_t>(v);
        }
        return v < 269 ? 13 : 14;
    };
    const std::uint8_t delta_nibble = nibble(delta);
    const std::uint8_t length_nibble = nibble(length);
    out.push_back(static_cast<std::byte>((delta_nibble << 4U) | length_nibble));

    const auto append_extended = [&out](std::uint8_t chosen, std::size_t value) {
        if (chosen == 13) {
            out.push_back(static_cast<std::byte>(value - 13));
        } else if (chosen == 14) {
            const auto extended = static_cast<std::uint16_t>(value - 269);
            out.push_back(static_cast<std::byte>((extended >> 8U) & 0xFFU));
            out.push_back(static_cast<std::byte>(extended & 0xFFU));
        }
    };
    append_extended(delta_nibble, delta);
    append_extended(length_nibble, length);
}

}  // namespace detail

/// Encode a run of options, delta-coded from `previous_number`. RFC 8613
/// Section 5.3 needs this twice with different starting points: the inner
/// options restart from 0 inside the plaintext, while the outer ones are
/// encoded normally.
[[nodiscard]] inline auto encode_options(std::span<const coap_option> options,
                                         std::uint16_t previous_number = 0)
    -> std::vector<std::byte> {
    std::vector<std::byte> out;
    for (const auto& option : options) {
        if (option.number < previous_number) {
            throw unsupported_feature_error("CoAP options must be encoded in ascending order");
        }
        detail::append_option_header(
            out, static_cast<std::uint16_t>(option.number - previous_number), option.value.size());
        out.insert(out.end(), option.value.begin(), option.value.end());
        previous_number = option.number;
    }
    return out;
}

/// Decode options (and any payload) from the bytes following a CoAP token.
inline auto decode_options(std::span<const std::byte> bytes, std::vector<coap_option>& options,
                           std::vector<std::byte>& payload, bool& has_payload) -> void {
    std::size_t offset = 0;
    std::uint16_t number = 0;
    while (offset < bytes.size()) {
        const auto first = static_cast<std::uint8_t>(bytes[offset]);
        if (first == 0xFF) {
            has_payload = true;
            payload.assign(bytes.begin() + static_cast<std::ptrdiff_t>(offset) + 1, bytes.end());
            return;
        }
        ++offset;
        auto delta = static_cast<std::uint16_t>((first >> 4U) & 0x0FU);
        auto length = static_cast<std::size_t>(first & 0x0FU);

        const auto read_extended = [&](auto& field, std::uint16_t nibble) {
            using field_type = std::remove_reference_t<decltype(field)>;
            if (nibble == 13) {
                if (offset + 1 > bytes.size()) {
                    throw verification_error("truncated CoAP option (extended by one byte)");
                }
                field = static_cast<field_type>(static_cast<std::uint8_t>(bytes[offset]) + 13);
                offset += 1;
            } else if (nibble == 14) {
                if (offset + 2 > bytes.size()) {
                    throw verification_error("truncated CoAP option (extended by two bytes)");
                }
                const auto hi = static_cast<std::uint16_t>(bytes[offset]);
                const auto lo = static_cast<std::uint16_t>(bytes[offset + 1]);
                field = static_cast<field_type>(((hi << 8U) | lo) + 269);
                offset += 2;
            } else if (nibble == 15) {
                throw verification_error("reserved nibble 15 in a CoAP option header");
            }
        };
        read_extended(delta, delta);
        read_extended(length, static_cast<std::uint16_t>(length));

        if (offset + length > bytes.size()) {
            throw verification_error("truncated CoAP option value");
        }
        number = static_cast<std::uint16_t>(number + delta);
        coap_option option;
        option.number = number;
        option.value.assign(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                            bytes.begin() + static_cast<std::ptrdiff_t>(offset + length));
        options.push_back(std::move(option));
        offset += length;
    }
}

[[nodiscard]] inline auto parse_message(std::span<const std::byte> bytes) -> coap_message {
    if (bytes.size() < 4) {
        throw verification_error("CoAP message shorter than its 4-byte header");
    }
    coap_message message;
    const auto first = static_cast<std::uint8_t>(bytes[0]);
    message.version = static_cast<std::uint8_t>((first >> 6U) & 0x03U);
    message.type = static_cast<std::uint8_t>((first >> 4U) & 0x03U);
    const auto token_length = static_cast<std::size_t>(first & 0x0FU);
    if (token_length > 8) {
        throw verification_error("CoAP token longer than 8 bytes");
    }
    message.code = static_cast<std::uint8_t>(bytes[1]);
    message.message_id = static_cast<std::uint16_t>((static_cast<std::uint16_t>(bytes[2]) << 8U) |
                                                    static_cast<std::uint16_t>(bytes[3]));
    if (bytes.size() < 4 + token_length) {
        throw verification_error("truncated CoAP token");
    }
    message.token.assign(bytes.begin() + 4,
                         bytes.begin() + 4 + static_cast<std::ptrdiff_t>(token_length));
    decode_options(bytes.subspan(4 + token_length), message.options, message.payload,
                   message.has_payload);
    return message;
}

[[nodiscard]] inline auto serialize_message(const coap_message& message) -> std::vector<std::byte> {
    if (message.token.size() > 8) {
        throw unsupported_feature_error("CoAP token longer than 8 bytes");
    }
    std::vector<std::byte> out;
    out.push_back(static_cast<std::byte>(((message.version & 0x03U) << 6U) |
                                         ((message.type & 0x03U) << 4U) |
                                         static_cast<std::uint8_t>(message.token.size())));
    out.push_back(static_cast<std::byte>(message.code));
    out.push_back(static_cast<std::byte>((message.message_id >> 8U) & 0xFFU));
    out.push_back(static_cast<std::byte>(message.message_id & 0xFFU));
    out.insert(out.end(), message.token.begin(), message.token.end());

    const auto encoded = encode_options(message.options);
    out.insert(out.end(), encoded.begin(), encoded.end());
    if (message.has_payload) {
        out.push_back(std::byte{0xFF});
        out.insert(out.end(), message.payload.begin(), message.payload.end());
    }
    return out;
}

// ── Block options (RFC 7959), as carried inside OSCORE ─────────────────────
// Only the *inner* form is produced here: the blocks are formed before
// protection, so each is individually authenticated and end-to-end
// (RFC 8613 Section 4.1.3.4.1).

struct block_option {
    std::uint32_t number{0};  //!< block sequence number (NUM)
    bool more{false};         //!< M bit: another block follows
    std::uint8_t szx{6};      //!< size exponent; block size is 2^(szx+4), max 1024

    [[nodiscard]] auto size() const -> std::size_t {
        return std::size_t{1} << (static_cast<std::size_t>(szx) + 4);
    }
};

[[nodiscard]] inline auto encode_block_option(const block_option& block) -> std::vector<std::byte> {
    if (block.szx > 6) {
        throw unsupported_feature_error("block szx above 6 (1024 bytes) is not defined");
    }
    const std::uint32_t value =
        (block.number << 4U) | (block.more ? 0x08U : 0x00U) | (block.szx & 0x07U);
    // CoAP unsigned option values are minimum-length big-endian.
    std::vector<std::byte> out;
    for (int shift = 16; shift >= 0; shift -= 8) {
        const auto byte = static_cast<std::uint8_t>((value >> shift) & 0xFFU);
        if (!out.empty() || byte != 0 || shift == 0) {
            out.push_back(static_cast<std::byte>(byte));
        }
    }
    return out;
}

[[nodiscard]] inline auto decode_block_option(std::span<const std::byte> value) -> block_option {
    if (value.size() > 3) {
        throw verification_error("block option value longer than three bytes");
    }
    std::uint32_t raw = 0;
    for (const auto b : value) {
        raw = (raw << 8U) | static_cast<std::uint32_t>(b);
    }
    block_option block;
    block.szx = static_cast<std::uint8_t>(raw & 0x07U);
    block.more = (raw & 0x08U) != 0;
    block.number = raw >> 4U;
    if (block.szx == 7) {
        throw verification_error("reserved block szx value 7");
    }
    return block;
}

/// Find a Block1 (27) or Block2 (23) option in a message, if present.
[[nodiscard]] inline auto find_block_option(const coap_message& message, std::uint16_t number)
    -> std::optional<block_option> {
    for (const auto& option : message.options) {
        if (option.number == number) {
            return decode_block_option(option.value);
        }
    }
    return std::nullopt;
}

// ── Security context and the protect / verify procedures (Section 8) ───────

/// Everything a response needs from the request it answers. RFC 8613 binds the
/// two: the response's AAD names the *request's* kid and Partial IV, and a
/// response without its own Partial IV reuses the request's nonce
/// (Sections 5.2, 5.4, 8.3). Callers hold one of these between sending a
/// request and verifying its response.
struct request_binding {
    std::vector<std::byte> kid;
    std::vector<std::byte> partial_iv;
    std::vector<std::byte> nonce;
};

namespace detail {

/// Partial IVs are minimum-length big-endian, but a sequence number of 0 still
/// occupies one byte (RFC 8613 Appendix C.1 derives a nonce from "Partial IV
/// equal to 0", which is the single byte 0x00).
[[nodiscard]] inline auto encode_partial_iv(std::uint64_t sequence) -> std::vector<std::byte> {
    std::vector<std::byte> out;
    for (int shift = 32; shift >= 0; shift -= 8) {
        const auto byte = static_cast<std::uint8_t>((sequence >> shift) & 0xFFU);
        if (!out.empty() || byte != 0 || shift == 0) {
            out.push_back(static_cast<std::byte>(byte));
        }
    }
    return out;
}

[[nodiscard]] inline auto decode_partial_iv(std::span<const std::byte> bytes) -> std::uint64_t {
    std::uint64_t value = 0;
    for (const auto b : bytes) {
        value = (value << 8U) | static_cast<std::uint64_t>(b);
    }
    return value;
}

struct split_options {
    std::vector<coap_option> inner;
    std::vector<coap_option> outer;
};

[[nodiscard]] inline auto split_by_class(std::span<const coap_option> options) -> split_options {
    split_options split;
    for (const auto& option : options) {
        if (option.number == coap_option_oscore) {
            throw unsupported_feature_error("the message already carries an OSCORE option");
        }
        (is_outer_option(option.number) ? split.outer : split.inner).push_back(option);
    }
    return split;
}

/// RFC 8613 Section 5.3: `code || Class E options (delta-coded from 0) ||
/// 0xFF || payload`.
[[nodiscard]] inline auto build_plaintext(const coap_message& message,
                                          std::span<const coap_option> inner)
    -> std::vector<std::byte> {
    std::vector<std::byte> plaintext;
    plaintext.push_back(static_cast<std::byte>(message.code));
    const auto encoded = encode_options(inner, 0);
    plaintext.insert(plaintext.end(), encoded.begin(), encoded.end());
    if (message.has_payload) {
        plaintext.push_back(std::byte{0xFF});
        plaintext.insert(plaintext.end(), message.payload.begin(), message.payload.end());
    }
    return plaintext;
}

}  // namespace detail

/// One OSCORE Security Context (RFC 8613 Section 3): the Common Context shared
/// with the peer, plus this endpoint's Sender and Recipient Contexts.
///
/// Thread-safe. The transports drive this from an event-loop thread while
/// application threads may be submitting requests, and the sender sequence
/// number must never be issued twice -- reusing a Partial IV under one key is
/// the one catastrophic failure mode of an AEAD like AES-CCM.
class security_context {
public:
    explicit security_context(const oscore_credentials& credentials) {
        if (credentials.aead_algorithm != "AES-CCM-16-64-128") {
            throw coap_security_config_error(
                "OSCORE: only AES-CCM-16-64-128 is implemented, not '" +
                credentials.aead_algorithm + "'");
        }
        if (credentials.master_secret.empty()) {
            throw coap_security_config_error("OSCORE: a Master Secret is required");
        }
        if (credentials.sender_id == credentials.recipient_id) {
            // RFC 8613 Section 3.3: the two endpoints' Sender IDs must differ,
            // or both derive the same key and the nonce uniqueness argument
            // collapses.
            throw coap_security_config_error(
                "OSCORE: sender_id and recipient_id must differ within a Security Context");
        }
        _sender_id = credentials.sender_id;
        _recipient_id = credentials.recipient_id;
        _sender_key = hkdf_sha256(credentials.master_salt, credentials.master_secret,
                                  build_info(_sender_id, _id_context, _alg, "Key", aead_key_length),
                                  aead_key_length);
        _recipient_key = hkdf_sha256(
            credentials.master_salt, credentials.master_secret,
            build_info(_recipient_id, _id_context, _alg, "Key", aead_key_length), aead_key_length);
        _common_iv = hkdf_sha256(credentials.master_salt, credentials.master_secret,
                                 build_info({}, _id_context, _alg, "IV", aead_nonce_length),
                                 aead_nonce_length);
    }

    // ── Client side ────────────────────────────────────────────────────────

    /// RFC 8613 Section 8.1. Fills `binding` with what verifying the eventual
    /// response will need.
    [[nodiscard]] auto protect_request(const coap_message& message, request_binding& binding) const
        -> coap_message {
        if (!message.is_request()) {
            throw unsupported_feature_error("protect_request() called with a response");
        }
        const auto split = detail::split_by_class(message.options);
        const auto plaintext = detail::build_plaintext(message, split.inner);

        const auto partial_iv = detail::encode_partial_iv(next_sequence());
        const auto nonce = compute_nonce(_common_iv, _sender_id, partial_iv);
        const auto aad = build_aad(_alg, _sender_id, partial_iv, {});
        const auto ciphertext = aead_encrypt(_sender_key, nonce, aad, plaintext);

        option_fields fields;
        fields.partial_iv = partial_iv;
        fields.kid = _sender_id;
        fields.has_kid = true;

        coap_message out;
        out.version = message.version;
        out.type = message.type;
        // Section 8.1: the outer code of a protected request is POST.
        out.code = 0x02;
        out.message_id = message.message_id;
        out.token = message.token;
        out.options = split.outer;
        out.options.push_back(coap_option{coap_option_oscore, encode_option(fields)});
        std::stable_sort(
            out.options.begin(), out.options.end(),
            [](const coap_option& a, const coap_option& b) { return a.number < b.number; });
        out.payload = ciphertext;
        out.has_payload = true;

        binding.kid = _sender_id;
        binding.partial_iv = partial_iv;
        binding.nonce = nonce;
        return out;
    }

    /// RFC 8613 Section 8.4.
    [[nodiscard]] auto unprotect_response(const coap_message& message,
                                          const request_binding& binding) const -> coap_message {
        const auto fields = extract_option(message);
        // The AAD names the request's kid and Partial IV, which is what binds
        // this response to the request the caller actually sent.
        const auto aad = build_aad(_alg, binding.kid, binding.partial_iv, {});
        // Section 5.2: a response carrying its own Partial IV gets its own
        // nonce, built from the *server's* Sender ID -- our Recipient ID.
        const auto nonce = fields.partial_iv.empty()
                               ? binding.nonce
                               : compute_nonce(_common_iv, _recipient_id, fields.partial_iv);
        const auto plaintext = aead_decrypt(_recipient_key, nonce, aad, message.payload);
        return rebuild(message, plaintext);
    }

    /// As `unprotect_response`, plus the notification ordering check of
    /// RFC 8613 Section 8.4.2.
    ///
    /// Observe turns one request into a stream of responses, so a valid AEAD
    /// tag is no longer enough: an attacker can replay an *old* notification,
    /// which authenticates perfectly and rolls the observer's view backwards.
    /// The Partial IV is what orders them, so each notification's must exceed
    /// the last accepted one for this observation.
    ///
    /// `last_partial_iv` is empty until the first notification is accepted, and
    /// deliberately not a plain zero: Partial IV 0 is a legal value (RFC 8613
    /// Appendix C.1 derives a nonce from exactly that), so a sentinel of 0
    /// cannot be told apart from a server whose Sender Sequence Number is still
    /// at its initial value. Encoding "nothing accepted yet" as a zero would
    /// reject the first notification of every fresh observation as a replay.
    [[nodiscard]] auto unprotect_notification(const coap_message& message,
                                              const request_binding& binding,
                                              std::optional<std::uint64_t>& last_partial_iv)
        -> coap_message {
        const auto fields = extract_option(message);
        if (fields.partial_iv.empty()) {
            throw verification_error(
                "an Observe notification must carry a Partial IV (RFC 8613 Section 8.4.2)");
        }
        const auto sequence = detail::decode_partial_iv(fields.partial_iv);
        // Verify first, then order: an unauthenticated message must never be
        // able to move the window.
        auto plaintext_message = unprotect_response(message, binding);
        if (last_partial_iv.has_value() && sequence <= *last_partial_iv) {
            throw verification_error("replayed or reordered Observe notification");
        }
        last_partial_iv = sequence;
        return plaintext_message;
    }

    // ── Server side ────────────────────────────────────────────────────────

    /// RFC 8613 Section 8.2, including the replay check of Section 7.4.
    [[nodiscard]] auto unprotect_request(const coap_message& message, request_binding& binding)
        -> coap_message {
        const auto fields = extract_option(message);
        if (fields.partial_iv.empty()) {
            throw verification_error("a protected request must carry a Partial IV");
        }
        if (!fields.has_kid) {
            throw verification_error("a protected request must carry a kid");
        }
        if (fields.kid != _recipient_id) {
            throw verification_error("no Recipient Context matches the request's kid");
        }
        check_and_record_replay(detail::decode_partial_iv(fields.partial_iv));

        const auto aad = build_aad(_alg, fields.kid, fields.partial_iv, {});
        const auto nonce = compute_nonce(_common_iv, fields.kid, fields.partial_iv);
        const auto plaintext = aead_decrypt(_recipient_key, nonce, aad, message.payload);

        binding.kid = fields.kid;
        binding.partial_iv = fields.partial_iv;
        binding.nonce = nonce;
        return rebuild(message, plaintext);
    }

    /// RFC 8613 Section 8.3.
    ///
    /// `with_own_partial_iv` selects between the two legal nonce choices for a
    /// response. False reuses the request's nonce, which is the recommended
    /// behaviour for a single response and is what Appendix C.7 does. True
    /// consumes a fresh Sender Sequence Number and carries it in the OSCORE
    /// option, which is *required* for Observe notifications -- there are many
    /// responses to one request, so reusing the request's nonce would reuse a
    /// nonce under one key -- and is what Appendix C.8 does. It is also the
    /// answer when the server cannot do replay protection (Appendix B.1.2).
    [[nodiscard]] auto protect_response(const coap_message& message, const request_binding& binding,
                                        bool with_own_partial_iv = false) const -> coap_message {
        if (message.is_request()) {
            throw unsupported_feature_error("protect_response() called with a request");
        }
        const auto split = detail::split_by_class(message.options);
        const auto plaintext = detail::build_plaintext(message, split.inner);

        std::vector<std::byte> own_partial_iv;
        std::vector<std::byte> nonce = binding.nonce;
        if (with_own_partial_iv) {
            own_partial_iv = detail::encode_partial_iv(next_sequence());
            // Built from *this* endpoint's Sender ID, not the request's kid:
            // the nonce must be unique per (key, sender), and the responder is
            // the sender here.
            nonce = compute_nonce(_common_iv, _sender_id, own_partial_iv);
        }

        // The AAD still names the *request's* kid and Partial IV either way --
        // that is what binds the notification to the observation it answers.
        const auto aad = build_aad(_alg, binding.kid, binding.partial_iv, {});
        const auto ciphertext = aead_encrypt(_sender_key, nonce, aad, plaintext);

        coap_message out;
        out.version = message.version;
        out.type = message.type;
        // Section 8.3: the outer code of a protected response is 2.04 Changed.
        out.code = 0x44;
        out.message_id = message.message_id;
        out.token = message.token;
        out.options = split.outer;
        // A response never carries a kid. Without its own Partial IV the option
        // value is empty (Appendix C.7); with one it is flags || PIV
        // (Appendix C.8).
        option_fields fields;
        fields.partial_iv = own_partial_iv;
        out.options.push_back(coap_option{coap_option_oscore, encode_option(fields)});
        std::stable_sort(
            out.options.begin(), out.options.end(),
            [](const coap_option& a, const coap_option& b) { return a.number < b.number; });
        out.payload = ciphertext;
        out.has_payload = true;
        return out;
    }

    // ── Accessors, for tests and diagnostics ───────────────────────────────
    [[nodiscard]] auto sender_key() const -> const std::vector<std::byte>& { return _sender_key; }
    [[nodiscard]] auto recipient_key() const -> const std::vector<std::byte>& {
        return _recipient_key;
    }
    [[nodiscard]] auto common_iv() const -> const std::vector<std::byte>& { return _common_iv; }

    /// Forces the next Partial IV, so a test can reproduce a published vector.
    /// Not for production use: rewinding a sequence number reuses a nonce.
    auto set_sender_sequence_for_testing(std::uint64_t sequence) -> void {
        const std::lock_guard lock(_mutex);
        _sender_sequence = sequence;
    }

private:
    [[nodiscard]] auto next_sequence() const -> std::uint64_t {
        const std::lock_guard lock(_mutex);
        if (_sender_sequence > 0xFFFFFFFFFFULL) {
            // RFC 8613 Section 7.2.1: when the sequence space is exhausted the
            // context MUST NOT be used again. Refusing beats wrapping.
            throw coap_security_error(
                "OSCORE: sender sequence number space exhausted; a new Security Context is "
                "required (RFC 8613 Section 7.2.1)");
        }
        return _sender_sequence++;
    }

    /// RFC 8613 Section 7.4 / Appendix B.1.2: a sliding replay window over the
    /// highest Partial IV seen.
    auto check_and_record_replay(std::uint64_t sequence) -> void {
        const std::lock_guard lock(_mutex);
        if (!_seen_any) {
            _seen_any = true;
            _replay_high = sequence;
            _replay_bitmap = 1;
            return;
        }
        if (sequence > _replay_high) {
            const auto shift = sequence - _replay_high;
            _replay_bitmap = shift >= 64 ? 0 : (_replay_bitmap << shift);
            _replay_bitmap |= 1;
            _replay_high = sequence;
            return;
        }
        const auto age = _replay_high - sequence;
        if (age >= 64) {
            throw verification_error("Partial IV is older than the replay window");
        }
        const std::uint64_t bit = 1ULL << age;
        if ((_replay_bitmap & bit) != 0) {
            throw verification_error("replayed Partial IV");
        }
        _replay_bitmap |= bit;
    }

    [[nodiscard]] static auto extract_option(const coap_message& message) -> option_fields {
        for (const auto& option : message.options) {
            if (option.number == coap_option_oscore) {
                if (!message.has_payload) {
                    throw verification_error("OSCORE message carries no ciphertext");
                }
                return decode_option(option.value);
            }
        }
        throw verification_error("message carries no OSCORE option");
    }

    /// Reassemble the original message: code and Class E options come from the
    /// decrypted plaintext, the outer options minus the OSCORE one are kept.
    [[nodiscard]] static auto rebuild(const coap_message& outer,
                                      std::span<const std::byte> plaintext) -> coap_message {
        if (plaintext.empty()) {
            throw verification_error("decrypted plaintext is empty");
        }
        coap_message out;
        out.version = outer.version;
        out.type = outer.type;
        out.message_id = outer.message_id;
        out.token = outer.token;
        out.code = static_cast<std::uint8_t>(plaintext[0]);

        std::vector<coap_option> inner;
        decode_options(plaintext.subspan(1), inner, out.payload, out.has_payload);
        for (const auto& option : inner) {
            if (is_outer_option(option.number) && option.number != coap_option_oscore) {
                // RFC 8613 Section 8.2 item 6: a Class U option smuggled into
                // the ciphertext is a protocol violation, not something to
                // merge in quietly.
                throw verification_error("Class U option " + std::to_string(option.number) +
                                         " found inside the ciphertext");
            }
        }
        out.options = std::move(inner);
        for (const auto& option : outer.options) {
            if (option.number != coap_option_oscore) {
                out.options.push_back(option);
            }
        }
        std::stable_sort(
            out.options.begin(), out.options.end(),
            [](const coap_option& a, const coap_option& b) { return a.number < b.number; });
        return out;
    }

    std::vector<std::byte> _sender_id;
    std::vector<std::byte> _recipient_id;
    std::vector<std::byte> _id_context;
    std::vector<std::byte> _sender_key;
    std::vector<std::byte> _recipient_key;
    std::vector<std::byte> _common_iv;
    int _alg{aead_alg_aes_ccm_16_64_128};

    mutable std::mutex _mutex;
    mutable std::uint64_t _sender_sequence{0};
    std::uint64_t _replay_high{0};
    std::uint64_t _replay_bitmap{0};
    bool _seen_any{false};
};

}  // namespace kythira::oscore
