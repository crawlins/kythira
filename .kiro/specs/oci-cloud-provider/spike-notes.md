# Spike Notes — OCI Cloud Provider Support

**Date**: 2026-07-28
**Method**: Desk research against Oracle's own officially-maintained,
open-source artifacts — the `oci-go-sdk`, `oci-python-sdk`, and
`terraform-provider-oci` repositories (all `github.com/oracle/*`), plus
OCI CLI command-reference documentation — rather than experimentation
against a live tenancy. This is the same category of evidence Task 0
originally called for ("confirmed against current OCI API documentation
and/or a live tenancy"); a live tenancy was not used for this pass. Findings
below are traceable to specific files/pages, not inferred from training
data alone. Items not resolved by this pass remain open (see Conclusions).

## Findings

### 1. OCI Request Signing Version 1 canonical string — CONFIRMED, one correction to `design.md`'s draft

Source: `oracle/oci-go-sdk`, `common/http_signer.go` (self-contained —
unlike `oci-python-sdk`'s `signer.py`, which delegates the actual signing-
string construction to the external `httpsig_cffi` library and only
confirms the *signed-header list*, not the join format).

Confirmed construction:
- Each signed header becomes a line `"{lowercased-name}: {value}"`; lines
  are joined with `"\n"` (no trailing newline noted).
- The `(request-target)` pseudo-header's value is `"{lowercased-method}
  {request-uri-path-and-query}"`.
- **Header order for a GET/DELETE (no body)**: `date`, `(request-target)`,
  `host` — in that order.
- **Header order for a POST/PUT/PATCH (with body)**: the same three,
  followed by `content-length`, `content-type`, `x-content-sha256`.
- `authorization` header value shape:
  `Signature version="1",headers="{space-separated signed-header list,
  in the order above}",keyId="{tenancy_ocid}/{user_ocid}/{fingerprint}",
  algorithm="rsa-sha256",signature="{base64 RSA-SHA256 signature over the
  joined signing string}"`.

**Correction to `design.md`'s "OCI Request Signing Version 1 — canonical
form" section**: that section's draft listed `(request-target)` as the
*first* signed header, ahead of `date`. The confirmed order is `date`
first, then `(request-target)`, then `host`. This is a real, load-bearing
correction — signing a canonical string with headers in the wrong order
produces a signature OCI will reject — and is applied to `design.md` in
this same pass.

`oci-python-sdk`'s `signer.py` independently confirms the *signed-header
set* (not the join order, since it delegates that): generic
`["date", "(request-target)", "host"]`, body-additional
`["content-length", "content-type", "x-content-sha256"]`, algorithm
`rsa-sha256`, version `"1"` — consistent with the Go SDK's fuller picture.

**Resolution**: Requirement 1's canonical-string ACs and `design.md`'s
signing section are corrected to this confirmed order. `oci_signing.hpp`'s
implementation and its golden-vector unit test (Requirement 13.3) can be
written directly from this finding with no further spike needed for the
API-key signing path specifically.

**Not yet confirmed by this pass**: whether trailing whitespace/newline
handling, and behavior for headers with multiple values, match exactly —
these are edge cases neither SDK's public docs page addressed and would
need either reading further into each SDK's request-construction code or
a live-tenancy round-trip test. Low risk (the common case — one value per
header, one line per header — is fully confirmed) but not exhaustively
verified.

### 2. Instance Pool growth cannot target a specific Availability Domain — CONFIRMED

Source: `oracle/terraform-provider-oci`,
`website/docs/r/core_instance_pool.html.markdown` (raw GitHub source; the
same content is also published at
`docs.oracle.com/en-us/iaas/tools/terraform-provider-oci/latest/docs/r/core_instance_pool.html`,
which blocked automated fetching directly — see Method note below — so the
GitHub-hosted markdown source was used instead).

Confirmed: `oci_core_instance_pool`'s `size` argument is a single pool-wide
integer ("The number of instances that should be in the instance pool.
Modifying this value will override the size of the instance pool.").
`placement_configurations` takes one entry per Availability Domain (plus
optional `fault_domains`), but carries no per-entry instance-count field.
The provider's own guidance: "the system makes a best effort to distribute
instances across all fault domains based on capacity" when no explicit
placement is pinned — i.e. OCI's own placement algorithm decides where
pool growth lands, not the caller.

**Resolution**: Requirement 6.2's "open question" is settled in favor of
the documented fallback: `oci_instance_pool_quorum_manager` requires one
Instance Pool per Availability Domain when per-AD topology control matters
(exactly the operational constraint `design.md`'s Non-Goals already
anticipated). This is now stated as a confirmed design constraint, not a
conditional, in `requirements.md`/`design.md`.

### 3. OCI Certificates Management DOES accept a caller-supplied CSR — CONFIRMED, reverses this spec's most significant open risk

Source: `oracle/oci-go-sdk`,
`certificatesmanagement/create_certificate_managed_externally_issued_by_internal_ca_config_details.go`
(raw GitHub source — the literal Go struct Oracle's own SDK generator
produced), cross-confirmed independently via the OCI CLI command reference
page `create-certificate-managed-externally-issued-by-internal-ca`
(`docs.oracle.com/en-us/iaas/tools/oci-cli/.../certs-mgmt/certificate/...`,
surfaced through search-engine indexing since direct fetch of
`docs.oracle.com` was blocked for this session — see Method note).

Confirmed: a third `certificate_config` `config_type`,
`MANAGED_EXTERNALLY_ISSUED_BY_INTERNAL_CA` (alongside `IMPORTED` and
`ISSUED_BY_INTERNAL_CA`, the two this spec's `requirements.md` draft
already knew about), takes:

```go
type CreateCertificateManagedExternallyIssuedByInternalCaConfigDetails struct {
    IssuerCertificateAuthorityId *string `mandatory:"true" json:"issuerCertificateAuthorityId"`
    CsrPem                       *string `mandatory:"true" json:"csrPem"`
    VersionName                  *string `mandatory:"false" json:"versionName"`
    Validity                     *Validity `mandatory:"false" json:"validity"`
}
```

`CsrPem`'s doc comment: "The certificate signing request (in PEM format)."
Both `IssuerCertificateAuthorityId` and `CsrPem` are `mandatory:"true"`.
This is the direct OCI analogue of AWS ACM Private CA's
`IssueCertificate(CSR)` — the caller generates the key pair and CSR
locally, submits only the CSR, and OCI's CA never sees or generates the
private key.

**Resolution**: This settles `requirements.md` Requirement 12.1's central
open question in favor of Requirement 12.2 (the "good" path) — `sign_csr`
calls `CreateCertificate` with
`certificateConfig.configType = "MANAGED_EXTERNALLY_ISSUED_BY_INTERNAL_CA"`,
`issuerCertificateAuthorityId = config.certificate_authority_id`, and
`csrPem = csr_pem`, and returns `pem_material` with `private_key_pem`
empty — matching `aws_acm_pca_provider::sign_csr`'s contract and
`certificate_provider.hpp`'s "never a private key" invariant exactly, with
**no** deviation-documenting caveat needed. Requirement 12.3's
Vault-export fallback design (for the case where OCI generates the key
internally) is **not needed** for the primary `sign_csr` path and is
demoted to a documented non-goal — see Conclusions.

### 4. Certificate revocation operation name — CORRECTED

Source: search-indexed OCI Python SDK reference pages
(`docs.oracle.com/en-us/iaas/tools/python/.../api/certificates_management/client/oci.certificates_management.CertificatesManagementClient.html`)
and OCI CLI command reference
(`certs-mgmt/certificate-authority-version/revoke`,
`certs-mgmt/certificate/...`).

`requirements.md`'s working assumption (`ScheduleCertificateDeletion`) was
**wrong for revocation specifically** — `ScheduleCertificateDeletion`
exists, but schedules deletion of the certificate *resource* (with a
delay), which is a different operation from revoking one *version* of a
certificate. The correct operation for `certificate_provider::revoke()`'s
semantics (matching `aws_acm_pca_provider::revoke`'s `RevokeCertificate`
analogue) is **`RevokeCertificateVersion`** (Python SDK:
`revoke_certificate_version`), which revokes a specific certificate
version by OCID + version number. A separate `RevokeCertificateAuthorityVersion`
exists for revoking a CA version itself — not applicable to this spec's
`revoke(certificate_serial)` method, which operates on issued leaf
certificates.

**Resolution**: `requirements.md`/`design.md`/`tasks.md` are corrected to
call `RevokeCertificateVersion`, not `ScheduleCertificateDeletion`.

## Not Yet Resolved (remain open — future spike or live-tenancy pass needed)

- **Instance Principal metadata-service contract** (Task 0(b)): exact
  `169.254.169.254` paths and refresh cadence for the certificate/key used
  in Instance Principal auth. Not investigated this pass; the two SDKs'
  `auth`/`instanceprincipal` packages are the next place to look (analogous
  desk-research approach to Findings 1 and 3 above), or a live OCI compute
  instance if desk research proves insufficient.
- **Freeform tag key character set** (Task 0(d)): whether hyphenated keys
  like `kythira-cluster` are valid, or some other separator is required.
  Not investigated this pass.
- **CI OIDC federation to a Dynamic Group** (Task 0(f)): whether OCI
  supports federating GitHub Actions' OIDC tokens directly, and if so the
  exact provisioning steps. Not investigated this pass; needed before
  Requirement 14.2 can be implemented as designed rather than falling back
  to a long-lived API key.
- **Signing edge cases** noted at the end of Finding 1 (multi-value
  headers, exact whitespace handling) — low risk, not exhaustively
  confirmed.

## Conclusions

- **Two of Task 0's six sub-questions are now fully resolved** (2:
  per-AD Instance Pool growth; 3: CSR support in Certificates Management),
  **one is fully resolved with a correction applied** (1: signing
  canonical string — header order corrected from `design.md`'s draft),
  and **one additional correction was found outside the original six**
  (revocation operation name).
- **The most consequential finding is #3**: this spec's `design.md` had
  flagged the CSR-support question as the single biggest architectural
  risk to `oci_certificates_provider` — if unconfirmed, it would have
  forced the private-key-leaves-OCI fallback design (Requirement 12.3),
  a real deviation from every other `certificate_provider` implementation
  in this codebase. That fallback is no longer needed. `sign_csr` is a
  direct, clean analogue of `aws_acm_pca_provider::sign_csr` after all.
- **Still not spiked**: Instance Principal's metadata contract and CI's
  OIDC federation story (Task 0(b), (f)) remain open and should be
  resolved by a follow-up desk-research or live-tenancy pass before
  Requirement 1.6 and Requirement 14.2 are implemented, respectively.
  Task 0 is therefore **partially, not fully, complete** — `tasks.md` is
  updated to reflect exactly which sub-items remain.
- **Method note**: `docs.oracle.com` blocked direct automated fetching
  (HTTP 403) for every URL attempted directly against it during this
  session, both for the general API Reference index and for specific
  Terraform-provider/CLI documentation pages. All confirmed findings above
  instead came from raw source files on `github.com`/`raw.githubusercontent.com`
  (unblocked) or from search-engine-indexed snippets of the same
  `docs.oracle.com` pages (via `WebSearch`, which does not fetch the page
  directly). A future pass with authenticated or differently-proxied
  access to `docs.oracle.com` could cross-check the SDK-source-derived
  findings above against Oracle's prose documentation directly, though
  the SDK source is arguably the stronger primary source for wire-format
  details in any case (it is what actually executes).

---

## Second pass — August 11, 2026: against a **live tenancy**

Method: an authenticated `ListRegions` from `include/raft/oci_http_client.hpp`
against `us-phoenix-1`, with the OCI CLI (`oci iam region list --debug`) as a
known-good oracle for comparison. This is the first time anything in this tree
has made a real signed request to Oracle. Everything before it was verified
against golden vectors read out of `oci-go-sdk` and against a mock that did
not check signatures.

It found two defects in code that had shipped green, and confirmed one earlier
finding far more strongly than desk research could.

### Finding 5: the endpoint domain is `{service}.{region}.oci.oraclecloud.com`

The `oci` label is **not optional**, and `host_for()` omitted it. Of the four
services these providers use, only two tolerate its absence:

| host | resolves |
|---|---|
| `identity.us-phoenix-1.oraclecloud.com` | yes |
| `iaas.us-phoenix-1.oraclecloud.com` | yes |
| `certificatesmanagement.us-phoenix-1.oraclecloud.com` | **no DNS** |
| `certificates.us-phoenix-1.oraclecloud.com` | **no DNS** |

So `oci_certificates_provider` could never have reached OCI at all — not a
subtle failure, a nonexistent hostname. `requirements.md` (1.7, 1.2's
`endpoint_override` row, the Glossary's Certificates Management entry) and
`design.md`'s canonical-form example all carried the wrong domain and are
corrected in the same pass, per Requirement 1.1.

The mock tier is structurally incapable of catching this: `endpoint_override`
replaces the entire host, so no test ever exercises the derivation.

### Finding 6: cpp-httplib appends `:443`, breaking every signature

`Host` is one of the signed headers, so a request verifies only if the `Host`
on the wire is byte-identical to the signed one. cpp-httplib builds its own in
`ClientImpl`'s constructor initializer list:

```cpp
host_and_port_(detail::make_host_and_port_string(host_, port, is_ssl())),
```

`is_ssl()` is **virtual**. Called from a base-class constructor's initializer
list it resolves to `ClientImpl::is_ssl()`, which returns `false` even when the
object being constructed is an `SSLClient`. `make_host_and_port_string` then
fails to recognise 443 as the default port and appends it. The client signed
`identity.us-phoenix-1.oci.oraclecloud.com` and sent
`identity.us-phoenix-1.oci.oraclecloud.com:443`, and Oracle answered
`401 NotAuthenticated: Failed to verify the HTTP(S) Signature` — an error that
names neither header nor cause.

Fixed by setting `Host` from the same string that is signed, which retires the
whole signs-one-thing-sends-another class rather than tracking httplib's idea
of a default port.

**No local test can reach this.** A mock server necessarily listens on a
non-default port, where both spellings agree. Confirmed by mutation: removing
the explicit `Host` fails the unit assertion on the header map and leaves the
entire mock suite green.

### Finding 7: Finding 1's canonical form is confirmed against a live signature

Much stronger evidence than the original SDK-source reading, and it exonerates
`oci_signing.hpp` completely:

- The CLI's own live `authorization` signature **verifies** against the string
  `build_signing_string()` produces — and against none of four plausible
  reorderings. `date`, `(request-target)`, `host` is right.
- Signing the CLI's exact request with `oci_signing::sign_request` yields a
  **byte-identical** base64 signature. Key loading, RSA PKCS#1 v1.5 + SHA-256,
  and the base64 encoding are all correct.
- `authorization` **parameter order does not matter**: `version="1"` first (ours)
  and last (the CLI's) both return HTTP 200 against the live service.

So neither defect was in the signing. Both were in what surrounded it — which
is precisely the gap golden vectors cannot cover.

### What this changes about testing

Task 4 had made signature verification in `oci_mock_server` explicitly
optional, reasoning that "a mock that re-derived the signature would be
verifying the client against a second copy of the client's own logic." That
reasoning is wrong in one specific way, and it is the way that mattered: a mock
can rebuild the canonical string **from the bytes that arrived** rather than
from the client's intent, and then it is answering a different question —
*does what we sent match what we signed?* Both defects lived on exactly that
axis. Verification is now on by default and every test in the tree enables it.

### Still open

Task 0(b) (Instance Principal) and 0(f) (CI OIDC federation) are untouched by
this pass. Task 0(g)'s capacity-error shape and pricing-API questions are also
still open — they need launches, and this pass deliberately created no billable
resource.
