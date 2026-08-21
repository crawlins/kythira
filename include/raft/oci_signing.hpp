// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file oci_signing.hpp
/// @brief OCI Request Signing Version 1 — the HTTP Signatures profile every
///        OCI REST call must carry.
///
/// Hand-rolled rather than taken from an SDK, which is the whole reason this
/// feature adds nothing to `vcpkg.json`: the scheme is a canonical string, an
/// RSA-SHA256 signature over it, and one header. OpenSSL is already a hard
/// dependency, so there is nothing left to depend on.
///
/// **The canonical form is confirmed, not inferred** — read out of
/// `oracle/oci-go-sdk`'s `common/http_signer.go` during this spec's Task 0
/// spike (`.kiro/specs/oci-cloud-provider/spike-notes.md`, Finding 1). It
/// matters because the scheme is a *signature over an ordered string*: a
/// plausible-looking header order produces a well-formed request that the
/// service rejects with a 401 giving no hint which of the two sides is wrong.
/// `design.md`'s first draft had `(request-target)` first; the real order puts
/// `date` first, and that correction is carried here in code rather than only
/// in prose.
///
/// The signed set is fixed and depends only on whether there is a body:
///
///     no body   date, (request-target), host
///     with body date, (request-target), host, content-length, content-type,
///               x-content-sha256
///
/// each line formatted `"{lowercased-name}: {value}"` and joined with `"\n"`
/// (no trailing newline).

#include <raft/oci_client_config.hpp>

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/sha.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace kythira::oci_signing {

namespace detail {

template<typename T, void (*Deleter)(T*)> struct openssl_deleter {
    void operator()(T* p) const noexcept {
        if (p != nullptr) {
            Deleter(p);
        }
    }
};
using evp_pkey_ptr = std::unique_ptr<EVP_PKEY, openssl_deleter<EVP_PKEY, EVP_PKEY_free>>;
using bio_ptr = std::unique_ptr<BIO, openssl_deleter<BIO, BIO_free_all>>;

[[noreturn]] inline void throw_openssl_error(const std::string& context) {
    std::ostringstream out;
    unsigned long code = 0;  // NOLINT(google-runtime-int) — matches OpenSSL's own typedef
    bool first = true;
    while ((code = ERR_get_error()) != 0) {
        std::array<char, 256> buf{};
        ERR_error_string_n(code, buf.data(), buf.size());
        if (!first) {
            out << "; ";
        }
        out << buf.data();
        first = false;
    }
    auto details = out.str();
    throw std::runtime_error(details.empty() ? context : context + ": " + details);
}

/// Standard base64 with padding (RFC 4648 §4), which is what this scheme wants
/// in both the `x-content-sha256` header and the signature — *not* the
/// base64url `acme_jws` uses. Two encodings that differ in three characters is
/// exactly the kind of near-miss worth spelling out at the call site.
[[nodiscard]] inline auto base64_encode(const unsigned char* data, std::size_t len) -> std::string {
    // EVP_EncodeBlock writes 4 bytes per 3 input bytes, plus a NUL.
    std::string out(4 * ((len + 2) / 3) + 1, '\0');
    const int written =
        EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data()), data, static_cast<int>(len));
    if (written < 0) {
        throw_openssl_error("oci_signing: EVP_EncodeBlock failed");
    }
    out.resize(static_cast<std::size_t>(written));
    return out;
}

[[nodiscard]] inline auto sha256_base64(std::string_view body) -> std::string {
    std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
    SHA256(reinterpret_cast<const unsigned char*>(body.data()), body.size(), digest.data());
    return base64_encode(digest.data(), digest.size());
}

[[nodiscard]] inline auto to_lower(std::string_view in) -> std::string {
    std::string out;
    out.reserve(in.size());
    for (const char c : in) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

/// An RFC 1123 date in GMT, which is what the `date` header must carry.
///
/// Formatted explicitly from the broken-down time rather than via `%a`/`%b`,
/// because those two conversions are locale-sensitive: under a non-C locale
/// `std::strftime` will happily emit a localised day or month name, producing a
/// header the service rejects — on a machine whose only difference from a
/// working one is `$LC_TIME`.
[[nodiscard]] inline auto rfc1123_now(std::time_t when) -> std::string {
    std::tm tm_utc{};
#if defined(_WIN32)
    gmtime_s(&tm_utc, &when);
#else
    gmtime_r(&when, &tm_utc);
#endif
    static constexpr std::array<const char*, 7> days{"Sun", "Mon", "Tue", "Wed",
                                                     "Thu", "Fri", "Sat"};
    static constexpr std::array<const char*, 12> months{"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                                        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    std::array<char, 64> buf{};
    const int written =
        std::snprintf(buf.data(), buf.size(), "%s, %02d %s %04d %02d:%02d:%02d GMT",
                      days.at(static_cast<std::size_t>(tm_utc.tm_wday)), tm_utc.tm_mday,
                      months.at(static_cast<std::size_t>(tm_utc.tm_mon)), tm_utc.tm_year + 1900,
                      tm_utc.tm_hour, tm_utc.tm_min, tm_utc.tm_sec);
    if (written <= 0) {
        throw std::runtime_error("oci_signing: failed to format the date header");
    }
    return std::string(buf.data(), static_cast<std::size_t>(written));
}

/// Load a PEM private key, honouring an optional passphrase.
///
/// The passphrase is passed through OpenSSL's callback rather than its `void*`
/// userdata shortcut, because the shortcut requires a NUL-terminated,
/// non-const buffer and silently treats an empty string as "prompt on the
/// terminal" — which in a server process is a hang, not an error.
[[nodiscard]] inline auto load_private_key(const std::string& pem, const std::string& passphrase)
    -> evp_pkey_ptr {
    bio_ptr bio{BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()))};
    if (!bio) {
        throw_openssl_error("oci_signing: BIO_new_mem_buf failed");
    }
    auto callback = [](char* buf, int size, int /*rwflag*/, void* userdata) -> int {
        const auto* pass = static_cast<const std::string*>(userdata);
        if (pass == nullptr || pass->empty()) {
            return 0;
        }
        const int len =
            static_cast<int>(std::min<std::size_t>(pass->size(), static_cast<std::size_t>(size)));
        std::memcpy(buf, pass->data(), static_cast<std::size_t>(len));
        return len;
    };
    evp_pkey_ptr key{PEM_read_bio_PrivateKey(bio.get(), nullptr, callback,
                                             const_cast<std::string*>(&passphrase))};
    if (!key) {
        throw_openssl_error("oci_signing: could not read the configured private key");
    }
    return key;
}

[[nodiscard]] inline auto rsa_sha256_sign(EVP_PKEY* key, std::string_view signing_string)
    -> std::string {
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (ctx == nullptr) {
        throw_openssl_error("oci_signing: EVP_MD_CTX_new failed");
    }
    struct ctx_guard {
        EVP_MD_CTX* c;
        ~ctx_guard() { EVP_MD_CTX_free(c); }
    } guard{ctx};

    if (EVP_DigestSignInit(ctx, nullptr, EVP_sha256(), nullptr, key) != 1) {
        throw_openssl_error("oci_signing: EVP_DigestSignInit failed");
    }
    std::size_t len = 0;
    if (EVP_DigestSign(ctx, nullptr, &len,
                       reinterpret_cast<const unsigned char*>(signing_string.data()),
                       signing_string.size()) != 1) {
        throw_openssl_error("oci_signing: EVP_DigestSign (sizing) failed");
    }
    std::vector<unsigned char> sig(len);
    if (EVP_DigestSign(ctx, sig.data(), &len,
                       reinterpret_cast<const unsigned char*>(signing_string.data()),
                       signing_string.size()) != 1) {
        throw_openssl_error("oci_signing: EVP_DigestSign failed");
    }
    sig.resize(len);
    return base64_encode(sig.data(), sig.size());
}

}  // namespace detail

/// @brief The ordered header names signed for a request, per body presence.
///
/// Exposed because it is the one thing a verifier — a mock server, a test —
/// must agree with the signer about exactly. Two independently written copies
/// of this list is how a signing bug becomes a test that passes against itself.
[[nodiscard]] inline auto signed_header_names(bool has_body) -> std::vector<std::string> {
    if (has_body) {
        return {"date",           "(request-target)", "host",
                "content-length", "content-type",     "x-content-sha256"};
    }
    return {"date", "(request-target)", "host"};
}

/// @brief Build the canonical signing string (Requirement 1.4).
///
/// Split out from @ref sign_request so a test can assert the exact bytes that
/// get signed. A test that could only check the resulting base64 signature
/// would be asserting the output of a hash — it would catch a broken signature
/// but tell you nothing about *which* part of the canonical form was wrong,
/// which is the only question worth asking when a service answers 401.
[[nodiscard]] inline auto build_signing_string(std::string_view method,
                                               std::string_view request_target,
                                               std::string_view host, const std::string& body,
                                               const std::string& date_header,
                                               std::string_view content_type = "application/json")
    -> std::string {
    const bool has_body = !body.empty();
    std::ostringstream out;
    out << "date: " << date_header << "\n";
    out << "(request-target): " << detail::to_lower(method) << " " << request_target << "\n";
    out << "host: " << host;
    if (has_body) {
        out << "\ncontent-length: " << body.size();
        // The content type is **signed**, so it is a parameter rather than the
        // constant it was until task 10 of the cloud-object-persistence spec.
        // Every caller before that was a control-plane API whose bodies are
        // JSON; Object Storage is a data plane with opaque bodies, and a client
        // that sent `application/octet-stream` while signing `application/json`
        // would get `NotAuthenticated: Failed to verify the HTTP(S) Signature`
        // — a failure naming neither side of the mismatch. The default keeps
        // every existing caller byte-identical.
        out << "\ncontent-type: " << content_type;
        out << "\nx-content-sha256: " << detail::sha256_base64(body);
    }
    return out.str();
}

/// @brief Sign a request with an explicit key and `keyId` — the shared core
///        of the API-key path and the Instance Principal session-token path.
///
/// The two auth modes differ *only* here: which private key signs and what
/// the `keyId` says. API-key signing passes
/// `{tenancy}/{user}/{fingerprint}`; Instance Principal passes
/// `"ST$" + token` with the ephemeral session key
/// (`oci_federation::instance_principal_signer` owns that flow). The
/// canonical string, the signed set and the header shapes are identical —
/// confirmed by Task 0(b), and the reason this is one function rather than
/// two near-copies that could drift.
[[nodiscard]] inline auto sign_request_with_key(
    std::string_view key_id, const std::string& private_key_pem,
    const std::string& private_key_passphrase, std::string_view method,
    std::string_view request_target, std::string_view host, const std::string& body,
    std::time_t when = std::time(nullptr), std::string_view content_type = "application/json")
    -> std::map<std::string, std::string> {
    const bool has_body = !body.empty();
    const std::string date_header = detail::rfc1123_now(when);
    const std::string signing_string =
        build_signing_string(method, request_target, host, body, date_header, content_type);

    auto key = detail::load_private_key(private_key_pem, private_key_passphrase);
    const std::string signature = detail::rsa_sha256_sign(key.get(), signing_string);

    std::ostringstream headers_list;
    bool first = true;
    for (const auto& name : signed_header_names(has_body)) {
        if (!first) {
            headers_list << " ";
        }
        headers_list << name;
        first = false;
    }

    std::ostringstream authorization;
    authorization << "Signature version=\"1\","
                  << "headers=\"" << headers_list.str() << "\","
                  << "keyId=\"" << key_id << "\","
                  << "algorithm=\"rsa-sha256\","
                  << "signature=\"" << signature << "\"";

    std::map<std::string, std::string> out;
    out["date"] = date_header;
    out["authorization"] = authorization.str();
    if (has_body) {
        out["content-length"] = std::to_string(body.size());
        // Must be the same string the signing form above used — signing one
        // content type and sending another is the whole failure this parameter
        // exists to prevent.
        out["content-type"] = std::string(content_type);
        out["x-content-sha256"] = detail::sha256_base64(body);
    }
    return out;
}

/// @brief Sign a request with the config's credentials — API key, or a
///        caller-supplied federated security token — returning every header
///        the caller must add.
///
/// @param cfg            Auth material. Validated here, not in the config type.
///                       A non-empty `security_token` selects UPST signing
///                       (`keyId = "ST$" + token`, `private_key_pem` holding
///                       the session key); otherwise the four API-key fields
///                       apply.
/// @param method         HTTP method; case-insensitive, lowercased for the
///                       `(request-target)` pseudo-header.
/// @param request_target Path *and query* exactly as it will go on the wire.
///                       Signed verbatim: this is a signature over the bytes,
///                       so a caller that signs a decoded path and sends an
///                       encoded one gets a 401.
/// @param host           The `Host` header value the request will carry.
/// @param body           Request body; empty selects the three-header form.
/// @param when           Timestamp for the `date` header, injectable so a test
///                       can pin a signature. Defaults to now.
///
/// @throws std::invalid_argument when @p cfg lacks a field the selected auth
///         mode requires (Requirement 1.5).
/// @throws std::runtime_error when `use_instance_principal` is set — that
///         mode is stateful (a cached federation token) and cannot live
///         behind a pure function; `oci_http_client` routes it to
///         `oci_federation::instance_principal_signer` instead of here.
[[nodiscard]] inline auto sign_request(const oci_client_config& cfg, std::string_view method,
                                       std::string_view request_target, std::string_view host,
                                       const std::string& body,
                                       std::time_t when = std::time(nullptr),
                                       std::string_view content_type = "application/json")
    -> std::map<std::string, std::string> {
    if (cfg.use_instance_principal) {
        throw std::runtime_error(
            "oci_signing: sign_request only implements API-key signing — Instance "
            "Principal auth is stateful and lives in oci_federation::"
            "instance_principal_signer (include/raft/oci_federation.hpp), which "
            "oci_http_client selects automatically when use_instance_principal is set");
    }

    // UPST mode: the caller already holds a federated token (CI's GitHub-OIDC
    // exchange, Requirement 14.2) bound to the session key in
    // private_key_pem. Same keyId scheme Instance Principal uses — "ST$" plus
    // the token — because it is the same kind of token; what differs is who
    // performed the exchange. Checked before the API-key validation so a
    // token-mode config is not forced to carry vestigial tenancy/user/
    // fingerprint values just to pass a gate that does not apply to it.
    if (!cfg.security_token.empty()) {
        if (cfg.private_key_pem.empty()) {
            throw std::invalid_argument(
                "oci_signing: private_key_pem must hold the session private key whose "
                "public half was submitted in the token exchange when security_token "
                "is set");
        }
        return sign_request_with_key("ST$" + cfg.security_token, cfg.private_key_pem,
                                     cfg.private_key_passphrase, method, request_target, host, body,
                                     when, content_type);
    }

    // Named individually rather than as "credentials are incomplete", because
    // the whole failure mode here is a config with one field left blank.
    const auto require = [](const std::string& value, const char* field) {
        if (value.empty()) {
            throw std::invalid_argument(std::string("oci_signing: ") + field +
                                        " is required when neither use_instance_principal "
                                        "nor security_token is set");
        }
    };
    require(cfg.tenancy_id, "tenancy_id");
    require(cfg.user_id, "user_id");
    require(cfg.fingerprint, "fingerprint");
    require(cfg.private_key_pem, "private_key_pem");

    return sign_request_with_key(cfg.tenancy_id + "/" + cfg.user_id + "/" + cfg.fingerprint,
                                 cfg.private_key_pem, cfg.private_key_passphrase, method,
                                 request_target, host, body, when, content_type);
}

}  // namespace kythira::oci_signing
