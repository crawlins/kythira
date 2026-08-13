# Spike Notes — Alibaba Cloud Services

Findings from Task 0, in the OCI spec's CONFIRMED/CORRECTED/WAS format.
Corrections are folded back into requirements.md/design.md in place; this
file is the record of *how* each fact was established, and — as important —
which facts are still only documentation-deep because no account exists.

**Account status: none.** Everything below is derived from vendor
documentation plus independent recomputation. Sub-items needing live traffic
are marked OPEN and stay open until Task 10 provisions an account.

---

## Finding 1 — V3 signing (ACS3-HMAC-SHA256): CONFIRMED, fully reproducible

The vendor's V3 documentation publishes a complete worked example, and
critically it uses a **non-masked** AccessKeySecret (`YourAccessKeySecret`),
so every stage is independently checkable. Recomputed from scratch (Python
hashlib/hmac, August 13, 2026):

| Stage | Value | Reproduced? |
|---|---|---|
| SHA-256 of the canonical request | `7ea06492…b1e259` | yes, exact |
| Final signature | `06563a9e…83c0` | yes, exact |
| Empty-body payload hash | `e3b0c442…2b855` | yes (the well-known empty SHA-256) |

So the canonical form in design.md is not a paraphrase to be confirmed
later — it is verified. `tests/alibaba_signing_unit_test.cpp` pins all three
stages plus the final `Authorization` header, and the C++ implementation
reproduces the vendor's header byte for byte.

Details worth recording because they are easy to get wrong:

- **Canonical headers carry a trailing newline**, and the SignedHeaders line
  follows the blank line that trailing newline creates. Getting this wrong
  yields a well-formed request the service rejects with no hint which side
  is wrong — the same failure class OCI's spike hit.
- **Percent-encoding is RFC 3986 unreserved-only** (`A-Za-z0-9-_.~`
  literal). The vendor's rule set is phrased as post-processing a Java
  URLEncoder result ("replace `+` with `%20`, `*` with `%2A`, `%7E` with
  `~`"); encoding correctly in the first place is equivalent and avoids
  carrying a Java artifact into C++.
- **Header names are lowercased and sorted**, which `std::map<std::string,
  std::string>` gives for free — the implementation leans on that rather
  than sorting explicitly.

## Finding 2 — OSS V4 signing: canonical form CONFIRMED, final signature OPEN

The OSS V4 documentation also publishes a worked example, and its canonical
request **hashes to the documented string-to-sign value exactly**
(`c46d9639…36eca`, reproduced independently). So the canonical structure —
including the `AdditionalHeaders` line, the `UNSIGNED-PAYLOAD` payload
constant, and the scope format `{date}/{region}/oss/aliyun_v4_request` — is
confirmed.

**CORRECTED vs. the pre-implementation draft:** the example's *final*
signature is **not** reproducible, because the vendor masked that example's
AccessKeySecret (`yourAccessKeySecret` is a placeholder, not the real input
— recomputing with it yields neither the documented signing key nor the
documented signature). This is the one place this provider's golden vectors
are weaker than the OCI provider's.

WAS: requirements.md 2.2 said golden vectors would be "captured from the
confirmation" as if the whole chain were checkable.

What the implementation does about it: the key-derivation chain follows the
documented algorithm literally (`HMAC("aliyun_v4"+SK, date)` → region →
`"oss"` → `"aliyun_v4_request"`), and the unit test pins the
*shape* — credential scope, header set, `UNSIGNED-PAYLOAD` — plus a
self-derived vector that locks the implementation against silent change.
**Live confirmation is OPEN**, and it is the single highest-value thing the
first account run should do: one successful PutObject proves the whole chain
at once. Until then, treat OSS V4 as structurally-verified, not
end-to-end-verified, and expect the first live call to be where a residual
detail surfaces.

## Finding 3 — Endpoints: PARTIALLY CONFIRMED (documentation only)

`ess.aliyuncs.com` and `sts.aliyuncs.com` are the documented central
endpoints; ECS documents regional endpoints (`ecs.{region}.aliyuncs.com`),
which is what `alibaba_http_client::host_for` implements. OSS is
`{bucket}.oss-{region}.aliyuncs.com` (virtual-host).

OPEN: the OCI spike found endpoint derivation to be the single richest
source of live-only surprises (its `iaas` vs `oci` realm split cost two
defects). Nothing here has been resolved against DNS. The mock tier cannot
see any of it — `endpoint_override` replaces the host wholesale — so this
stays a first-live-run checklist item.

## Finding 4 — CAS Private CA: WITHDRAWN (component descoped)

The spike item asking which CAS operation to use, and what EKUs it issues,
is withdrawn: the certificate provider was descoped on cost grounds (no
Alibaba CA, private or public, will be purchased). See requirements.md
Requirement 12, which also records the conditions under which the component
— and therefore this spike item — would be revived.

## Finding 5 — SDK decision: CONFIRMED no-SDK

Nothing in Findings 1–2 argues for a vendor SDK. V3 signing is ~40 lines
over OpenSSL primitives with no key parsing at all (strictly simpler than
OCI's RSA-based v1, which this repo already ships), and OSS V4 adds a
four-step HMAC chain. The fallback recorded in design.md (adopt the vcpkg
`aliyun-oss-cpp-sdk` for the data plane only) is **not** triggered, and the
cost it would have carried — a second HTTP stack, opaque write-retry
behavior beneath a durability contract — remains the reason not to.

## Finding 6 — AssumeRoleWithOIDC: OPEN

Not yet examined in detail; needed only by Task 8's CI wiring, which is
downstream of an account existing. The shape assumed by requirements.md 17.2
(an unauthenticated STS call carrying the OIDC token, role ARN and OIDC
provider ARN, returning an AccessKeyId/Secret/SecurityToken triple) is from
the vendor's overview documentation and has not been exercised.

---

## Implementation notes from wave 1 (not spike findings, but adjacent)

- **The `Host`-header rule is inherited, not rediscovered.** cpp-httplib's
  `Host` can carry `:443` (its `is_ssl()` is virtual and false during base
  construction), which breaks any scheme signing the host. Both new clients
  set `Host` explicitly to the bare hostname they signed. Nothing local
  could catch a regression here — the mock tier speaks plain http on a
  non-default port, where the two spellings agree — so the invariant is
  asserted directly in `alibaba_http_client_unit_test`'s
  `host_header_is_the_bare_signed_hostname`.
- **Listings request `encoding-type=url`.** That makes keys percent-encoded
  ASCII, which sidesteps XML entity decoding for arbitrary key bytes without
  taking an XML dependency. The listing helper throws on any structurally
  unexpected page (missing `<IsTruncated>`, truncated page with no
  continuation token, unterminated element) rather than returning a short
  list — Requirement 2.4's rule, and the one place a bug would be silent
  data loss rather than a visible failure.
