#pragma once

/// @file oci_federation.hpp
/// @brief Instance Principal authentication — the X.509 federation client
///        Requirement 1.6 asks for (`spike-notes.md` Finding 16).
///
/// Instance Principal is **not** the API-key path with different credentials.
/// Requests are never signed with the instance certificate's key; the real
/// flow, read out of `oracle/oci-go-sdk`'s `common/auth/` (the same source
/// Finding 16 was derived from — every wire detail below is quoted from that
/// SDK, not inferred), is:
///
///   1. Read `/instance/region`, `/identity/cert.pem`, `/identity/key.pem`
///      and `/identity/intermediate.pem` from `http://169.254.169.254/opc/v2`
///      (overridable via `OCI_METADATA_BASE_URL`). Every metadata request
///      carries `Authorization: Bearer Oracle` — the v2 metadata service
///      rejects requests without it, a detail Finding 16 originally omitted.
///   2. Generate an ephemeral RSA-2048 **session key pair**.
///   3. `POST https://auth.{region}.oraclecloud.com/v1/x509` with
///      `{"certificate": <leaf, PEM armor stripped>, "publicKey": <base64
///      PKIX DER of the session public key>, "intermediateCertificates":
///      [...], "fingerprintAlgorithm": "SHA256"}`, signed with the **leaf**
///      key under `keyId = "{tenancy}/fed-x509-sha256/{fingerprint}"`, where
///      the tenancy OCID comes out of the leaf certificate's subject
///      (`opc-tenant:` prefix) and the fingerprint is the SHA-256 of the leaf
///      DER as lowercase colon-separated hex. The signed set for *this one
///      request* is `date (request-target) content-length content-type
///      x-content-sha256` — **no `host`**, unlike every ordinary OCI call.
///   4. Receive `{"token": "<JWT>"}`; sign ordinary API requests with the
///      **session** private key and `keyId = "ST$" + token`, renewing when
///      the token is within five minutes of its `exp` claim (the go-sdk's
///      buffer), regenerating the session key pair on every renewal.
///
/// Lives in its own header rather than in `oci_signing.hpp` because the two
/// have different dependency weights on purpose: signing is pure
/// OpenSSL-over-strings, while federation needs an HTTP client and JSON.
/// This header carries the same deps as `oci_http_client.hpp` and nothing
/// more.
///
/// **Verification status, honestly:** the wire contract here is confirmed
/// against a known-good implementation (the go-sdk source), and the mock
/// tier verifies both signatures cryptographically — but the flow has not
/// yet been exercised on a real OCI instance, which is the only place the
/// metadata service exists. The defect classes this project keeps finding
/// live-only (Finding 5-8's endpoint/Host/duplicate-header family) should
/// be covered by reusing `oci_signing`/`oci_http_client`'s corrected
/// primitives, but "should" is doing work in that sentence until a real
/// instance has run this.

#include <raft/oci_client_config.hpp>
#include <raft/oci_signing.hpp>

#include <boost/json.hpp>

#include <httplib.h>

#include <openssl/asn1.h>
#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <openssl/sha.h>
#include <openssl/x509.h>

#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace kythira::oci_federation {

namespace detail {

using kythira::oci_signing::detail::base64_encode;
using kythira::oci_signing::detail::bio_ptr;
using kythira::oci_signing::detail::throw_openssl_error;

using x509_ptr =
    std::unique_ptr<X509, kythira::oci_signing::detail::openssl_deleter<X509, X509_free>>;
using evp_pkey_ctx_ptr =
    std::unique_ptr<EVP_PKEY_CTX,
                    kythira::oci_signing::detail::openssl_deleter<EVP_PKEY_CTX, EVP_PKEY_CTX_free>>;

[[nodiscard]] inline auto parse_certificate(const std::string& pem) -> x509_ptr {
    bio_ptr bio{BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()))};
    if (!bio) {
        throw_openssl_error("oci_federation: BIO_new_mem_buf failed");
    }
    x509_ptr cert{PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr)};
    if (!cert) {
        throw_openssl_error("oci_federation: could not parse the instance certificate PEM");
    }
    return cert;
}

/// Decode one base64url segment (RFC 7515's alphabet, no padding) — what the
/// two textual JWT segments use. Deliberately separate from the standard
/// base64 in `oci_signing`: the alphabets differ in two characters and the
/// padding convention differs, and conflating the two is a classic
/// silently-wrong-on-some-inputs bug (a token whose payload happens to avoid
/// `-`/`_` decodes fine either way).
[[nodiscard]] inline auto base64url_decode(std::string_view in) -> std::string {
    std::string translated(in);
    for (auto& c : translated) {
        if (c == '-') {
            c = '+';
        } else if (c == '_') {
            c = '/';
        }
    }
    const std::size_t pad = (4 - translated.size() % 4) % 4;
    translated.append(pad, '=');

    std::string out(3 * (translated.size() / 4), '\0');
    const int written = EVP_DecodeBlock(reinterpret_cast<unsigned char*>(out.data()),
                                        reinterpret_cast<const unsigned char*>(translated.data()),
                                        static_cast<int>(translated.size()));
    if (written < 0) {
        throw std::runtime_error("oci_federation: token segment is not valid base64url");
    }
    // EVP_DecodeBlock always emits a multiple of three bytes, zero-filling
    // for padding; trim what the padding accounted for.
    out.resize(static_cast<std::size_t>(written) - pad);
    return out;
}

}  // namespace detail

/// @brief Strip PEM armor and newlines, leaving bare base64 — the form the
///        federation endpoint wants certificates and public keys in.
///
/// Mirrors the go-sdk's `sanitizeCertificateString` verbatim: it removes the
/// CERTIFICATE and PUBLIC KEY begin/end markers and every newline, nothing
/// else.
[[nodiscard]] inline auto sanitize_pem(std::string pem) -> std::string {
    static constexpr std::array<std::string_view, 4> markers{
        "-----BEGIN CERTIFICATE-----",
        "-----END CERTIFICATE-----",
        "-----BEGIN PUBLIC KEY-----",
        "-----END PUBLIC KEY-----",
    };
    for (const auto marker : markers) {
        for (std::size_t at = pem.find(marker); at != std::string::npos;
             at = pem.find(marker, at)) {
            pem.erase(at, marker.size());
        }
    }
    std::erase(pem, '\n');
    std::erase(pem, '\r');
    return pem;
}

/// @brief Split a PEM bundle into its individual `CERTIFICATE` blocks.
///
/// `/identity/intermediate.pem` is one file but not necessarily one
/// certificate, and the federation request wants an *array* — sanitizing a
/// concatenation would splice two DERs into one broken base64 string.
[[nodiscard]] inline auto split_pem_certificates(const std::string& bundle)
    -> std::vector<std::string> {
    static constexpr std::string_view begin_marker = "-----BEGIN CERTIFICATE-----";
    static constexpr std::string_view end_marker = "-----END CERTIFICATE-----";
    std::vector<std::string> out;
    std::size_t at = 0;
    while (true) {
        const auto begin = bundle.find(begin_marker, at);
        if (begin == std::string::npos) {
            break;
        }
        const auto end = bundle.find(end_marker, begin);
        if (end == std::string::npos) {
            break;
        }
        out.push_back(bundle.substr(begin, end + end_marker.size() - begin));
        at = end + end_marker.size();
    }
    return out;
}

/// @brief SHA-256 fingerprint of a certificate's DER bytes, formatted the way
///        the federation `keyId` wants it: lowercase hex, colon-separated.
///
/// The formatting is part of the contract, not a display choice — the go-sdk
/// builds it with `fmt.Sprintf("% x", ...)` plus a space→colon replace, which
/// yields lowercase. An uppercase or colon-less spelling produces a keyId the
/// Auth service rejects with a 401 that names nothing.
[[nodiscard]] inline auto certificate_fingerprint(const std::string& cert_pem) -> std::string {
    const auto cert = detail::parse_certificate(cert_pem);
    unsigned char* der = nullptr;
    const int len = i2d_X509(cert.get(), &der);
    if (len <= 0) {
        detail::throw_openssl_error("oci_federation: i2d_X509 failed");
    }
    struct der_guard {
        unsigned char* p;
        ~der_guard() { OPENSSL_free(p); }
    } guard{der};

    std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
    SHA256(der, static_cast<std::size_t>(len), digest.data());

    static constexpr char hex[] = "0123456789abcdef";
    std::string out;
    out.reserve(digest.size() * 3 - 1);
    for (std::size_t i = 0; i < digest.size(); ++i) {
        if (i != 0) {
            out.push_back(':');
        }
        out.push_back(hex[digest.at(i) >> 4]);
        out.push_back(hex[digest.at(i) & 0x0F]);
    }
    return out;
}

/// @brief The tenancy OCID carried inside an instance leaf certificate.
///
/// OCI encodes it as a subject name attribute whose value is prefixed
/// `opc-tenant:` — there is no config field to read it from on an instance,
/// the certificate *is* the source of truth (go-sdk
/// `extractTenancyIDFromCertificate`).
[[nodiscard]] inline auto extract_tenancy_ocid(const std::string& cert_pem) -> std::string {
    const auto cert = detail::parse_certificate(cert_pem);
    X509_NAME* subject = X509_get_subject_name(cert.get());
    static constexpr std::string_view prefix = "opc-tenant:";
    for (int i = 0; i < X509_NAME_entry_count(subject); ++i) {
        const X509_NAME_ENTRY* entry = X509_NAME_get_entry(subject, i);
        const ASN1_STRING* data = X509_NAME_ENTRY_get_data(entry);
        unsigned char* utf8 = nullptr;
        const int len = ASN1_STRING_to_UTF8(&utf8, data);
        if (len < 0) {
            continue;
        }
        std::string value(reinterpret_cast<char*>(utf8), static_cast<std::size_t>(len));
        OPENSSL_free(utf8);
        if (value.starts_with(prefix)) {
            return value.substr(prefix.size());
        }
    }
    throw std::runtime_error(
        "oci_federation: the instance certificate's subject carries no 'opc-tenant:' "
        "attribute — this does not look like an OCI instance identity certificate");
}

/// @brief Map what `/instance/region` returns onto a canonical region name.
///
/// The metadata service answers with the *short* code for OCI's four original
/// regions and the full name everywhere else; the go-sdk's `StringToRegion`
/// maps exactly these four and passes everything else through, and so does
/// this. Anything short and unknown is an error rather than a guess — a wrong
/// region silently derives a wrong auth host, which then fails DNS at best.
[[nodiscard]] inline auto resolve_region(const std::string& metadata_region) -> std::string {
    if (metadata_region.find('-') != std::string::npos) {
        return metadata_region;
    }
    static const std::map<std::string, std::string> legacy_short_codes{
        {"phx", "us-phoenix-1"},
        {"iad", "us-ashburn-1"},
        {"fra", "eu-frankfurt-1"},
        {"lhr", "uk-london-1"},
    };
    if (const auto found = legacy_short_codes.find(metadata_region);
        found != legacy_short_codes.end()) {
        return found->second;
    }
    throw std::runtime_error("oci_federation: unrecognised short region code '" + metadata_region +
                             "' from the metadata service — set oci_client_config::region "
                             "explicitly to bypass the lookup");
}

/// @brief The Auth service host for a region.
///
/// The **bare** domain form, not the `.oci.` one — per-service domains are
/// per-service (`spike-notes.md` Finding 10) and the go-sdk derives `auth`
/// endpoints without the label. Checked against DNS: both spellings currently
/// resolve (to the same addresses), and the go-sdk's is the one used here.
[[nodiscard]] inline auto auth_host_for(const std::string& region) -> std::string {
    return "auth." + region + ".oraclecloud.com";
}

/// @brief The signed set for the one federation request, in order.
///
/// `date (request-target) content-length content-type x-content-sha256` —
/// **no `host`**. This is the single place Instance Principal departs from
/// the canonical form every other OCI request uses, and it comes verbatim
/// from the go-sdk's federation client (`genericHeaders = {"date",
/// "(request-target)"}` — its ordinary signer has `"host"` in that list, the
/// federation one does not).
[[nodiscard]] inline auto federation_signed_headers() -> std::vector<std::string> {
    return {"date", "(request-target)", "content-length", "content-type", "x-content-sha256"};
}

/// @brief Canonical string for the federation exchange — @ref
///        federation_signed_headers joined the same way `oci_signing` joins
///        the ordinary set.
///
/// Split out for the same reason `oci_signing::build_signing_string` is: a
/// 401 from the Auth service names nothing, and the only useful test is one
/// that can assert the exact bytes signed.
[[nodiscard]] inline auto build_federation_signing_string(std::string_view method,
                                                          std::string_view request_target,
                                                          const std::string& body,
                                                          const std::string& date_header)
    -> std::string {
    std::ostringstream out;
    out << "date: " << date_header << "\n";
    out << "(request-target): " << oci_signing::detail::to_lower(method) << " " << request_target
        << "\n";
    out << "content-length: " << body.size() << "\n";
    out << "content-type: application/json\n";
    out << "x-content-sha256: " << oci_signing::detail::sha256_base64(body);
    return out.str();
}

/// @brief Generate the ephemeral RSA-2048 session key pair, as PEM.
///
/// A fresh pair per token renewal, matching the go-sdk (`renewSecurityToken`
/// calls `sessionKeySupplier.Refresh()` before every exchange) — the session
/// key's whole point is having no lifetime beyond its token.
[[nodiscard]] inline auto generate_session_key_pem() -> std::string {
    detail::evp_pkey_ctx_ptr ctx{EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr)};
    if (!ctx || EVP_PKEY_keygen_init(ctx.get()) != 1 ||
        EVP_PKEY_CTX_set_rsa_keygen_bits(ctx.get(), 2048) <= 0) {
        detail::throw_openssl_error("oci_federation: could not initialise RSA keygen");
    }
    EVP_PKEY* raw = nullptr;
    if (EVP_PKEY_keygen(ctx.get(), &raw) != 1) {
        detail::throw_openssl_error("oci_federation: RSA keygen failed");
    }
    oci_signing::detail::evp_pkey_ptr key{raw};

    detail::bio_ptr bio{BIO_new(BIO_s_mem())};
    if (!bio || PEM_write_bio_PrivateKey(bio.get(), key.get(), nullptr, nullptr, 0, nullptr,
                                         nullptr) != 1) {
        detail::throw_openssl_error("oci_federation: could not PEM-encode the session key");
    }
    char* data = nullptr;
    const long len = BIO_get_mem_data(bio.get(), &data);  // NOLINT(google-runtime-int)
    return std::string(data, static_cast<std::size_t>(len));
}

/// @brief The session public key as base64 of its PKIX (SubjectPublicKeyInfo)
///        DER — the `publicKey` field of the federation request
///        (go-sdk `privateToPublicDERBase64`: `x509.MarshalPKIXPublicKey` +
///        standard base64).
[[nodiscard]] inline auto public_key_der_base64(const std::string& private_key_pem) -> std::string {
    const auto key = oci_signing::detail::load_private_key(private_key_pem, "");
    unsigned char* der = nullptr;
    const int len = i2d_PUBKEY(key.get(), &der);
    if (len <= 0) {
        detail::throw_openssl_error("oci_federation: i2d_PUBKEY failed");
    }
    struct der_guard {
        unsigned char* p;
        ~der_guard() { OPENSSL_free(p); }
    } guard{der};
    return detail::base64_encode(der, static_cast<std::size_t>(len));
}

/// @brief The federation request body, serialized.
///
/// Field names are the go-sdk's json tags verbatim: `certificate`,
/// `publicKey`, `intermediateCertificates`, `fingerprintAlgorithm` (always
/// `"SHA256"`). No `purpose` field exists in the X.509 variant.
[[nodiscard]] inline auto federation_request_body(const std::string& leaf_cert_pem,
                                                  const std::string& session_public_key_b64,
                                                  const std::string& intermediate_pem_bundle)
    -> std::string {
    boost::json::object body;
    body["certificate"] = sanitize_pem(leaf_cert_pem);
    body["publicKey"] = session_public_key_b64;
    boost::json::array intermediates;
    for (const auto& pem : split_pem_certificates(intermediate_pem_bundle)) {
        intermediates.push_back(boost::json::value(sanitize_pem(pem)));
    }
    if (!intermediates.empty()) {
        body["intermediateCertificates"] = std::move(intermediates);
    }
    body["fingerprintAlgorithm"] = "SHA256";
    return boost::json::serialize(body);
}

/// @brief Read the `exp` claim out of a JWT without verifying it.
///
/// Verification is the Auth service's side of the contract, not this
/// client's — the token is opaque credential material and `exp` is read only
/// to schedule renewal, exactly as the go-sdk's `jwtToken` does.
[[nodiscard]] inline auto parse_jwt_exp(const std::string& token) -> std::time_t {
    const auto first_dot = token.find('.');
    const auto second_dot =
        first_dot == std::string::npos ? std::string::npos : token.find('.', first_dot + 1);
    if (second_dot == std::string::npos) {
        throw std::runtime_error(
            "oci_federation: the security token is not a JWT (expected three '.'-separated "
            "segments)");
    }
    const auto payload_json = detail::base64url_decode(
        std::string_view(token).substr(first_dot + 1, second_dot - first_dot - 1));
    boost::json::value payload;
    try {
        payload = boost::json::parse(payload_json);
    } catch (const std::exception& e) {
        throw std::runtime_error(
            std::string("oci_federation: the security token's payload is not JSON: ") + e.what());
    }
    const auto* obj = payload.if_object();
    const auto* exp = obj != nullptr ? obj->if_contains("exp") : nullptr;
    if (exp == nullptr || !exp->is_number()) {
        throw std::runtime_error(
            "oci_federation: the security token carries no numeric 'exp' claim");
    }
    return static_cast<std::time_t>(exp->to_number<std::int64_t>());
}

/// @brief A federated security token and when it stops being usable.
struct security_token {
    std::string raw;
    std::time_t exp{0};

    /// The go-sdk's `bufferTimeBeforeTokenExpiration`: a token inside five
    /// minutes of `exp` is treated as already expired, so a request signed
    /// now does not arrive dead.
    static constexpr std::time_t expiry_buffer_seconds = 5 * 60;

    [[nodiscard]] auto valid(std::time_t when) const -> bool {
        return !raw.empty() && exp > when + expiry_buffer_seconds;
    }
};

/// @brief The Instance Principal signer: metadata fetch, federation exchange,
///        token cache, session-key signing (Requirement 1.6).
///
/// One instance is safe to share across threads — every entry point takes the
/// internal mutex, which is also what makes the cached token a cache rather
/// than a race. `oci_http_client` holds one when
/// `oci_client_config::use_instance_principal` is set; nothing else needs to
/// construct one directly, but tests do.
class instance_principal_signer {
public:
    struct options {
        /// Metadata service base, `/opc/v2` included. The default is the
        /// link-local address every OCI instance serves; the environment
        /// variable is the same override the go-sdk honours, and the mock
        /// tier's whole entry point.
        std::string metadata_base_url{"http://169.254.169.254/opc/v2"};

        /// Full origin for the Auth service (e.g. `https://auth.us-phoenix-1.
        /// oraclecloud.com`). Empty derives it from the region. The
        /// environment override name is the go-sdk's
        /// (`OCI_SDK_AUTH_CLIENT_REGION_URL`).
        std::string auth_endpoint_override;

        /// When non-empty, used instead of `/instance/region` for deriving
        /// the auth host. The providers always have `cfg.region` in hand, and
        /// preferring it avoids depending on the short-code mapping at all.
        std::string region_hint;

        std::chrono::seconds timeout{30};
    };

    /// Options from a client config plus the two environment overrides.
    [[nodiscard]] static auto options_from(const oci_client_config& cfg) -> options {
        options opts;
        if (const char* metadata = std::getenv("OCI_METADATA_BASE_URL"); metadata != nullptr) {
            opts.metadata_base_url = metadata;
        }
        if (const char* auth = std::getenv("OCI_SDK_AUTH_CLIENT_REGION_URL"); auth != nullptr) {
            opts.auth_endpoint_override = auth;
        }
        opts.region_hint = cfg.region;
        opts.timeout = cfg.api_timeout;
        return opts;
    }

    explicit instance_principal_signer(options opts) : _opts(std::move(opts)) {}

    /// @brief Sign one API request with the session key and `ST$` keyId,
    ///        renewing the token first if it is missing or inside the expiry
    ///        buffer.
    ///
    /// Renewal happens at most once per call and the result is used even if
    /// the fresh token is itself short-lived — matching the go-sdk, which
    /// renews and proceeds rather than looping. @p when is injectable for the
    /// same reason `oci_signing::sign_request`'s is: a test that cannot pin
    /// the clock cannot pin a signature.
    [[nodiscard]] auto sign(std::string_view method, std::string_view request_target,
                            std::string_view host, const std::string& body,
                            std::time_t when = std::time(nullptr))
        -> std::map<std::string, std::string> {
        const std::lock_guard<std::mutex> lock(_mutex);
        if (!_token.valid(when)) {
            refresh_locked();
        }
        return oci_signing::sign_request_with_key("ST$" + _token.raw, _session_private_key_pem,
                                                  /*passphrase=*/"", method, request_target, host,
                                                  body, when);
    }

    /// How many federation exchanges have run — lets a test assert "the
    /// second sign reused the token" / "the expired token was renewed"
    /// without reaching into the internals.
    [[nodiscard]] auto exchange_count() const -> int {
        const std::lock_guard<std::mutex> lock(_mutex);
        return _exchanges;
    }

private:
    /// One full renewal: fresh identity, fresh session key, one exchange.
    ///
    /// The identity documents are re-fetched every time rather than cached,
    /// because the instance certificate itself rotates (the go-sdk refreshes
    /// its certificate retrievers on every renewal for the same reason) — a
    /// cached leaf eventually signs with a certificate the Auth service no
    /// longer recognises.
    void refresh_locked() {
        const std::string region_raw =
            _opts.region_hint.empty() ? fetch_metadata("/instance/region") : _opts.region_hint;
        const std::string leaf_cert = fetch_metadata("/identity/cert.pem");
        const std::string leaf_key = fetch_metadata("/identity/key.pem");
        const std::string intermediate = fetch_metadata("/identity/intermediate.pem");

        _session_private_key_pem = generate_session_key_pem();
        const std::string body = federation_request_body(
            leaf_cert, public_key_der_base64(_session_private_key_pem), intermediate);

        const std::string key_id = extract_tenancy_ocid(leaf_cert) + "/fed-x509-sha256/" +
                                   certificate_fingerprint(leaf_cert);

        const std::string token = exchange(region_raw, key_id, leaf_key, body);
        _token = security_token{token, parse_jwt_exp(token)};
        ++_exchanges;
    }

    /// GET one metadata path, `Authorization: Bearer Oracle` attached — the
    /// v2 metadata service returns 401 without it.
    ///
    /// Three attempts with a short pause, far short of the go-sdk's eight
    /// exponential ones: the metadata service is link-local and the caller
    /// (a quorum poll or a certificate issuance) has its own timeout budget
    /// that eight backed-off attempts would blow through silently.
    [[nodiscard]] auto fetch_metadata(const std::string& path) const -> std::string {
        const auto [origin, prefix] = split_origin(_opts.metadata_base_url);
        httplib::Client client(origin);
        client.set_connection_timeout(static_cast<time_t>(_opts.timeout.count()), 0);
        client.set_read_timeout(static_cast<time_t>(_opts.timeout.count()), 0);
        const httplib::Headers headers{{"Authorization", "Bearer Oracle"}};

        std::string last_error;
        for (int attempt = 0; attempt < 3; ++attempt) {
            if (attempt != 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
            }
            auto result = client.Get(prefix + path, headers);
            if (result && result->status == 200) {
                return result->body;
            }
            last_error = result ? "HTTP " + std::to_string(result->status)
                                : httplib::to_string(result.error());
        }
        throw std::runtime_error("oci_federation: GET " + _opts.metadata_base_url + path +
                                 " failed after 3 attempts: " + last_error +
                                 " — is this process actually running on an OCI instance?");
    }

    /// POST the signed federation request; returns the raw token.
    [[nodiscard]] auto exchange(const std::string& region_raw, const std::string& key_id,
                                const std::string& leaf_key_pem, const std::string& body) const
        -> std::string {
        const std::string origin = _opts.auth_endpoint_override.empty()
                                       ? "https://" + auth_host_for(resolve_region(region_raw))
                                       : _opts.auth_endpoint_override;
        static constexpr const char* path = "/v1/x509";

        const std::string date_header = oci_signing::detail::rfc1123_now(std::time(nullptr));
        const std::string signing_string =
            build_federation_signing_string("POST", path, body, date_header);
        const auto key = oci_signing::detail::load_private_key(leaf_key_pem, "");
        const std::string signature =
            oci_signing::detail::rsa_sha256_sign(key.get(), signing_string);

        std::ostringstream headers_list;
        bool first = true;
        for (const auto& name : federation_signed_headers()) {
            if (!first) {
                headers_list << " ";
            }
            headers_list << name;
            first = false;
        }
        std::ostringstream authorization;
        authorization << "Signature version=\"1\"," << "headers=\"" << headers_list.str() << "\","
                      << "keyId=\"" << key_id << "\"," << "algorithm=\"rsa-sha256\","
                      << "signature=\"" << signature << "\"";

        // The same two wire lessons oci_http_client carries: content-type is
        // httplib's to set (supplying it too puts two on the wire — Finding
        // 7), and the client is constructed inside a try so a TLS-less
        // httplib build names the missing flag instead of a bare "'https'
        // scheme is not supported".
        httplib::Headers headers{
            {"date", date_header},
            {"authorization", authorization.str()},
            {"content-length", std::to_string(body.size())},
            {"x-content-sha256", oci_signing::detail::sha256_base64(body)},
        };

        std::unique_ptr<httplib::Client> client;
        try {
            client = std::make_unique<httplib::Client>(origin);
        } catch (const std::exception& e) {
            throw std::runtime_error(
                "oci_federation: could not create an HTTP client for " + origin + ": " + e.what() +
                " — the Auth service is https, which needs cpp-httplib compiled with "
                "CPPHTTPLIB_OPENSSL_SUPPORT (CONFIG_HTTP_TRANSPORT_TLS=y)");
        }
        client->set_connection_timeout(static_cast<time_t>(_opts.timeout.count()), 0);
        client->set_read_timeout(static_cast<time_t>(_opts.timeout.count()), 0);

        auto result = client->Post(path, headers, body, "application/json");
        if (!result) {
            throw std::runtime_error("oci_federation: POST " + origin + path +
                                     " failed: " + httplib::to_string(result.error()));
        }
        if (result->status < 200 || result->status >= 300) {
            throw std::runtime_error("oci_federation: POST " + origin + path +
                                     " failed with HTTP " + std::to_string(result->status) +
                                     (result->body.empty() ? "" : ": " + result->body));
        }

        boost::json::value parsed;
        try {
            parsed = boost::json::parse(result->body);
        } catch (const std::exception& e) {
            throw std::runtime_error(
                std::string("oci_federation: the federation response is not JSON: ") + e.what());
        }
        const auto* obj = parsed.if_object();
        const auto* token = obj != nullptr ? obj->if_contains("token") : nullptr;
        if (token == nullptr || !token->is_string()) {
            throw std::runtime_error(
                "oci_federation: the federation response carries no string 'token' field");
        }
        return std::string(token->get_string());
    }

    /// Split a base URL into the origin httplib wants and the path prefix to
    /// keep — `http://host:port/opc/v2` → (`http://host:port`, `/opc/v2`).
    [[nodiscard]] static auto split_origin(const std::string& base_url)
        -> std::pair<std::string, std::string> {
        auto scheme_end = base_url.find("://");
        const std::size_t host_start = scheme_end == std::string::npos ? 0 : scheme_end + 3;
        const auto path_start = base_url.find('/', host_start);
        if (path_start == std::string::npos) {
            return {base_url, ""};
        }
        return {base_url.substr(0, path_start), base_url.substr(path_start)};
    }

    options _opts;
    mutable std::mutex _mutex;
    std::string _session_private_key_pem;
    security_token _token;
    int _exchanges{0};
};

}  // namespace kythira::oci_federation
