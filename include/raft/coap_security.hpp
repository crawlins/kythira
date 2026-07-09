#pragma once

// Explicit, mutually-exclusive CoAP channel-security mode configuration
// (coap-transport-security spec). This header is intentionally free of any
// libcoap dependency so it can be included from coap_transport.hpp (which
// forward-declares libcoap types) without dragging in coap3/coap.h; the
// provider *implementations* that need real libcoap types live in
// coap_security_impl.hpp.

#include <raft/coap_exceptions.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

// Forward declarations for libcoap types, mirroring coap_transport.hpp so
// this header can be included standalone.
struct coap_context_t;
struct coap_session_t;
struct coap_address_t;
struct coap_pdu_t;

namespace kythira {

// ── Mode enumeration (Requirement 1.1) ────────────────────────────────────

enum class coap_auth_mode {
    none,
    dtls_psk,
    dtls_pki,
    dtls_rpk,
    oscore
};

// Which side of a session a provider is being asked to configure. DTLS-PSK
// in particular uses genuinely different libcoap entry points for client
// (coap_context_set_psk) vs. server (coap_context_set_psk2) roles, so the
// role has to be known at provider-construction time.
enum class coap_security_role {
    client,
    server
};

// ── Credential structs (Requirement 1.1, Component 1 of design.md) ───────

struct psk_credentials {
    std::string identity;
    std::vector<std::byte> key;
};

struct pki_credentials {
    std::string cert_file;
    std::string key_file;
    std::string ca_file;
    bool verify_peer_cert{true};
    std::vector<std::string> cipher_suites;

    // Optional application-level hook invoked from the CN-validation
    // callback in addition to libcoap's own chain validation. Left null by
    // translate_legacy_fields(): the default behavior trusts libcoap's
    // cert_chain_validation result, matching COAP_PKI_KEY_PEM's normal use.
    std::function<bool(const std::string& peer_cert_pem)> cn_validator;
};

struct rpk_credentials {
    // PEM-encoded raw public/private key buffers (COAP_PKI_KEY_PEM_BUF with
    // is_rpk_not_cert set) — see coap_security_impl.hpp's dtls_rpk_provider.
    std::vector<std::byte> public_key;
    std::vector<std::byte> private_key;
    // Raw peer public keys (DER SubjectPublicKeyInfo bytes, as delivered by
    // the CN-validation callback's asn1_public_cert/asn1_length when RPK is
    // in use) that this side trusts.
    std::vector<std::vector<std::byte>> trusted_peer_keys;
};

enum class oscore_bootstrap {
    static_provisioned,
    edhoc
};

struct edhoc_params {
    std::vector<std::byte> identity_credential;   // this device's own EDHOC credential (CCS)
    std::vector<std::byte> identity_private_key;  // this device's static private key (I or R)
    std::vector<std::byte> peer_credential;       // expected peer credential (CCS)
    bool is_initiator{true};  // coap_client acts as EDHOC Initiator, coap_server as Responder
};

struct oscore_credentials {
    std::vector<std::byte> sender_id;
    std::vector<std::byte> recipient_id;
    std::vector<std::byte> master_secret;
    std::vector<std::byte> master_salt;
    std::string aead_algorithm{"AES-CCM-16-64-128"};
    oscore_bootstrap bootstrap_method{oscore_bootstrap::static_provisioned};
    edhoc_params edhoc;  // used only when bootstrap_method == edhoc
};

enum class ace_target_profile {
    dtls_psk,
    oscore
};

struct ace_oauth_config {
    std::string as_token_endpoint;
    std::string client_id;
    std::string client_secret;
    std::string scope;
    ace_target_profile target_profile{ace_target_profile::dtls_psk};
};

struct coap_security_config {
    coap_auth_mode mode{coap_auth_mode::none};
    std::variant<std::monostate, psk_credentials, pki_credentials, rpk_credentials,
                 oscore_credentials>
        credentials;
    std::optional<ace_oauth_config> ace_bootstrap;
};

// ── Exceptions (Data Models section of design.md) ─────────────────────────

inline auto to_string(coap_auth_mode mode) -> std::string {
    switch (mode) {
        case coap_auth_mode::none:
            return "none";
        case coap_auth_mode::dtls_psk:
            return "dtls_psk";
        case coap_auth_mode::dtls_pki:
            return "dtls_pki";
        case coap_auth_mode::dtls_rpk:
            return "dtls_rpk";
        case coap_auth_mode::oscore:
            return "oscore";
    }
    return "unknown";
}

// Raised when a configured mode's runtime capability (e.g. OSCORE, DTLS) is
// not compiled into the linked libcoap (Requirement 7).
class coap_unsupported_security_mode_error : public coap_security_error {
public:
    coap_unsupported_security_mode_error(coap_auth_mode mode, const std::string& missing_capability)
        : coap_security_error("Channel-security mode '" + to_string(mode) +
                              "' is not supported by this build: " + missing_capability),
          _mode(mode) {}

    [[nodiscard]] auto mode() const -> coap_auth_mode { return _mode; }

private:
    coap_auth_mode _mode;
};

// Raised when EDHOC or ACE-OAuth credential provisioning fails, before any
// coap_security_provider is ever constructed for the target mode
// (Requirements 5.4, 6.3).
class coap_credential_bootstrap_error : public coap_security_error {
public:
    coap_credential_bootstrap_error(const std::string& mechanism, const std::string& reason)
        : coap_security_error("Credential bootstrap via '" + mechanism + "' failed: " + reason),
          _mechanism(mechanism) {}

    [[nodiscard]] auto mechanism() const -> const std::string& { return _mechanism; }

private:
    std::string _mechanism;
};

// Raised on inconsistent mode/credential combinations (Requirements 1.3, 2.3).
class coap_security_config_error : public coap_security_error {
public:
    explicit coap_security_config_error(const std::string& message)
        : coap_security_error(message) {}
};

// ── coap_security_provider interface (Requirement 1.4, Component 2) ──────

class coap_security_provider {
public:
    virtual ~coap_security_provider() = default;

    // Session/context-level setup: PKI/PSK/RPK configure the coap_context_t
    // globally (libcoap then applies it to every session created against
    // that context); OSCORE registers the server-side default OSCORE
    // configuration. No-op for `none`.
    virtual auto configure_session(coap_context_t* ctx) -> void = 0;

    // Message-level hooks. Every mode implemented against this libcoap
    // version protects/deprotects transparently below the PDU send/receive
    // API (the same way DTLS does) rather than exposing a per-PDU call, so
    // these remain identity passthroughs for every provider, including
    // OSCORE — see coap_security_impl.hpp's oscore_provider for details.
    virtual auto protect(coap_pdu_t* pdu) -> coap_pdu_t* { return pdu; }
    virtual auto unprotect(coap_pdu_t* pdu) -> coap_pdu_t* { return pdu; }

    // Client-side session creation. Every mode except OSCORE creates a
    // plain session (the context-level configure_session() above already
    // primed the context with its credentials); OSCORE must create the
    // session through a dedicated constructor that takes the OSCORE
    // configuration directly, so this is virtual rather than a free
    // function call site.
    virtual auto create_client_session(coap_context_t* ctx, const coap_address_t* local_if,
                                       const coap_address_t* server_addr, std::uint8_t proto)
        -> coap_session_t* = 0;

    [[nodiscard]] virtual auto mode() const -> coap_auth_mode = 0;
};

// Constructs the provider for `config.mode`, throwing
// coap_security_config_error if `config.credentials` doesn't hold the
// variant alternative that `config.mode` requires (Requirement 1.3).
auto make_security_provider(const coap_security_config& config, coap_security_role role)
    -> std::unique_ptr<coap_security_provider>;

}  // namespace kythira
