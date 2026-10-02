# Requirements Document

## Introduction

`pem_material::chain_pem` is documented as "Leaf + root, PEM-concatenated"
(`include/raft/certificate_authority.hpp:72`). Every `certificate_provider`
except one builds it that way:

| Provider | Where | `chain_pem` |
|---|---|---|
| Built-in `certificate_authority` | `certificate_authority_impl.hpp:558,621` | leaf + root |
| `aws_acm_pca_provider` | `aws_acm_pca_provider_impl.hpp:153` | leaf + `GetCertificateChain()` |
| `gcp_privateca_certificate_provider` | `gcp_privateca_certificate_provider_impl.hpp:193-204` | leaf + `pem_certificate_chain` |
| `azure_key_vault_ca_provider` | `azure_key_vault_ca_provider_impl.hpp:375` | leaf + CA certificate |
| `acme_certificate_provider` | `acme_certificate_provider_impl.hpp:601` | the ACME `certificate` resource, leaf first (RFC 8555 §7.4.2) |
| `oci_certificates_provider` | `oci_certificates_provider.hpp:298` | **`certChainPem` only: the issuer chain, no leaf** |

OCI's `GetCertificateBundle` returns the leaf in `certificatePem` and the
issuing CA chain in `certChainPem`. `sign_csr()` copies `certChainPem`
straight into `chain_pem`, so the leaf is missing.

That breaks the one consumer that prefers `chain_pem`.
`issuing_tls_material_source::issue()`
(`include/raft/issuing_tls_material_source.hpp:171-173`) publishes
`chain_pem` as `tls_material::certificate_chain_pem` whenever it is
non-empty. `publish()` runs `validate_tls_material()`, which checks the
*first* certificate of the chain against the freshly generated private key
(`tls_material_source.hpp:115-126`). With OCI the first certificate is the
CA's, so validation throws "private key does not match certificate" on
every issuance. The source never reaches generation 1: it reports
`tls_material_source.renewal.failed` and backs off forever. Had validation
not caught it, the node would have presented the CA certificate with the
leaf's key and every TLS handshake would fail.

Nothing caught this because both OCI tests accept the wrong shape:

- `tests/oci_certificates_provider_mock_test.cpp:198` asserts
  `chain_pem == root_pem()`, which pins the bug.
- `tests/oci_certificates_provider_real_test.cpp:180` checks only that
  `chain_pem` is non-empty.
- The mock server's leaf and root are placeholder strings, not
  certificates (`tests/oci_mock_server.hpp:1195-1197,1289`), so no test can
  compose `oci_certificates_provider` with `issuing_tls_material_source`.

The rejection inside `issuing_tls_material_source` is inferred from reading
the code, not observed: no build here composes the two. The missing leaf
itself is verified from the code and the mock test.

Source: `/mnt/project-files/audits/parallel-implementation-parity-audit.md`,
finding S2 (re-verified against `main` at `9c738d2`).

### Non-goals

- Changing `chain_pem`'s meaning for other providers. They already match
  the contract.
- Wiring `oci_certificates_provider` into `ca_service --provider`. It is not
  offered there today, and that is a separate feature.
- The OCI provider's other parity gaps from the same audit (S5 `revoke()`
  serials, S6 ignored validity fields).
- Newline handling in `aws_acm_pca_provider`'s concatenation
  (`aws_acm_pca_provider_impl.hpp:153` joins two PEM strings without
  checking for a trailing newline). It is noted in `tasks.md` as a
  follow-up, not fixed here.

## Glossary

- **Leaf**: the end-entity certificate issued from the caller's CSR,
  `pem_material::certificate_pem`.
- **Issuer chain**: the certificates from the leaf's issuer up to and
  including the root, as OCI returns them in `certChainPem`.
- **Leaf-first chain**: a PEM concatenation whose first certificate is the
  leaf, followed by its issuer chain. This is what `chain_pem` must hold.
- **Same certificate**: two PEM blocks whose decoded DER bytes are equal.
  Comparing DER, not PEM text, ignores line wrapping and trailing
  whitespace differences.

## Requirements

### Requirement 1: OCI `chain_pem` starts with the leaf

**User Story:** As an operator issuing node certificates from OCI
Certificates Management, I want `sign_csr()` to return the same chain shape
as every other provider, so that code written against `certificate_provider`
works unchanged on OCI.

#### Acceptance Criteria

1. WHEN `oci_certificates_provider::sign_csr()` succeeds THEN `chain_pem`
   SHALL be a leaf-first chain: `certificate_pem`, then the certificates of
   `certChainPem` in the order OCI returned them.
2. WHEN `certChainPem` already begins with the same certificate as
   `certificatePem` THEN `chain_pem` SHALL NOT repeat the leaf.
3. Each PEM block in `chain_pem` SHALL end with a newline, so the
   concatenation never joins an `-----END CERTIFICATE-----` line to the
   next `-----BEGIN CERTIFICATE-----`.
4. WHEN `certChainPem` is absent or empty THEN `chain_pem` SHALL be the leaf
   followed by the CA bundle from `root_certificate_pem()` (cached after the
   first call, so this normally costs no request). The provider SHALL NOT
   return a non-empty `chain_pem` that lacks the leaf.
5. WHEN `certChainPem` is present but contains no parseable certificate
   THEN `sign_csr()` SHALL fail with `std::runtime_error` naming the
   certificate OCID. It SHALL NOT return a partial chain.
6. `certificate_pem`, `private_key_pem`, and `serial` SHALL be unchanged by
   this work.

### Requirement 2: The `pem_material` contract says what "chain" means

**User Story:** As a developer adding the next CA provider, I want the
`chain_pem` contract stated precisely, so that I do not repeat this bug.

#### Acceptance Criteria

1. THE doc comment on `pem_material::chain_pem` SHALL state that it is a
   leaf-first chain: the leaf, then each issuer up to and including the
   root, and that it is empty only for the root itself.
2. THE `certificate_provider` concept's documentation
   (`include/raft/certificate_provider.hpp`) SHALL state that `sign_csr()`
   results follow that contract.

### Requirement 3: `issuing_tls_material_source` refuses to lose the leaf

**User Story:** As an operator, I want the issuing source to serve a
working identity even if some provider gets the chain shape wrong, so that
one provider bug cannot take a node's TLS down.

#### Acceptance Criteria

1. WHEN `issue()` receives a non-empty `chain_pem` whose first certificate
   is not the same certificate as `certificate_pem` THEN it SHALL publish
   `certificate_pem` followed by `chain_pem` as `certificate_chain_pem`.
2. WHEN that correction happens THEN the source SHALL emit the metric
   `tls_material_source.chain.leaf_prepended`, so the provider bug is
   visible rather than silently papered over.
3. WHEN `chain_pem` is already leaf-first, or empty, THEN the published
   material SHALL be exactly what it is today.
4. THE correction SHALL NOT reorder, drop, or deduplicate any other
   certificate in the chain.

### Requirement 4: Tests pin the leaf-first shape

**User Story:** As a maintainer, I want tests that fail on the old
behaviour, so that the regression cannot come back.

#### Acceptance Criteria

1. THE OCI mock server SHALL issue real X.509 certificates: it SHALL sign the
   submitted CSR with a real test CA and return that CA's certificate as
   `certChainPem` and from `GetCertificateAuthorityBundle`.
2. `tests/oci_certificates_provider_mock_test.cpp` SHALL assert that
   `chain_pem`'s first certificate is the same certificate as
   `certificate_pem` and that its last is the mock CA's root. The assertion
   at line 198 that pins the old shape SHALL be replaced.
3. THE mock test SHALL cover Requirement 1.2 (leaf already present), 1.4
   (empty `certChainPem`), and 1.5 (unparseable `certChainPem`) through
   mock switches.
4. A mock test SHALL compose `oci_certificates_provider` with
   `issuing_tls_material_source` and assert that generation 1 is published
   and that its certificate chain validates against the private key.
5. `tests/issuing_tls_material_source_unit_test.cpp` SHALL cover
   Requirement 3 with a provider that returns an issuer-only chain, and
   SHALL check the metric is emitted once per affected issuance.
6. `tests/oci_certificates_provider_real_test.cpp` SHALL assert, against
   real OCI, that `chain_pem`'s first certificate is the leaf and that each
   certificate in it is issued by the next (subject/issuer name match).
7. Existing tests for the other five providers SHALL pass unchanged.

### Requirement 5: Documentation

**User Story:** As a reader of the OCI spec, I want to find this correction
from the requirement it amends.

#### Acceptance Criteria

1. `.kiro/specs/oci-cloud-provider/requirements.md` Requirement 12 SHALL
   gain a note pointing to this spec for `chain_pem`'s shape.
2. THE doc comment on `oci_certificates_provider::sign_csr()` SHALL say
   that `chain_pem` is assembled from `certificatePem` and `certChainPem`,
   and why.
