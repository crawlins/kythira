# Implementation Plan

## Implementation Status

Not started. Spec only.

## Overview

Make `oci_certificates_provider::sign_csr()` return a leaf-first
`chain_pem`, guard `issuing_tls_material_source` against any provider that
does not, tighten the `pem_material` contract, and replace the tests that
pinned the wrong shape.

## Tasks

- [ ] 1. PEM chain helper
  - [ ] 1.1 Add `include/raft/pem_chain.hpp` with `split_certificates`,
    `same_certificate` and `leaf_first`, per the design
  - [ ] 1.2 Write `tests/pem_chain_unit_test.cpp` and register it in
    `tests/CMakeLists.txt` next to `tls_material_source_unit_test`, under
    the same OpenSSL gate
  - _Requirements: 1.2, 1.3_

- [ ] 2. Contract documentation
  - [ ] 2.1 Rewrite the `pem_material::chain_pem` doc comment in
    `include/raft/certificate_authority.hpp`
  - [ ] 2.2 Add the `sign_csr()` chain sentence to
    `include/raft/certificate_provider.hpp`
  - _Requirements: 2.1, 2.2_

- [ ] 3. OCI mock server signs real certificates
  - [ ] 3.1 Add `certificate_signer`, `set_certificate_signer()` and
    `chain_mode` / `set_cert_chain_mode()` to `tests/oci_mock_server.hpp`;
    keep the placeholder path as the default so other OCI tests are
    untouched
  - [ ] 3.2 Sign at `CreateCertificate`, store the leaf on the certificate
    record, and serve `certChainPem` per `chain_mode`
  - [ ] 3.3 Link `certificate_authority` into
    `oci_certificates_provider_mock_test` only
  - _Requirements: 4.1_

- [ ] 4. Failing tests first
  - [ ] 4.1 Replace the `chain_pem == root_pem()` assertion at
    `tests/oci_certificates_provider_mock_test.cpp:198`
  - [ ] 4.2 Add mock cases 1-5 from the design's Testing Strategy; confirm
    case 1 and case 5 fail on current `main`
  - [ ] 4.3 Add the leaf-stripping provider case to
    `tests/issuing_tls_material_source_unit_test.cpp`; confirm it fails on
    current `main`
  - _Requirements: 4.2, 4.3, 4.4, 4.5_

- [ ] 5. Fix the OCI provider
  - [ ] 5.1 Assemble `chain_pem` with `pem_chain::leaf_first` in
    `oci_certificates_provider::sign_csr()`, falling back to the cached CA
    bundle when `certChainPem` is empty
  - [ ] 5.2 Map a `certChainPem` parse failure to `std::runtime_error`
    naming the certificate OCID
  - [ ] 5.3 Update the `sign_csr()` doc comment
  - _Requirements: 1.1, 1.2, 1.3, 1.4, 1.5, 1.6, 5.2_

- [ ] 6. Guard the issuing source
  - [ ] 6.1 Prepend the leaf in `issuing_tls_material_source::issue()` when
    `chain_pem` does not start with it, and emit
    `tls_material_source.chain.leaf_prepended`
  - [ ] 6.2 Mention the metric in the class's header comment next to
    `tls_material_source.renewal.failed`
  - _Requirements: 3.1, 3.2, 3.3, 3.4_

- [ ] 7. Real-OCI assertion
  - [ ] 7.1 Extend
    `sign_csr_issues_from_the_callers_csr_and_returns_no_private_key` in
    `tests/oci_certificates_provider_real_test.cpp` with the leaf-first
    and issuer-chain checks
  - [ ] 7.2 Run it through the real-cloud workflow (per project rules on
    real-cloud dispatch) and record the run ID here
  - _Requirements: 4.6_

- [ ] 8. Cross-references
  - [ ] 8.1 Add a note to `.kiro/specs/oci-cloud-provider/requirements.md`
    Requirement 12 pointing at this spec
  - [ ] 8.2 Run the other providers' unit and mock tests and confirm they
    pass unchanged
  - _Requirements: 4.7, 5.1_

## Notes

- `aws_acm_pca_provider_impl.hpp:153` joins `GetCertificate()` and
  `GetCertificateChain()` with no newline check. If ACM ever returns a leaf
  without a trailing newline, the chain's first two PEM blocks merge. Using
  `pem_chain::leaf_first` there is a one-line follow-up, left out to keep
  this change to the provider that is actually wrong.
- No Kconfig change is needed: `pem_chain.hpp` is header-only and is
  included only by code already behind `CONFIG_OCI_CERTIFICATES_PROVIDER`
  or the OpenSSL gate.
