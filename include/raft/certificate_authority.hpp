#pragma once

/// @file certificate_authority.hpp
/// @brief In-process X.509 certificate authority for issuing real, cryptographically
///        valid root CA and leaf certificates in tests and tooling.
///
/// No OpenSSL types appear in this header: all cryptographic state lives behind a
/// pimpl (`certificate_authority::impl`), so consumers do not need OpenSSL headers
/// merely to hold a `certificate_authority`. See `certificate_authority_impl.hpp`
/// for the implementation.

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace raft::testing {

class certificate_authority;

namespace detail_testing {
/// Test-only accessor for the CA's own private key PEM. Not part of the stable
/// public API — `certificate_authority` deliberately has no such accessor
/// (Requirement 1.3); this exists solely so test code can round-trip a CA's
/// material through `certificate_authority::from_existing()`.
[[nodiscard]] auto unsafe_extract_ca_private_key_pem(const certificate_authority& ca)
    -> std::string;
}  // namespace detail_testing

/// Key algorithm used when generating a root CA or leaf certificate's key pair.
enum class key_algorithm : std::uint8_t {
    rsa_2048,
    rsa_4096,
    ecdsa_p256,
    ecdsa_p384
};

/// Subject/issuer distinguished name fields used when building a certificate.
struct distinguished_name {
    std::string common_name;
    std::string organization{"Kythira Test CA"};
    std::string country{"US"};
};

/// Options controlling root CA generation. See `certificate_authority::certificate_authority`.
struct ca_options {
    distinguished_name subject{.common_name = "Kythira Test Root CA"};
    key_algorithm algorithm{key_algorithm::ecdsa_p256};
    std::chrono::seconds validity{std::chrono::hours(24 * 365)};
};

/// Options controlling leaf certificate issuance. See `certificate_authority::issue`.
struct leaf_certificate_options {
    distinguished_name subject;
    key_algorithm algorithm{key_algorithm::ecdsa_p256};
    std::vector<std::string> dns_names;
    std::vector<std::string> ip_addresses;
    bool server_auth{true};
    bool client_auth{false};
    std::chrono::system_clock::time_point not_before{std::chrono::system_clock::now()};
    std::chrono::seconds validity{std::chrono::hours(24 * 30)};
};

/// PEM-encoded output of a certificate issuance call.
struct pem_material {
    std::string certificate_pem;
    std::string private_key_pem;  ///< Empty for `sign_csr()` results — the CA never sees the key.
    std::string chain_pem;        ///< Leaf + root, PEM-concatenated; empty for the root itself.
    std::uint64_t serial{0};      ///< Exposed so `revoke()` can match without reparsing PEM.
};

/// Options controlling `certificate_authority::sign_csr()`: the same SAN/keyUsage/
/// EKU/validity fields as `leaf_certificate_options`, minus the key-algorithm
/// selector (the key already exists inside the CSR) and the subject (taken from
/// the CSR's own embedded subject).
struct csr_signing_options {
    std::vector<std::string> dns_names;
    std::vector<std::string> ip_addresses;
    bool server_auth{true};
    bool client_auth{false};
    std::chrono::seconds validity{std::chrono::hours(24 * 30)};
};

/// Output of `generate_key_and_csr()`: a freshly generated private key plus a CSR
/// carrying the corresponding public key. The private key never leaves the caller.
struct csr_material {
    std::string private_key_pem;
    std::string csr_pem;
};

/// In-process X.509 certificate authority: generates a self-signed root CA on
/// construction (no filesystem or network I/O) and issues leaf certificates signed
/// by that root. Safe to call concurrently from multiple threads on one instance.
class certificate_authority {
public:
    explicit certificate_authority(ca_options options = {});
    ~certificate_authority();

    certificate_authority(const certificate_authority&) = delete;
    certificate_authority& operator=(const certificate_authority&) = delete;
    certificate_authority(certificate_authority&&) noexcept;
    certificate_authority& operator=(certificate_authority&&) noexcept;

    /// Returns the root CA certificate as PEM text. No accessor exposes the CA's
    /// private key in PEM form — it is consumed internally for signing only.
    [[nodiscard]] auto root_certificate_pem() const -> const std::string&;

    /// Issues a leaf certificate signed by this instance's root CA. Throws
    /// `std::invalid_argument` when both `dns_names` and `ip_addresses` are empty.
    [[nodiscard]] auto issue(leaf_certificate_options options) -> pem_material;

    /// Same as `issue()`, but with `notBefore`/`notAfter` both shifted into the past
    /// (default: issued 2 days ago, expired 1 day ago).
    [[nodiscard]] auto issue_expired(leaf_certificate_options options) -> pem_material;

    /// Same as `issue()`, but with `notBefore` shifted into the future (default: +1 day).
    [[nodiscard]] auto issue_not_yet_valid(leaf_certificate_options options) -> pem_material;

    /// Marks the certificate identified by `cert.serial` as revoked (revocation time
    /// "now"). Throws `std::invalid_argument` if that serial was not issued by this
    /// instance.
    auto revoke(const pem_material& cert) -> void;

    /// Records `serial` as revoked without requiring that this instance
    /// issued it — for reconstructing revocation history (e.g.
    /// `ca_cluster_node` replaying a replicated ledger into a signer rebuilt
    /// via `from_existing()` after a leader failover, which has no local
    /// record of certificates issued by a different instance). Idempotent.
    auto mark_revoked_externally(std::uint64_t serial,
                                 std::chrono::system_clock::time_point revoked_at) -> void;

    /// Returns a PEM-encoded, CA-signed CRL listing every revoked serial and its
    /// revocation time. Rebuilt fresh on every call.
    [[nodiscard]] auto crl_pem() const -> std::string;

    /// Parses `csr_pem`'s embedded public key and subject, applies SAN/keyUsage/
    /// EKU/validity from `options`, signs the resulting certificate with this
    /// instance's CA key, and returns a `pem_material` whose `private_key_pem` is
    /// empty — the CA never sees the requester's private key. Throws
    /// `std::invalid_argument` on an unparseable CSR, a CSR whose self-signature
    /// doesn't verify, or SAN-less `options`.
    [[nodiscard]] auto sign_csr(std::string csr_pem, csr_signing_options options) -> pem_material;

    /// Constructs a `certificate_authority` around already-existing root CA
    /// material instead of generating a fresh root. Throws `std::invalid_argument`
    /// on unparseable PEM or a key/certificate mismatch.
    [[nodiscard]] static auto from_existing(std::string ca_cert_pem, std::string ca_key_pem)
        -> certificate_authority;

    /// Exports this instance's own root CA certificate and private key as
    /// PEM (in `pem_material::certificate_pem`/`private_key_pem`; `chain_pem`
    /// and `serial` are unused and left default). For callers that need to
    /// durably persist or replicate the CA's identity themselves rather than
    /// relying on this in-process instance's lifetime — e.g.
    /// `ca_cluster_node`'s `--bootstrap-ca` path, which encrypts this key
    /// before committing it to the replicated Raft log (Requirement 17.4) and
    /// later reconstructs an equivalent instance via `from_existing()`.
    /// Ordinary callers SHOULD NOT need this: `certificate_authority`
    /// otherwise deliberately never exposes its own private key
    /// (Requirement 1.3) — reach for `issue()`/`sign_csr()` instead.
    [[nodiscard]] auto export_root_material() const -> pem_material;

private:
    friend auto detail_testing::unsafe_extract_ca_private_key_pem(const certificate_authority&)
        -> std::string;

    struct impl;
    std::unique_ptr<impl> _impl;

    explicit certificate_authority(std::unique_ptr<impl> existing_impl);
};

/// Fixture defined in `tests/ca_test_fixture.hpp`; forward-declared here only so
/// `temp_cert_files` can grant it access to `replace_atomically()`.
class ca_test_fixture;

/// RAII helper: materializes a `pem_material`'s non-empty fields as files under a
/// uniquely-named directory beneath the system temp directory, and removes that
/// directory (best-effort) on destruction. The private key file is created with
/// mode 0600.
class temp_cert_files {
public:
    explicit temp_cert_files(const pem_material& material);
    ~temp_cert_files();

    temp_cert_files(const temp_cert_files&) = delete;
    temp_cert_files& operator=(const temp_cert_files&) = delete;
    temp_cert_files(temp_cert_files&&) = delete;
    temp_cert_files& operator=(temp_cert_files&&) = delete;

    [[nodiscard]] auto cert_path() const -> const std::string&;
    [[nodiscard]] auto key_path() const -> const std::string&;
    [[nodiscard]] auto chain_path() const -> const std::string&;  ///< "" if no chain material.

private:
    friend class ca_test_fixture;

    /// Writes `cert.pem.tmp`/`key.pem.tmp`/`chain.pem.tmp` in the same directory,
    /// then `std::filesystem::rename()`s each over its non-`.tmp` counterpart — an
    /// atomic same-filesystem replace per POSIX, so a concurrent reader (e.g. a
    /// transport's `reload_tls_material()` poller) never observes a
    /// partially-written file. Used only by `ca_test_fixture::renew()`.
    auto replace_atomically(const pem_material& material) -> void;

    std::string _dir;
    std::string _cert_path;
    std::string _key_path;
    std::string _chain_path;
};

}  // namespace raft::testing
