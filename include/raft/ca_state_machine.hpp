#pragma once

/// @file ca_state_machine.hpp
/// @brief Kythira `state_machine` implementation replicating a CA's root
///        material, issuance ledger, and revocation list across a Raft
///        cluster (`ca_cluster_node`, cmd/ca_cluster_node/main.cpp).
///
/// `apply()` only ever records already-computed facts supplied within the
/// command bytes — it never generates key material, computes a certificate
/// signature, or invokes any other non-deterministic cryptographic
/// primitive, so every replica applying the same command bytes reaches
/// identical state (Requirement 17.1). The encrypt/decrypt helpers below are
/// deliberately free functions, not methods on `ca_state_machine` — they are
/// called once by whichever node is bootstrapping or currently leading,
/// outside of `apply()`, with only the resulting ciphertext ever crossing
/// into a command.

#include <raft/types.hpp>

#include <boost/json.hpp>

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace raft::testing {

// `noop` carries no ledger effect — apply() returns immediately without
// touching any field. It exists solely so a freshly-elected leader can
// append and commit one entry in its own current term (the standard Raft
// technique, Raft paper §5.4.2/Figure 8: a leader cannot advance
// commit_index over entries from a previous term by replication count
// alone — it must commit an entry of its own term first, which then
// retroactively commits everything before it). Without this, a
// single-node — or any freshly-restarted — cluster with committed-but-not-
// yet-reapplied entries sitting in its persisted log from a prior run would
// never re-apply them until an unrelated client command happened to arrive.
enum class ca_command_type : std::uint8_t {
    bootstrap_ca = 0,
    record_issuance = 1,
    record_revocation = 2,
    noop = 3,
};

struct ca_ledger_entry {
    std::uint64_t serial{0};
    std::string subject;
    std::vector<std::string> dns_names;
    std::vector<std::string> ip_addresses;
    std::string certificate_pem;  // never a private key
    std::chrono::system_clock::time_point not_before;
    std::chrono::system_clock::time_point not_after;
    std::optional<std::chrono::system_clock::time_point> revoked_at;
};

namespace ca_state_machine_detail {

// ---------------------------------------------------------------------------
// Minimal base64 codec, self-contained so this header doesn't depend on
// file_persistence.hpp's private statics.
// ---------------------------------------------------------------------------

constexpr std::string_view k_b64_alphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

[[nodiscard]] inline auto base64_encode(const std::vector<unsigned char>& in) -> std::string {
    std::string out;
    out.reserve(((in.size() + 2) / 3) * 4);
    for (std::size_t i = 0; i < in.size(); i += 3) {
        std::uint32_t v = static_cast<std::uint32_t>(in[i]) << 16;
        if (i + 1 < in.size()) v |= static_cast<std::uint32_t>(in[i + 1]) << 8;
        if (i + 2 < in.size()) v |= static_cast<std::uint32_t>(in[i + 2]);
        out += k_b64_alphabet[(v >> 18) & 0x3F];
        out += k_b64_alphabet[(v >> 12) & 0x3F];
        out += (i + 1 < in.size()) ? k_b64_alphabet[(v >> 6) & 0x3F] : '=';
        out += (i + 2 < in.size()) ? k_b64_alphabet[v & 0x3F] : '=';
    }
    return out;
}

[[nodiscard]] inline auto base64_decode(const std::string& in) -> std::vector<unsigned char> {
    static const auto tbl = [] {
        std::array<int8_t, 256> t{};
        t.fill(-1);
        for (int i = 0; i < 64; ++i)
            t[static_cast<std::uint8_t>(k_b64_alphabet[static_cast<std::size_t>(i)])] =
                static_cast<int8_t>(i);
        return t;
    }();
    std::vector<unsigned char> out;
    out.reserve(in.size() * 3 / 4);
    std::uint32_t v = 0;
    int bits = 0;
    for (char c : in) {
        if (c == '=') break;
        int8_t b = tbl[static_cast<std::uint8_t>(c)];
        if (b < 0) continue;
        v = (v << 6) | static_cast<std::uint32_t>(b);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<unsigned char>((v >> bits) & 0xFF));
        }
    }
    return out;
}

[[noreturn]] inline void throw_openssl_error(const std::string& context) {
    std::ostringstream out;
    unsigned long code = 0;  // NOLINT(google-runtime-int) — matches OpenSSL's own typedef
    bool first = true;
    while ((code = ERR_get_error()) != 0) {
        char buf[256];
        ERR_error_string_n(code, buf, sizeof(buf));
        if (!first) out << "; ";
        out << buf;
        first = false;
    }
    auto details = out.str();
    throw std::runtime_error(details.empty() ? context : context + ": " + details);
}

constexpr int k_pbkdf2_iterations = 200000;
constexpr int k_salt_len = 16;
constexpr int k_key_len = 32;    // AES-256
constexpr int k_nonce_len = 12;  // Standard GCM nonce size
constexpr int k_tag_len = 16;

[[nodiscard]] inline auto random_bytes(int n) -> std::vector<unsigned char> {
    std::vector<unsigned char> buf(static_cast<std::size_t>(n));
    if (RAND_bytes(buf.data(), n) != 1) throw_openssl_error("ca_state_machine: RAND_bytes failed");
    return buf;
}

[[nodiscard]] inline auto pbkdf2_derive_key(const std::string& passphrase,
                                            const unsigned char* salt, int salt_len)
    -> std::vector<unsigned char> {
    std::vector<unsigned char> key(static_cast<std::size_t>(k_key_len));
    if (PKCS5_PBKDF2_HMAC(passphrase.data(), static_cast<int>(passphrase.size()), salt, salt_len,
                          k_pbkdf2_iterations, EVP_sha256(), k_key_len, key.data()) != 1) {
        throw_openssl_error("ca_state_machine: PBKDF2 key derivation failed");
    }
    return key;
}

struct cipher_ctx_guard {
    EVP_CIPHER_CTX* ctx;
    explicit cipher_ctx_guard(EVP_CIPHER_CTX* c) : ctx(c) {}
    ~cipher_ctx_guard() { EVP_CIPHER_CTX_free(ctx); }
    cipher_ctx_guard(const cipher_ctx_guard&) = delete;
    cipher_ctx_guard& operator=(const cipher_ctx_guard&) = delete;
};

}  // namespace ca_state_machine_detail

/// Encrypts `key_pem` (the CA's private key, PEM text) with AES-256-GCM using
/// a key derived via PBKDF2-HMAC-SHA256 from `passphrase`. Returns a single
/// base64 string bundling salt ‖ nonce ‖ tag ‖ ciphertext — everything needed
/// to decrypt given the same passphrase, and safe to commit to the Raft log
/// (Requirement 17.4: the private key is never present in plaintext in
/// anything replicated or persisted).
[[nodiscard]] inline auto encrypt_ca_private_key(const std::string& key_pem,
                                                 const std::string& passphrase) -> std::string {
    namespace d = ca_state_machine_detail;

    auto salt = d::random_bytes(d::k_salt_len);
    auto nonce = d::random_bytes(d::k_nonce_len);
    auto key = d::pbkdf2_derive_key(passphrase, salt.data(), static_cast<int>(salt.size()));

    EVP_CIPHER_CTX* raw_ctx = EVP_CIPHER_CTX_new();
    if (raw_ctx == nullptr) d::throw_openssl_error("ca_state_machine: EVP_CIPHER_CTX_new failed");
    d::cipher_ctx_guard guard{raw_ctx};

    if (EVP_EncryptInit_ex(raw_ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1)
        d::throw_openssl_error("ca_state_machine: EVP_EncryptInit_ex (cipher) failed");
    if (EVP_CIPHER_CTX_ctrl(raw_ctx, EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(nonce.size()),
                            nullptr) != 1)
        d::throw_openssl_error("ca_state_machine: setting GCM IV length failed");
    if (EVP_EncryptInit_ex(raw_ctx, nullptr, nullptr, key.data(), nonce.data()) != 1)
        d::throw_openssl_error("ca_state_machine: EVP_EncryptInit_ex (key/iv) failed");

    std::vector<unsigned char> ciphertext(key_pem.size());
    int out_len = 0;
    if (EVP_EncryptUpdate(raw_ctx, ciphertext.data(), &out_len,
                          reinterpret_cast<const unsigned char*>(key_pem.data()),
                          static_cast<int>(key_pem.size())) != 1)
        d::throw_openssl_error("ca_state_machine: EVP_EncryptUpdate failed");
    int total_len = out_len;
    int final_len = 0;
    if (EVP_EncryptFinal_ex(raw_ctx, ciphertext.data() + total_len, &final_len) != 1)
        d::throw_openssl_error("ca_state_machine: EVP_EncryptFinal_ex failed");
    total_len += final_len;
    ciphertext.resize(static_cast<std::size_t>(total_len));

    std::vector<unsigned char> tag(static_cast<std::size_t>(d::k_tag_len));
    if (EVP_CIPHER_CTX_ctrl(raw_ctx, EVP_CTRL_GCM_GET_TAG, d::k_tag_len, tag.data()) != 1)
        d::throw_openssl_error("ca_state_machine: retrieving GCM tag failed");

    std::vector<unsigned char> bundle;
    bundle.reserve(salt.size() + nonce.size() + tag.size() + ciphertext.size());
    bundle.insert(bundle.end(), salt.begin(), salt.end());
    bundle.insert(bundle.end(), nonce.begin(), nonce.end());
    bundle.insert(bundle.end(), tag.begin(), tag.end());
    bundle.insert(bundle.end(), ciphertext.begin(), ciphertext.end());
    return d::base64_encode(bundle);
}

/// Inverse of `encrypt_ca_private_key()`. Throws `std::invalid_argument` if
/// `passphrase` is wrong or `encrypted_b64` was corrupted/tampered with (GCM
/// authentication tag mismatch) — callers (`ca_cluster_node` at startup)
/// SHALL treat this as fatal rather than silently proceeding with no CA
/// signer.
[[nodiscard]] inline auto decrypt_ca_private_key(const std::string& encrypted_b64,
                                                 const std::string& passphrase) -> std::string {
    namespace d = ca_state_machine_detail;

    auto bundle = d::base64_decode(encrypted_b64);
    const std::size_t header_len = static_cast<std::size_t>(d::k_salt_len) +
                                   static_cast<std::size_t>(d::k_nonce_len) +
                                   static_cast<std::size_t>(d::k_tag_len);
    if (bundle.size() < header_len) {
        throw std::invalid_argument("ca_state_machine: encrypted bootstrap material is truncated");
    }

    const unsigned char* salt = bundle.data();
    const unsigned char* nonce = bundle.data() + d::k_salt_len;
    const unsigned char* tag = bundle.data() + d::k_salt_len + d::k_nonce_len;
    const unsigned char* ciphertext = bundle.data() + header_len;
    const std::size_t ciphertext_len = bundle.size() - header_len;

    auto key = d::pbkdf2_derive_key(passphrase, salt, d::k_salt_len);

    EVP_CIPHER_CTX* raw_ctx = EVP_CIPHER_CTX_new();
    if (raw_ctx == nullptr) d::throw_openssl_error("ca_state_machine: EVP_CIPHER_CTX_new failed");
    d::cipher_ctx_guard guard{raw_ctx};

    if (EVP_DecryptInit_ex(raw_ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1)
        d::throw_openssl_error("ca_state_machine: EVP_DecryptInit_ex (cipher) failed");
    if (EVP_CIPHER_CTX_ctrl(raw_ctx, EVP_CTRL_GCM_SET_IVLEN, d::k_nonce_len, nullptr) != 1)
        d::throw_openssl_error("ca_state_machine: setting GCM IV length failed");
    if (EVP_DecryptInit_ex(raw_ctx, nullptr, nullptr, key.data(), nonce) != 1)
        d::throw_openssl_error("ca_state_machine: EVP_DecryptInit_ex (key/iv) failed");

    std::vector<unsigned char> plaintext(ciphertext_len);
    int out_len = 0;
    if (ciphertext_len > 0 && EVP_DecryptUpdate(raw_ctx, plaintext.data(), &out_len, ciphertext,
                                                static_cast<int>(ciphertext_len)) != 1)
        d::throw_openssl_error("ca_state_machine: EVP_DecryptUpdate failed");
    int total_len = out_len;

    if (EVP_CIPHER_CTX_ctrl(raw_ctx, EVP_CTRL_GCM_SET_TAG, d::k_tag_len,
                            const_cast<unsigned char*>(tag)) != 1)
        d::throw_openssl_error("ca_state_machine: setting GCM tag failed");

    int final_len = 0;
    if (EVP_DecryptFinal_ex(raw_ctx, plaintext.data() + total_len, &final_len) != 1) {
        throw std::invalid_argument(
            "ca_state_machine: decryption failed — wrong passphrase or corrupted data (GCM "
            "authentication tag "
            "mismatch)");
    }
    total_len += final_len;
    plaintext.resize(static_cast<std::size_t>(total_len));

    return std::string(reinterpret_cast<char*>(plaintext.data()), plaintext.size());
}

namespace ca_state_machine_detail {

[[nodiscard]] inline auto epoch_seconds(std::chrono::system_clock::time_point tp) -> std::int64_t {
    return std::chrono::duration_cast<std::chrono::seconds>(tp.time_since_epoch()).count();
}

[[nodiscard]] inline auto from_epoch_seconds(std::int64_t s)
    -> std::chrono::system_clock::time_point {
    return std::chrono::system_clock::time_point{std::chrono::seconds{s}};
}

[[nodiscard]] inline auto string_array(const std::vector<std::string>& v) -> boost::json::array {
    boost::json::array a;
    for (const auto& s : v) a.push_back(boost::json::string(s));
    return a;
}

[[nodiscard]] inline auto to_string_vector(const boost::json::array& a)
    -> std::vector<std::string> {
    std::vector<std::string> v;
    v.reserve(a.size());
    for (const auto& e : a) v.emplace_back(e.as_string());
    return v;
}

[[nodiscard]] inline auto ledger_entry_to_json(const ca_ledger_entry& e) -> boost::json::object {
    boost::json::object obj;
    obj["serial"] = e.serial;
    obj["subject"] = e.subject;
    obj["dns_names"] = string_array(e.dns_names);
    obj["ip_addresses"] = string_array(e.ip_addresses);
    obj["certificate_pem"] = e.certificate_pem;
    obj["not_before"] = epoch_seconds(e.not_before);
    obj["not_after"] = epoch_seconds(e.not_after);
    if (e.revoked_at.has_value()) {
        obj["revoked_at"] = epoch_seconds(*e.revoked_at);
    }
    return obj;
}

[[nodiscard]] inline auto json_to_ledger_entry(const boost::json::object& obj) -> ca_ledger_entry {
    ca_ledger_entry e;
    e.serial = static_cast<std::uint64_t>(obj.at("serial").to_number<std::int64_t>());
    e.subject = std::string(obj.at("subject").as_string());
    e.dns_names = to_string_vector(obj.at("dns_names").as_array());
    e.ip_addresses = to_string_vector(obj.at("ip_addresses").as_array());
    e.certificate_pem = std::string(obj.at("certificate_pem").as_string());
    e.not_before = from_epoch_seconds(obj.at("not_before").to_number<std::int64_t>());
    e.not_after = from_epoch_seconds(obj.at("not_after").to_number<std::int64_t>());
    if (auto* v = obj.if_contains("revoked_at")) {
        e.revoked_at = from_epoch_seconds(v->to_number<std::int64_t>());
    }
    return e;
}

[[nodiscard]] inline auto bytes_from_json(const boost::json::object& obj)
    -> std::vector<std::byte> {
    auto s = boost::json::serialize(obj);
    std::vector<std::byte> out(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) out[i] = static_cast<std::byte>(s[i]);
    return out;
}

[[nodiscard]] inline auto json_from_bytes(const std::vector<std::byte>& bytes)
    -> boost::json::value {
    std::string s(bytes.size(), '\0');
    for (std::size_t i = 0; i < bytes.size(); ++i) s[i] = static_cast<char>(bytes[i]);
    return boost::json::parse(s);
}

}  // namespace ca_state_machine_detail

/// Encodes a `bootstrap_ca` command. `encrypted_ca_key_pem` SHALL already be
/// the output of `encrypt_ca_private_key()` — never a plaintext key.
[[nodiscard]] inline auto encode_bootstrap_ca_command(std::string root_cert_pem,
                                                      std::string encrypted_ca_key_pem)
    -> std::vector<std::byte> {
    boost::json::object obj;
    obj["type"] = static_cast<int>(ca_command_type::bootstrap_ca);
    obj["root_cert_pem"] = std::move(root_cert_pem);
    obj["encrypted_ca_key_pem"] = std::move(encrypted_ca_key_pem);
    return ca_state_machine_detail::bytes_from_json(obj);
}

/// Encodes a `record_issuance` command from an already-computed
/// `ca_ledger_entry` (the leaf's private key never appears here or anywhere
/// else in the replicated log — Requirement 17.2).
[[nodiscard]] inline auto encode_record_issuance_command(const ca_ledger_entry& entry)
    -> std::vector<std::byte> {
    auto obj = ca_state_machine_detail::ledger_entry_to_json(entry);
    obj["type"] = static_cast<int>(ca_command_type::record_issuance);
    return ca_state_machine_detail::bytes_from_json(obj);
}

/// Encodes a `record_revocation` command.
[[nodiscard]] inline auto encode_record_revocation_command(
    std::uint64_t serial, std::chrono::system_clock::time_point revoked_at)
    -> std::vector<std::byte> {
    boost::json::object obj;
    obj["type"] = static_cast<int>(ca_command_type::record_revocation);
    obj["serial"] = serial;
    obj["revoked_at"] = ca_state_machine_detail::epoch_seconds(revoked_at);
    return ca_state_machine_detail::bytes_from_json(obj);
}

/// Encodes a `noop` command — see `ca_command_type::noop`.
[[nodiscard]] inline auto encode_noop_command() -> std::vector<std::byte> {
    boost::json::object obj;
    obj["type"] = static_cast<int>(ca_command_type::noop);
    return ca_state_machine_detail::bytes_from_json(obj);
}

/// Replicated CA state: root material (encrypted key + plaintext cert, the
/// cert being public by nature) plus the full issuance/revocation ledger.
/// Satisfies `kythira::state_machine<ca_state_machine, std::uint64_t>` so it
/// can be plugged in directly as a Raft `Types::state_machine_type`
/// (`cmd/ca_cluster_node/main.cpp`).
class ca_state_machine {
public:
    using log_index_t = std::uint64_t;

    /// Dispatches on the command's `ca_command_type`. Returns an empty byte
    /// vector on success. Returns a boost::json `{"error": "..."}` payload
    /// (as bytes) — without throwing and without mutating state — for the two
    /// deterministic business-logic rejections: a duplicate `bootstrap_ca`,
    /// or a `record_revocation` naming a serial not present in the ledger.
    /// Throws `std::invalid_argument` only for structurally malformed command
    /// bytes, which should never occur in practice since the leader
    /// validates everything before proposing (Requirement 17.1).
    auto apply(const std::vector<std::byte>& command, log_index_t index) -> std::vector<std::byte> {
        _last_applied_index = index;

        boost::json::object obj;
        try {
            obj = ca_state_machine_detail::json_from_bytes(command).as_object();
        } catch (const std::exception& ex) {
            throw std::invalid_argument(std::string("ca_state_machine: malformed command: ") +
                                        ex.what());
        }

        auto* type_val = obj.if_contains("type");
        if (type_val == nullptr) {
            throw std::invalid_argument("ca_state_machine: command missing \"type\"");
        }
        auto type = static_cast<ca_command_type>(type_val->to_number<int>());

        switch (type) {
            case ca_command_type::bootstrap_ca: {
                if (_bootstrapped) {
                    return error_result("already_bootstrapped");
                }
                _root_cert_pem = std::string(obj.at("root_cert_pem").as_string());
                _encrypted_ca_key_pem = std::string(obj.at("encrypted_ca_key_pem").as_string());
                _bootstrapped = true;
                return {};
            }
            case ca_command_type::record_issuance: {
                _ledger.push_back(ca_state_machine_detail::json_to_ledger_entry(obj));
                return {};
            }
            case ca_command_type::record_revocation: {
                auto serial =
                    static_cast<std::uint64_t>(obj.at("serial").to_number<std::int64_t>());
                auto it =
                    std::find_if(_ledger.begin(), _ledger.end(),
                                 [serial](const ca_ledger_entry& e) { return e.serial == serial; });
                if (it == _ledger.end()) {
                    return error_result("serial_not_found");
                }
                it->revoked_at = ca_state_machine_detail::from_epoch_seconds(
                    obj.at("revoked_at").to_number<std::int64_t>());
                return {};
            }
            case ca_command_type::noop: {
                return {};
            }
        }
        throw std::invalid_argument("ca_state_machine: unknown command type");
    }

    [[nodiscard]] auto get_state() const -> std::vector<std::byte> {
        boost::json::object obj;
        obj["bootstrapped"] = _bootstrapped;
        obj["encrypted_ca_key_pem"] = _encrypted_ca_key_pem;
        obj["root_cert_pem"] = _root_cert_pem;
        boost::json::array ledger_arr;
        for (const auto& e : _ledger)
            ledger_arr.push_back(ca_state_machine_detail::ledger_entry_to_json(e));
        obj["ledger"] = ledger_arr;
        return ca_state_machine_detail::bytes_from_json(obj);
    }

    auto restore_from_snapshot(const std::vector<std::byte>& snapshot, log_index_t index) -> void {
        _last_applied_index = index;
        if (snapshot.empty()) {
            _bootstrapped = false;
            _encrypted_ca_key_pem.clear();
            _root_cert_pem.clear();
            _ledger.clear();
            return;
        }
        auto obj = ca_state_machine_detail::json_from_bytes(snapshot).as_object();
        _bootstrapped = obj.at("bootstrapped").as_bool();
        _encrypted_ca_key_pem = std::string(obj.at("encrypted_ca_key_pem").as_string());
        _root_cert_pem = std::string(obj.at("root_cert_pem").as_string());
        _ledger.clear();
        for (const auto& v : obj.at("ledger").as_array()) {
            _ledger.push_back(ca_state_machine_detail::json_to_ledger_entry(v.as_object()));
        }
    }

    // Not part of the state_machine concept; used by ca_cluster_node's HTTP layer.
    [[nodiscard]] auto has_root_material() const -> bool { return _bootstrapped; }
    [[nodiscard]] auto encrypted_bootstrap_material() const -> const std::string& {
        return _encrypted_ca_key_pem;
    }
    [[nodiscard]] auto root_certificate_pem() const -> const std::string& { return _root_cert_pem; }
    [[nodiscard]] auto ledger() const -> const std::vector<ca_ledger_entry>& { return _ledger; }
    [[nodiscard]] auto last_applied_index() const -> log_index_t { return _last_applied_index; }

private:
    [[nodiscard]] static auto error_result(std::string_view error) -> std::vector<std::byte> {
        boost::json::object obj;
        obj["error"] = boost::json::string(error);
        return ca_state_machine_detail::bytes_from_json(obj);
    }

    bool _bootstrapped{false};
    std::string _encrypted_ca_key_pem;  // AES-256-GCM ciphertext + nonce + tag + salt, base64
    std::string _root_cert_pem;         // plaintext — the CA cert itself is public
    std::vector<ca_ledger_entry> _ledger;
    log_index_t _last_applied_index{0};
};

static_assert(kythira::state_machine<ca_state_machine, std::uint64_t>,
              "ca_state_machine must satisfy kythira::state_machine");

}  // namespace raft::testing
