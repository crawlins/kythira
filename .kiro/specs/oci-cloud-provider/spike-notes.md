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

### Finding 5: the endpoint domain is per-service — **superseded by Finding 10**

> **Correction, later the same day.** This finding concluded that *every*
> service takes the `oci` label. That is wrong for Core Services and the error
> was not academic: it broke `GetVnic` and therefore every `provision_node`.
> The corrected matrix is Finding 10; the table below is right only about the
> two certificate services. Left in place rather than rewritten because the
> shape of the mistake matters — the `oci` form *resolves and mostly works* for
> `iaas`, which is exactly why one passing call was not evidence.


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

---

## Third pass — August 11, 2026: driving the real manager

Method: `scripts/ci-cloud-credentials/oci/oci_tenancy_check.cpp` stage 6 —
`oci_instance_pool_quorum_manager` provisioning, assessing and decommissioning
a real instance in a real Instance Pool, with the OCI CLI as an oracle whenever
a call disagreed with it.

Four more defects, each only reachable after fixing the one before it. **Every
one of them was invisible to all 31 mock tests**, and the reasons are
structural rather than a matter of coverage — listed with each finding, because
that is the reusable part.

### Finding 8: every request with a body carried two `Content-Type` headers

cpp-httplib takes the content type as a separate argument on every body-bearing
overload — there is no `Put(path, headers, body)` — and sets the header from
it. Supplying our own signed `content-type` as well put both on the wire. OCI
answers `400` with an **empty body**, so the error says nothing at all.

Fixed by withholding `content-type` from httplib's header map and letting it
own the header; the value is identical and the signature covers the value, not
the sent letter case.

*Why the mock could not see it*: cpp-httplib's **server** accepts duplicate
headers, and `get_header_value` returns the first. The POST test asserting
`content-type == application/json` passed with both present.

### Finding 9: a new instance is not taggable the moment it appears

`provision_node` polls for an instance lacking a `kythira-node-id` tag. The
instance appears in `ListInstancePoolInstances` while the pool is still
building it, and `UpdateInstance` against one in that state is refused:
`409 Conflict: instance ... is currently being modified, try again later`.
Since tagging is the next thing `provision_node` does, taking the candidate
that early fails the provision.

Fixed by requiring `lifecycleState == RUNNING` before adopting a candidate,
which also matches `aws_asg_quorum_manager` waiting for `InService`.

*Why the mock could not see it*: it materialised instances already `RUNNING`.
It now models the gap (`set_provisioning_reads`).

### Finding 10: the endpoint domain is per-service — corrects Finding 5

Probed against a live tenancy in `us-phoenix-1`:

| service | `{svc}.{region}.oraclecloud.com` | `{svc}.{region}.oci.oraclecloud.com` |
|---|---|---|
| `iaas` | **works** | 404 on `GetVnic`, `GetSubnet` |
| `identity` | works | works |
| `certificatesmanagement` | **no DNS record** | works |
| `certificates` | **no DNS record** | works |

Core Services predates the `oci` label and must not carry it; the certificate
services exist only with it. **No single template is correct.** `host_for` now
maps service to suffix explicitly.

`iaas` is the trap worth naming. The `oci` form resolves *and serves most
operations* — instances, instance pools, `GetInstance`, `UpdateInstance` all
worked — while 404ing `GetVnic`, which `provision_node` needs to return an
address at all. Finding 5 was written on the strength of calls that succeeded;
they were not evidence about the ones that had not been tried.

The diagnosis also took a wrong turn worth recording: the 404 was first read as
eventual consistency, and `private_ip_of` gained a poll to absorb it. The poll
then ran its **full 600-second budget** without ever succeeding, which is what
disproved the theory. The polling was kept — it is correct defensively and free
when the host is right — but it fixed nothing.

*Why the mock could not see it*: `endpoint_override` replaces the whole host,
so the derivation is never exercised by any test, at any coverage level.

### Finding 11: the pool must be `RUNNING` before an instance can be detached

`UpdateInstancePool` leaves the pool `SCALING`, and
`DetachInstancePoolInstance` is refused for the duration:
`409 IncorrectState: instancepool ... Must be in State 'Running'`. So a
decommission shortly after a provision fails — which is not an exotic sequence,
it is exactly what `maintain_quorum` does to replace a node.

Fixed with a bounded `await_pool_running()` before every detach, including the
failure-path cleanup.

*Why the mock could not see it*: its pool was always `RUNNING`. It now models
the window (`set_scaling_reads`).

### Also confirmed by this pass

- **The full lifecycle works.** `provision_node` → `assess_quorum` →
  `decommission_node` against a real pool: node 1 at `10.0.1.29:7000` in 175s,
  assessed live 1/1, decommissioned, and a post-run audit showing every
  instance terminated and **no leaked boot volumes**.
- **A regional subnet serves an AD-scoped placement configuration.** Instances
  launched into one and received addresses from its range.
- **Requirement 6.7's rollback was not enough.** It covers only the launch-poll
  timeout, so a failure at the tag write or the VNIC lookup left a tagged,
  running, billed instance. That happened twice for real before
  `best_effort_detach` was added; it is a deliberate step beyond what
  Requirement 6.7 specifies.
- **Transient connection failures during the provision poll are normal.** Four
  `Could not establish connection` errors appeared in one successful run and
  were absorbed by the poll loop, which treats a failed poll as non-fatal and
  bounds itself by the deadline instead.
- **Pool scale-down terminates with a lag** of a minute or two after
  `UpdateInstancePool` returns. Long enough to look like a leak if audited
  immediately.

### Finding 12: a Certificate Authority reaches its Vault key as a *resource principal*

Creating a CA needs a Dynamic Group matching
`resource.type='certificateauthority'`, and a policy granting **that** group
`use keys` / `use vaults` on the compartment holding the key. A
service-principal statement is the wrong mechanism.

The failure mode is what makes this expensive. `CreateCertificateAuthority`
**accepts the request**, and minutes later the CA lands in
`lifecycle-state: FAILED` with `lifecycle-details: Authorization failed or
requested resource not found: Key Id ...`. Nothing fails at request time, so
`--wait-for-state ACTIVE` waits out the whole timeout before showing anything.

Four attempts to get here, and the shape of the wrong turns is the useful part:

| attempt | grant | result |
|---|---|---|
| 1 | none (AES key) | `InvalidParameter: ... invalid shape` — see below |
| 2 | none (RSA key) | FAILED, key authorization |
| 3 | `Allow service certificates to use keys` + `use vaults` | FAILED, identically |
| 4 | ...plus `use key-delegate` | FAILED, identically |
| 5 | **Dynamic Group on `resource.type='certificateauthority'`** | **ACTIVE** |

`certificatesmanagement` is not a valid service principal at all, despite being
the hostname the management API is served from — OCI rejects that one
immediately with `Service {x} does not exist.`, which at least makes it cheap.

**Not isolated, and the notes say so**: attempt 5 kept the
`Allow service certificates to use keys` statement from the failing attempts.
The Dynamic Group is therefore proven *necessary*; whether the service
statement is also required was never tested separately, so it is retained in
`policies/certificates.txt` with that caveat rather than dropped on a guess.

Two smaller corrections from the same sequence:

- The CA's master key must be **RSA**. `kms management key create` accepts
  `{"algorithm":"AES","length":32}` and the CA then rejects it with
  `InvalidParameter: The encryption key with the OCID ... has an invalid
  shape.` — which never mentions the algorithm. The working shape is
  `{"algorithm":"RSA","length":256}`; `length` is in **bytes**, so 256 is
  RSA-2048.
- The CLI subcommand is `create-root-ca-by-generating-config-details`. The CLI
  does suggest the right name, but only after the vault and key it depends on
  already exist.

### Stage 5 confirmed: the `certificates` retrieval plane works

With an ACTIVE CA, `GetCertificateAuthorityBundle` against
`certificates.us-phoenix-1.oci.oraclecloud.com` returns the real bundle. That
closes the last unverified path in `oci_certificates_provider`'s host
derivation — Finding 10's table is now confirmed for all four services by a
successful call rather than by DNS resolution alone.

Every stage of `oci_tenancy_check` now passes against a live tenancy: signing,
compartment access, `GetInstancePool`, `ListInstancePoolInstances`,
`GetCertificateAuthorityBundle`, and the full provision/assess/decommission
lifecycle.

---

## Fourth pass — August 11, 2026: closing Task 0(b), (d), (f) and (g)

Method: empirical probes against the live tenancy where one was possible, and
Oracle's own SDK source otherwise. **All four remaining sub-questions are now
answered, and three of the four contradict what the spec assumed.**

### Finding 13 — 0(d): freeform tag keys DO allow the colon

Probed by writing candidate keys to a real VCN and recording accept/reject:

| key | result |
|---|---|
| `kythira-cluster` (hyphen) | accepted |
| `kythira:cluster` (**colon**) | **accepted** |
| `kythira_cluster` | accepted |
| `kythira/cluster` | accepted |
| `Kythira-Cluster` (uppercase) | accepted |
| `kythira+cluster` | accepted |
| `kythira.cluster` (dot) | **rejected** — `Invalid tags` |
| `kythira cluster` (space) | **rejected** — `Invalid tags` |

**Requirement 4.1's premise is false.** It states "OCI freeform tag *keys*
disallow the colon character `raft`/AWS tags use (`kythira:cluster`); this spec
uses a hyphen instead — confirm the exact allowed character set during Task 0's
spike and adjust if the assumption above is wrong." It was wrong:
`kythira:cluster` is accepted, and the tags could have matched the AWS names
exactly.

**The hyphens are kept anyway**, and that is a deliberate choice rather than
inertia: they are already written on live instances, the tag key is the
manager's lookup key, and renaming it is a migration — every existing managed
instance would become invisible to `find_instance`/`next_node_id` at once. The
cost of the change is real and the benefit is cosmetic. What changes is the
*justification*: Requirement 4.1 must stop claiming the colon is disallowed.

### Finding 14 — 0(g) part 1: the capacity error, caught in the wild

Provoked by requesting `VM.Standard.A1.Flex` (4 OCPU) in each Availability
Domain. AD-1 and AD-3 launched; **AD-2 refused**:

```
status:  500
code:    InternalError
message: Out of host capacity.
```

Two things matter here, and one of them invalidates the spec's plan.

**The `code` is useless as a classifier.** Requirement 13.14 anticipated
`OutOfHostCapacity`. The real code is `InternalError` — generic, shared with
ordinary transient failures, and matching on it would treat every 500 as a
stockout. **The classifier must match the message string `Out of host
capacity.`** (note the trailing period), which is exactly the fragility
Requirement 13.14 warned about when it said this "cannot be written correctly
from inspection alone". It could not have been, and it was not.

**Per-AD stockout is confirmed, not assumed.** One AD refused while two
succeeded, at the same moment, for the same shape. That is direct evidence for
Requirement 13.13's `(shape, Availability Domain)` ladder: a shape-only ladder
would have retried into the same shortage.

Both probe instances were terminated; a post-run audit confirmed it.

### Finding 15 — 0(g) part 2: on-demand pricing is queryable, preemptible is not

`https://apexapps.oracle.com/pls/apex/cetools/api/v1/products/` is public and
unauthenticated. `?serviceCategory=Compute%20-%20Virtual%20Machine` returns the
catalog with per-currency `PAY_AS_YOU_GO` values:

```
B93113  Compute - Standard - E4 - OCPU   OCPU Per Hour  USD 0.025
B97384  Compute - Standard - E5 - OCPU   OCPU Per Hour  USD 0.03
B93297  Compute - Standard - A1 - OCPU   OCPU Per Hour  USD 0
```

So Requirement 13.12's cheapest-first ordering **can** be computed at runtime;
no hand-maintained table is needed, unlike the Azure work.

**But there is no preemptible SKU** — zero of 648 catalog entries mention it.
OCI prices preemptible as a discount off the on-demand rate rather than as a
separate product. Two consequences:

- The harness cannot *look up* a preemptible price. It must apply the
  documented discount factor. **This pass did not verify what that factor is**
  — do not write one into code from memory.
- If the discount is uniform across shapes, the preemptible cheapest-first
  ordering is **identical to the on-demand ordering**, since a constant factor
  cannot reorder a sorted list. That would make Requirement 13.12's ladder
  considerably simpler than it reads. Worth confirming before relying on it.

Note `A1` listing at USD 0 — that is presumably the Always Free allocation
rather than a true zero marginal rate, and a naive cheapest-first sort would
put it first on that basis. Combined with Finding 14 (A1 is also the shape most
likely to be out of capacity) that is a trap worth handling deliberately.

### Finding 16 — 0(b): Instance Principal needs a federation exchange

Sourced from `oci-go-sdk`'s `common/auth/instance_principal_key_provider.go`
and `federation_client.go`.

**Metadata service** — base `http://169.254.169.254/opc/v2`, overridable via
the `OCI_METADATA_BASE_URL` environment variable:

| path | content |
|---|---|
| `/instance/region` | the instance's region |
| `/identity/cert.pem` | leaf certificate |
| `/identity/key.pem` | leaf private key (no passphrase on Compute) |
| `/identity/intermediate.pem` | intermediate certificate |

**`design.md` is wrong about the rest of it.** It currently says Instance
Principal "replaces the API-key `keyId`/private-key pair with a short-lived
X.509 certificate + private key fetched from the local instance metadata
service ... the canonical-string construction and `authorization` header shape
are otherwise identical." Requests are **not** signed with the instance
certificate's key. The real flow is:

1. Read the leaf cert, leaf key and intermediate from the metadata service.
2. Generate an **ephemeral session key pair** locally.
3. Call the Auth service's X.509 federation endpoint — `region.Endpoint("auth")`
   — signing *that* request with the leaf certificate, using
   `keyId = "{tenancy}/fed-x509-sha256/{fingerprint}"`.
4. Receive a **security token**.
5. Sign ordinary API requests with the **session** private key and
   `keyId = "ST$" + token`.

So Requirement 1.6 is a larger piece of work than "swap the credentials":
there is a second service call, an ephemeral key, and a different `keyId`
scheme. The canonical string itself is unchanged, which is the one part the
design got right.

**Refresh**: renew when `securityToken == nil || !securityToken.Valid()`,
checked before every signing operation rather than on a timer. (The OAuth
variant of the same client uses a 20-minute stale window; the X.509 path
relies on the token's own validity.)

**Addendum (August 11, 2026, while implementing Requirement 1.6)** — the
remaining wire details, read out of the same `common/auth/` sources
(`federation_client.go`, `utils.go`, `jwt.go`). Still source-derived, not
live-probed: the metadata service only exists on a real instance, so unlike
Findings 5-10 none of this has been confirmed against OCI itself yet.

- **Every metadata request needs `Authorization: Bearer Oracle`** (`utils.go`
  `httpGet`). The v2 metadata service rejects requests without it — the
  original table above omitted this, and a client built from it alone would
  401 on every real instance while passing any mock that didn't enforce the
  header (the new mock does).
- The exchange is `POST {auth}/v1/x509` with body fields `certificate`,
  `publicKey`, `intermediateCertificates`, `fingerprintAlgorithm: "SHA256"`
  — certificates with PEM armor and newlines stripped, the session public
  key as base64 of its PKIX DER. No `purpose` field.
- **The federation request's signed set has no `host`**: `date
  (request-target) content-length content-type x-content-sha256`. The
  go-sdk's ordinary signer includes `host`; its federation client's
  `genericHeaders` list deliberately does not. The one place Instance
  Principal departs from the canonical form.
- The keyId fingerprint is SHA-256 over the leaf DER, formatted **lowercase
  colon-separated** (`fmt.Sprintf("% x")` + space→colon). The tenancy OCID
  comes out of the leaf certificate's subject, attribute value prefixed
  `opc-tenant:`.
- Response is `{"token": "<JWT>"}`; validity is the JWT `exp` with a
  **5-minute buffer** (`bufferTimeBeforeTokenExpiration`), and each renewal
  regenerates the session key pair (`sessionKeySupplier.Refresh()`).
- The auth host uses the **bare** domain form (`auth.{region}.
  oraclecloud.com`) — consistent with Finding 10's per-service rule; both
  spellings currently resolve to the same addresses, and the go-sdk uses the
  bare one. `/instance/region` may answer with a legacy short code (`phx`,
  `iad`, `fra`, `lhr` — the only four `StringToRegion` short-maps).

Implemented in `include/raft/oci_federation.hpp`
(`oci_federation::instance_principal_signer`), selected by `oci_http_client`
when `use_instance_principal` is set; the mock tier
(`tests/oci_federation_unit_test.cpp`) verifies both signatures
cryptographically from the wire artifacts and enforces the Bearer header.

### Finding 17 — 0(f): keyless CI federation exists, but not via Dynamic Groups

OCI IAM **Workload Identity Federation** supports exchanging a third-party OIDC
JWT — including GitHub Actions' — for a short-lived **User Principal Session
Token** (UPST) via the Token Exchange grant. The workload submits its JWT plus a
public key; OCI validates the signature, audience and authorization rules and
returns a UPST bound to that key; the workload then signs OCI API calls with the
matching private key.

**Requirement 14.2's fallback is not needed.** It said "if the spike finds no
such mechanism currently available, the fallback SHALL be a long-lived API key
stored as a CI secret ... a decision requiring explicit sign-off". The stronger
posture is available, so no sign-off is required and no long-lived key should be
stored.

**But the shape differs from what Requirement 14.2 assumed.** It describes
federation "adapted to OCI's Dynamic Group + Policy model". This is an
*identity-domain* mechanism producing a **User** Principal Session Token, so
policy is written against a user/group principal, not a dynamic group. Dynamic
Groups remain the right model for the *certificate authority* resource
principal (Finding 12) — the two mechanisms coexist and should not be confused.

An off-the-shelf GitHub Action implementing the exchange exists
(`gtrevorrow/oci-token-exchange-action`), which is worth evaluating before
writing one, though it is third-party and unvetted.

### Task 0 status after this pass

All seven sub-questions are resolved. (a), (c), (e) from the first pass;
(b), (d), (f), (g) here. Task 6 is no longer blocked on Task 0 for anything.

---

## Fifth pass — August 11, 2026: the certificates provider's first live run

Task 6's `oci_certificates_provider_real_test` against the CA built in the
fourth pass. Two of its three cases passed on the **first** attempt —
`root_certificate_pem` and `revoke` — which confirms the retrieval-plane host
and the serial-search path were right. `sign_csr` failed twice, for two
unrelated reasons.

### Finding 18: `certificateConfig.validity` makes `CreateCertificate` unparseable

Sending `certificateConfig.validity.timeOfValidityNotAfter` — an RFC 3339 stamp,
well-formed JSON — is rejected with:

```
400 InvalidParameter: Unable to process JSON input
```

A **parser**-level error that names no field, so it reads as malformed JSON
rather than as an unwanted one. Removing the field makes the byte-identical
request succeed.

`design.md`'s sketch of `sign_csr` lists only `configType`,
`issuerCertificateAuthorityId` and `csrPem`; the `validity` object was added
during implementation on the assumption it was an optional extra. It is not
optional-and-ignored, it is fatal. `config.validity` and `options.validity` are
consequently **not consulted** by `sign_csr` at all — the certificate takes the
issuing CA's validity — and that is now stated where those fields are declared.

### Finding 19: the CSR's key algorithm must match the CA's family

With the request accepted, the certificate then failed **asynchronously**:

```
lifecycleState:   FAILED
lifecycleDetails: The key algorithm is in a different algorithm family from
                  the issuing certificate authority's algorithm family.
```

The CSR was ECDSA P-256 — this project's `leaf_certificate_options` default —
against the RSA-2048 CA from Finding 12. That was a defect in the test, not in
the provider, but it exposes a **deployment constraint worth stating plainly**:

- An OCI Certificate Authority requires an **RSA** master key (Finding 12; an
  AES key is rejected outright, and OCI's CA does not accept an EC master key
  in the configuration this project uses).
- A certificate issued by it must therefore be requested with an **RSA** CSR.
- So a deployment whose nodes generate this project's *default* ECDSA keys
  **cannot use such a CA** without changing `leaf_certificate_options::algorithm`
  to `rsa_2048`/`rsa_4096`.

Nothing in the provider can detect this — the mismatch is only visible to OCI,
minutes after the request is accepted.

### The pattern across Findings 12, 18 and 19

All three fail in a way that defeats a synchronous reading:

| finding | request | failure |
|---|---|---|
| 12 (CA key access) | accepted | `FAILED` minutes later, generic authorization text |
| 18 (`validity`) | rejected | parser error naming no field |
| 19 (key family) | accepted | `FAILED` minutes later |

Two of the three surface only by polling the resource's `lifecycleDetails`
after the call returns. Any OCI code path that creates a resource and trusts
the 2xx is not actually checking anything — which is what
`oci_certificates_provider::await_active_version` does correctly, and is why it
caught Finding 19 at all.

### Finding 20: what the first least-privilege principal surfaced (the WIF CI bring-up, August 12, 2026)

Twelve dispatches from the deliberately-failing credentials step to a green
`oci` job (run 31564877239). Every prior live run had used a tenancy
administrator; the first non-admin caller — the impersonated service user
`kythira-ci-wif` via UPST — found five gaps, none reachable by any earlier
tier. The two that are OCI knowledge rather than local bugs:

- **Instance pools are resource principals on the new authorization path.**
  OCI moved pool workflows off the service-principal fallback in early
  August 2026 (announced for OKE node pools as "Prepare OKE Node Pool IAM
  Policies for an Authorization Path Update"; observed here for plain
  Compute pools). The audit log shows every call emitting paired v1/V2
  events: `UpdateInstancePoolV2` was denied 404 **deterministically**
  through three caller-policy variants up to the full
  `manage compute-management-family` aggregate, while v1 reads succeeded
  and `GetInstancePoolV2` failed only **intermittently** — a partial
  rollout masquerading as flakiness until the audit log named the paired
  events. The fix is Finding 12's shape one resource type over: a Dynamic
  Group matching the pool (`kythira-ci-pool-dg`; both `resource.type`
  spellings `instancepool`/`computeinstancepool` matched, since a wrong
  name matches nothing silently) with launch grants (`manage
  instance-family`, `read instance-configurations`, `use
  subnets`/`vnics`/`volume-family`, `read instance-images`) plus the
  tenancy-root `Oracle-Tags` tag-namespace statement — default tags are
  applied to every launched instance and the launching principal needs
  `use tag-namespaces` on a namespace no compartment policy can grant.
- **`CreateCertificate` needs the delegate grant, not just
  `manage leaf-certificate-family`.** Issuing makes the CA sign on the
  caller's behalf: `use certificate-authority-delegate`. Oracle's own
  CertificateAdmins example carries it; every read passes without it, and
  the POST answers the same 404 NotAuthorizedOrNotFound a wrong OCID gives.

The three local-side gaps, for completeness: a secret pasted with a
trailing newline (curl exit 3 — the workflow now strips whitespace), the
oci job never having had vcpkg/prefix-path plumbing (invisible while its
credentials step failed by design — "a task whose CI job cannot execute is
not complete" proved literally), and the quorum manager's constructor gate
demanding the API-key triple in token mode. The diagnostic that broke the
policy impasse was `oci audit event list` — it names the denied operation
and the principal, and it is the only place the v1/V2 pairing is visible.

Total bring-up spend: one instance-minute per successful dispatch
(~$0.0006 each); the audit step confirmed zero leaked instances after
every run.
