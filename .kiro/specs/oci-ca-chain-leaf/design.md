# Design Document

## Overview

Three small changes close the gap:

1. A shared header-only helper, `include/raft/pem_chain.hpp`, that splits a
   PEM bundle into certificates, compares two certificates by DER, and
   builds a leaf-first chain.
2. `oci_certificates_provider::sign_csr()` uses it to assemble `chain_pem`
   from `certificatePem` and `certChainPem`.
3. `issuing_tls_material_source::issue()` uses it to prepend the leaf when
   a provider hands back an issuer-only chain, and emits a metric when it
   has to.

The OCI mock server learns to sign CSRs with a real test CA, so the tests
can check real chains and compose the provider with the issuing source.

### Key design decision: fix the provider, and also guard the consumer

The bug is in the OCI provider, and Requirement 1 fixes it there. The guard
in `issuing_tls_material_source` (Requirement 3) is defence in depth: six
providers implement `certificate_provider`, the contract was only loosely
documented, and the failure mode (a node that never gets an identity) is
severe and quiet. The guard costs one DER comparison per issuance. It
emits a metric instead of correcting silently, so a future provider with
the same bug is still visible.

### Key design decision: compare DER, not PEM text

OCI may wrap base64 at a different width, or end without a trailing
newline. Comparing PEM strings would miss a leaf that is really there
(Requirement 1.2) and duplicate it. Decoding both blocks with
`PEM_read_bio_X509` and comparing `i2d_X509` output is exact.

### Key design decision: empty `certChainPem` falls back to the CA bundle

OCI's bundle schema does not promise a non-empty `certChainPem` (inferred,
not observed: the real test only saw it non-empty). If it is empty,
returning `chain_pem = certificate_pem` alone would satisfy "starts with the
leaf" but not "ends at the root", and every other provider includes the
root. `root_certificate_pem()` is already cached after its first call
(`oci_certificates_provider.hpp:190`), so the fallback normally costs
nothing. If that fetch fails, `sign_csr()` fails, rather than returning a
chain that silently lacks its issuer.

### Key design decision: the mock signs through a callback

`tests/oci_mock_server.hpp` serves every OCI mock test (compute, object
storage, pools, certificates). Making it depend on
`raft::testing::certificate_authority` would force the `certificate_authority`
library onto every OCI test target. Instead the mock gains an optional
signer callback. Without one it keeps today's placeholder PEM. The
certificates mock test installs a signer backed by a real
`certificate_authority`, and only that target links the library.

## Components and Interfaces

### `include/raft/pem_chain.hpp` (new)

```cpp
namespace kythira::pem_chain {

/// Splits a PEM bundle into one string per certificate, each ending in '\n'.
/// Non-certificate blocks are ignored. Throws std::invalid_argument when a
/// CERTIFICATE block fails to parse.
auto split_certificates(std::string_view bundle) -> std::vector<std::string>;

/// True when both PEM certificates decode to identical DER.
/// Throws std::invalid_argument when either fails to parse.
auto same_certificate(std::string_view a, std::string_view b) -> bool;

/// Returns leaf followed by every certificate in issuers, skipping a leading
/// copy of the leaf if issuers already starts with it. Each block ends in '\n'.
auto leaf_first(std::string_view leaf, std::string_view issuers) -> std::string;

}  // namespace kythira::pem_chain
```

It depends only on OpenSSL (`<openssl/pem.h>`, `<openssl/x509.h>`), which
both callers already include. It carries the project copyright header.

### `oci_certificates_provider::sign_csr()` (`include/raft/oci_certificates_provider.hpp`)

After the bundle fetch:

```cpp
auto issuers = oci_certificates_detail::json_string(bundle, "certChainPem");
if (issuers.empty()) {
    issuers = root_certificate_pem().get();   // cached
}
std::string chain;
try {
    chain = kythira::pem_chain::leaf_first(certificate_pem, issuers);
} catch (const std::invalid_argument& e) {
    throw std::runtime_error("GetCertificateBundle returned an unparseable certChainPem for " +
                             certificate_id + ": " + e.what());
}
```

The `std::invalid_argument` is rethrown as `std::runtime_error` on purpose:
`sign_csr()` keeps `invalid_argument` for caller errors (HTTP 400 at the CA
service), and a bad bundle from OCI is an upstream failure (502). The
existing outer `catch (const std::exception&)` then prefixes
`oci_certificates_provider::sign_csr: `.

An empty `certificate_pem` is already refused before this point
(`:290-293`), so `leaf_first` always has a leaf.

### `issuing_tls_material_source::issue()` (`include/raft/issuing_tls_material_source.hpp`)

```cpp
std::string chain = signed_cert.chain_pem;
if (chain.empty()) {
    chain = signed_cert.certificate_pem;
} else {
    auto certs = kythira::pem_chain::split_certificates(chain);
    if (certs.empty() ||
        !kythira::pem_chain::same_certificate(certs.front(), signed_cert.certificate_pem)) {
        chain = kythira::pem_chain::leaf_first(signed_cert.certificate_pem, chain);
        emit("tls_material_source.chain.leaf_prepended");
    }
}
```

A parse failure throws inside `issue()`, which `attempt()` already turns into
`tls_material_source.renewal.failed` plus backoff. That matches how an
unparseable leaf is handled today.

### `pem_material` and `certificate_provider` docs

`certificate_authority.hpp:72` becomes:

```cpp
std::string chain_pem;  ///< Leaf first, then each issuer up to and including the
                        ///< root, PEM-concatenated. Empty only for the root itself.
```

`certificate_provider.hpp`'s concept comment gains one sentence pointing at
that contract for `sign_csr()` results.

### OCI mock server (`tests/oci_mock_server.hpp`)

```cpp
/// Signs a CSR and returns {leaf_pem, issuer_chain_pem}. When unset, the mock
/// returns placeholder PEM as it always has.
using certificate_signer =
    std::function<std::pair<std::string, std::string>(const std::string& csr_pem)>;

auto set_certificate_signer(certificate_signer signer, std::string root_pem) -> void;

enum class chain_mode { issuers_only, leaf_included, empty, garbage };
auto set_cert_chain_mode(chain_mode mode) -> void;   // default issuers_only
```

- `set_certificate_signer` also replaces `_root_pem`, so
  `GetCertificateAuthorityBundle` returns the real root.
- The CSR is signed at `CreateCertificate` time and stored with the
  certificate record. `GetCertificateBundle` returns the stored leaf as
  `certificatePem` and, per `chain_mode`, the issuer chain, the leaf plus
  the issuer chain, an empty string, or a non-PEM string as
  `certChainPem`.
- The signing CA is the built-in `raft::testing::certificate_authority`
  (`sign_csr`, `certificate_authority.hpp:146`). Its `chain_pem` is already
  leaf + root, so the signer returns `{certificate_pem, root_pem}`.

`fake_csr_pem` in the mock test is not a parseable CSR. The cases that use
the real signer generate a real CSR with
`raft::testing::generate_key_and_csr()`, as
`issuing_tls_material_source` does. The existing request-shape case keeps
the placeholder path and only loses its line 198 assertion.

## Testing Strategy

`tests/pem_chain_unit_test.cpp` (new, no network):

1. `split_certificates` on one block, three blocks, blocks without a
   trailing newline, CRLF line endings, and a bundle with a private-key
   block mixed in (ignored).
2. `same_certificate`: identical PEM, the same certificate rewrapped at 76
   columns, and two different certificates.
3. `leaf_first`: issuer-only input, input already starting with the leaf
   (no duplicate), and empty issuers (leaf alone).
4. A malformed CERTIFICATE block throws `std::invalid_argument`.

`tests/oci_certificates_provider_mock_test.cpp`:

1. Real signer, `issuers_only`: first certificate of `chain_pem` is the
   leaf, last is the mock root, two certificates total. This is the case
   that fails on today's code.
2. Real signer, `leaf_included`: the leaf appears exactly once.
3. Real signer, `empty`: `chain_pem` is leaf + root from
   `GetCertificateAuthorityBundle`, and no extra bundle request is made when
   the root was already cached.
4. Real signer, `garbage`: `sign_csr()` fails with `std::runtime_error`
   naming the certificate OCID.
5. Composition: `issuing_tls_material_source<oci_certificates_provider>`
   against the mock reaches generation 1, its chain passes
   `validate_certificate_key_pair`, and
   `tls_material_source.chain.leaf_prepended` is not emitted.

`tests/issuing_tls_material_source_unit_test.cpp`:

1. A `switchable_provider` variant that strips the leaf from `chain_pem`:
   generation 1 is published, its chain starts with the leaf, and the
   metric is emitted once per issuance (checked across one renewal).
2. Existing cases: the metric is never emitted.

`tests/oci_certificates_provider_real_test.cpp` (credentialed, run only by
the real-cloud workflow):

1. `sign_csr_issues_from_the_callers_csr_and_returns_no_private_key` gains:
   the first certificate of `chain_pem` is the same certificate as
   `certificate_pem`, and for each adjacent pair the issuer name of one
   equals the subject name of the next.

## Correctness Properties

### Property 1: Every OCI chain starts with its leaf

*For any* successful `oci_certificates_provider::sign_csr()` result,
`split_certificates(chain_pem).front()` is the same certificate as
`certificate_pem`, and the leaf appears exactly once.
**Validates: Requirements 1.1, 1.2, 1.4**

### Property 2: The issuing source never publishes a chain that does not start with its leaf

*For any* provider result with a non-empty `certificate_pem`, the
`certificate_chain_pem` that `issue()` publishes starts with that
certificate.
**Validates: Requirement 3.1**

### Property 3: Correct providers are untouched

*For any* provider result whose `chain_pem` is already leaf-first or empty,
the published material is byte-identical to what `issue()` publishes
today, and no metric is emitted.
**Validates: Requirements 3.3, 4.7**
