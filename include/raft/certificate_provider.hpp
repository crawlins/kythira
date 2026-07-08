#pragma once

/// @file certificate_provider.hpp
/// @brief `certificate_provider` concept — a common interface for "something that
///        turns a CSR into a signed certificate" — plus the free functions and
///        adapter that let the local, in-process `certificate_authority` satisfy
///        it. The CSR is the deliberate seam between local and cloud issuance:
///        `certificate_authority::sign_csr()`, ACM Private CA's
///        `IssueCertificate`, and `ca_service`'s `/v1/certificates` route all take
///        a CSR in and hand back a certificate — never a private key.

#include <raft/certificate_authority.hpp>
#include <raft/future.hpp>

#include <concepts>
#include <string>

namespace raft::testing {

/// Generates a fresh key pair and a CSR carrying its public key and
/// `options.subject`/SAN entries, entirely locally — no `certificate_authority`
/// instance is needed. The private key never leaves the caller: it is returned
/// in `csr_material::private_key_pem` alongside the CSR, not transmitted anywhere.
[[nodiscard]] auto generate_key_and_csr(leaf_certificate_options options) -> csr_material;

/// Requires, for an lvalue `P& p`, a CSR PEM string, and `csr_signing_options`:
/// `p.root_certificate_pem()` and `p.sign_csr(csr_pem, options)`, both returning
/// `kythira::Future`s. Satisfied by `local_certificate_provider` (this header) and
/// `aws_acm_pca_provider` (`aws_acm_pca_provider.hpp`) — the only two places that
/// know which backend is actually in use; every call site above this concept is
/// backend-agnostic.
template<typename P>
concept certificate_provider = requires(P& p, std::string csr_pem, csr_signing_options options) {
    { p.root_certificate_pem() } -> std::same_as<kythira::Future<std::string>>;
    { p.sign_csr(csr_pem, options) } -> std::same_as<kythira::Future<pem_material>>;
};

/// Adapts a local, in-process `certificate_authority` to `certificate_provider` by
/// wrapping its synchronous calls in immediately-resolved futures.
class local_certificate_provider {
public:
    explicit local_certificate_provider(certificate_authority& ca) : _ca(ca) {}

    [[nodiscard]] auto root_certificate_pem() -> kythira::Future<std::string> {
        return kythira::FutureFactory::makeReadyFuture(std::string(_ca.root_certificate_pem()));
    }

    [[nodiscard]] auto sign_csr(std::string csr_pem, csr_signing_options options)
        -> kythira::Future<pem_material> {
        return kythira::FutureFactory::makeReadyFuture(
            _ca.sign_csr(std::move(csr_pem), std::move(options)));
    }

private:
    certificate_authority& _ca;  ///< Non-owning; caller manages lifetime.
};

static_assert(certificate_provider<local_certificate_provider>);

}  // namespace raft::testing
