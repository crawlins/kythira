#pragma once

/// @file acme_certificate_provider.hpp
/// @brief `certificate_provider` implementation speaking RFC 8555 (ACME) to
///        obtain certificates from any compliant CA (a real one, or
///        `tests/acme_test_server.hpp`'s self-contained mock). Public
///        config/class declaration; see `acme_certificate_provider_impl.hpp`
///        for the wire-protocol implementation.
///
/// Unlike `local_certificate_provider`/`aws_acm_pca_provider`, ACME requires
/// the *client* to actively prove control of each identifier being
/// certified via a challenge — there's no ARN or in-process key to just sign
/// against. `sign_csr()` drives the full RFC 8555 order lifecycle: account
/// (re)use, order creation, per-identifier challenge completion, order
/// finalization with the caller's CSR, and certificate download.
///
/// **`.local` (mDNS) identifiers are specific to `acme_test_server`** (or any
/// private ACME CA an operator runs on the same LAN) — they are ordinary
/// `"dns"`-type identifiers (RFC 8555/8738 define no separate mDNS identifier
/// type) validated via `http-01` only, and validation only succeeds when the
/// *validating* host (the ACME server) has an mDNS-capable resolver
/// configured (`nss-mdns`/Avahi on Linux, native `mDNSResponder` on macOS)
/// AND shares a local network segment with the node being validated. Real
/// public ACME CAs (Let's Encrypt and similar) do not support `.local`
/// identifiers at all — do not request one against a real `directory_url`.

#include <raft/acme_jws.hpp>
#include <raft/certificate_provider.hpp>
#include <raft/fault_injection.hpp>

#include <arpa/inet.h>

#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace raft::testing {

/// DNS-01 UPDATE settings — deliberately a plain struct with the same shape
/// as `rfc2136_ldns_discovery::config` (server/port/zone/TSIG) rather than a
/// hard dependency on that type, so this header doesn't require
/// `KYTHIRA_HAS_LDNS` merely to be included; `acme_certificate_provider_impl.hpp`
/// is the one gated on that macro for the dns-01 code path specifically.
struct acme_dns01_config {
    std::string server;
    std::uint16_t port{53};
    std::string zone;
    std::uint32_t ttl{30};
    std::string tsig_key_name;
    std::string tsig_algorithm{"hmac-sha256."};
    std::string tsig_key_base64;
};

struct acme_certificate_provider_config {
    std::string directory_url;  // e.g. "https://acme.example.com/directory"
    std::optional<std::string>
        account_key_pem;               // reuse an existing account; generate (ES256) if empty
    std::vector<std::string> contact;  // e.g. {"mailto:ops@example.com"}, optional

    enum class challenge_type : std::uint8_t {
        http_01,
        dns_01
    };
    challenge_type challenge{challenge_type::http_01};

    // "host:port" for the short-lived http-01 responder. Production ACME
    // requires port 80; tests SHOULD use ":0" (OS-assigned, non-privileged)
    // since the test CA (acme_test_server) is configured with a matching
    // `http01_validation_port` override rather than the RFC-mandated 80.
    std::string http01_bind_address{"0.0.0.0:80"};

    acme_dns01_config dns01;

    std::chrono::seconds poll_timeout{120};
    std::chrono::milliseconds poll_interval{2000};
};

/// Identifier classification and per-identifier challenge selection
/// (Requirement 20.3/20.5, Property 21). Pure/stateless — kept in the public
/// header rather than the impl file so both `acme_certificate_provider` and
/// tests can call it directly without needing the full ACME wire-protocol
/// machinery.
namespace acme_identifier {

enum class kind : std::uint8_t {
    dns,
    ip
};

/// Classifies `identifier` as `ip` when it parses as an IPv4 or IPv6 literal
/// (`inet_pton`), `dns` otherwise — including `.local` names, since mDNS is
/// not a distinct ACME identifier type (RFC 8555 only defines "dns" and,
/// via RFC 8738, "ip").
[[nodiscard]] inline auto classify(const std::string& identifier) -> kind {
    unsigned char buf[16];  // large enough for either in_addr or in6_addr
    if (inet_pton(AF_INET, identifier.c_str(), buf) == 1) {
        return kind::ip;
    }
    if (inet_pton(AF_INET6, identifier.c_str(), buf) == 1) {
        return kind::ip;
    }
    return kind::dns;
}

/// `ip` identifiers always use `http-01` (RFC 8738 §3 — there is no sensible
/// `_acme-challenge.<ip>.` dns-01 zone for a bare IP literal); `dns`
/// identifiers use whichever challenge type `configured` selects.
[[nodiscard]] inline auto challenge_for(kind identifier_kind,
                                        acme_certificate_provider_config::challenge_type configured)
    -> std::string {
    if (identifier_kind == kind::ip) {
        return "http-01";
    }
    return configured == acme_certificate_provider_config::challenge_type::http_01 ? "http-01"
                                                                                   : "dns-01";
}

}  // namespace acme_identifier

class acme_certificate_provider {
public:
    explicit acme_certificate_provider(acme_certificate_provider_config config);
    ~acme_certificate_provider();

    acme_certificate_provider(const acme_certificate_provider&) = delete;
    acme_certificate_provider& operator=(const acme_certificate_provider&) = delete;
    acme_certificate_provider(acme_certificate_provider&&) = delete;
    acme_certificate_provider& operator=(acme_certificate_provider&&) = delete;

    /// Returns the top-most certificate of the chain most recently returned
    /// by the ACME server's certificate-download endpoint — best-effort/
    /// informational against a real-world CA (Requirement 18.6: real ACME
    /// CAs distribute trust roots out-of-band), authoritative against
    /// `acme_test_server`, whose chain terminates at its own root. Rejects
    /// if no certificate has been obtained yet.
    [[nodiscard]] auto root_certificate_pem() -> kythira::Future<std::string>;

    /// Drives the full RFC 8555 order lifecycle for the identifiers named in
    /// `options` against `_config.directory_url`. `csr_pem`'s own subject/SAN
    /// are NOT what get certified — the order's identifiers (from `options`)
    /// are authoritative, exactly mirroring how `certificate_authority::sign_csr()`
    /// takes `csr_signing_options` separately from the CSR itself.
    [[nodiscard]] auto sign_csr(std::string csr_pem, csr_signing_options options)
        -> kythira::Future<pem_material>;

private:
    acme_jws::evp_pkey_ptr _account_key;
    std::optional<std::string> _account_url;
    std::string _last_root_pem;
    acme_certificate_provider_config _config;
    std::mutex _mutex;
};

static_assert(certificate_provider<acme_certificate_provider>);

}  // namespace raft::testing
