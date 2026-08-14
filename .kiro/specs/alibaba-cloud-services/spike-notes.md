# Spike Notes — Alibaba Cloud Services

Findings from Task 0, in the OCI spec's CONFIRMED/CORRECTED/WAS format.
Corrections are folded back into requirements.md/design.md in place; this
file is the record of *how* each fact was established, and — as important —
which facts are still only documentation-deep because no account exists.

**Account status: provisioned August 13, 2026** (account 5633986662052576, region `ap-southeast-1`, RAM user `kythira-ci-user`; OSS confirmed live, ESS/ECS activated but not yet exercised). Everything below is derived from vendor
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

## Finding 2 — OSS V4 signing: CONFIRMED LIVE (and it found a real defect)

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

**RESOLVED August 13, 2026, against the live service** (account
5633986662052576, `ap-southeast-1`, bucket `kythira-ci-5633986662052576`).
The first live `PutObject` returned **`SignatureDoesNotMatch`** — and the
defect it exposed is one no local test in this repo could have caught, which
is exactly why the spike flagged this as the highest-value first check.

**The bug: OSS V4 signs `Content-Type` when the request carries one, and the
client was not signing it.** cpp-httplib sets that header from its
body-overload argument, so it reached the wire on every PUT while the
signature covered a header set that omitted it.

This is the **exact inverse of the rule `oci_http_client` follows**, and the
inversion is the thing to remember. Both clients face the identical httplib
behaviour; the schemes differ in whether they sign the header. OCI does not
sign content-type, so there the fix is to *withhold* it from the header map
and let httplib own it. OSS does sign it, so here the same httplib behaviour
means the value must be folded *into* the signature. Copying the OCI rule
across — which is what happened — produces a client that is wrong in
precisely one line.

**How it was diagnosed, in one step:** OSS's 403 body echoes the
`CanonicalRequest` and `StringToSign` it computed. Diffing the server's
canonical request against ours showed a single extra line
(`content-type:application/octet-stream`); everything else — resource path,
header ordering, the two blank lines before the payload constant, the
credential scope — matched byte for byte. This is the repo's recurring
lesson again: **the service's own error output is the specification.** No
documentation reading would have been faster, and the vendor's masked
example had already made the golden-vector route impossible.

**Why no local tier could have caught it:** the mock servers here do not
verify signatures (the OCI mock does; this provider's is not written yet),
the unit tier asserts on header *shape* rather than on server acceptance,
and the vendor's own worked example is unreproducible because its
AccessKeySecret is masked. Live traffic was the only available oracle.
`content_type_is_signed_when_the_request_carries_one` in
`alibaba_oss_client_unit_test.cpp` now pins the behaviour, and the
forthcoming mock server must verify signatures (Requirement 16.6) so the
class of defect is catchable locally in future.

**Verified live end to end after the fix:** PutObject → GetObject
(round-trip) → GetObject on an absent key (`nullopt`, not an exception) →
paginated ListObjectsV2 → DeleteObject, all against real OSS over https with
virtual-host addressing — the configuration no local test exercises.

## Finding 7 — OSS persistence latency, measured live (August 14, 2026)

`alibaba_oss_persistence_real_test` run against the real bucket from a
developer machine to `ap-southeast-1`: **~9.6 s** for the term+vote case
(a handful of round trips) and **~34 s** for 12 appended log entries —
roughly **2-3 s per object round trip**, dominated by geography rather than
by OSS itself.

This is the figure Requirement 15.2's honesty clause wanted. It makes the
durability trade concrete: `save_current_term`/`save_voted_for` sit on the
election hot path and now cost a real WAN round trip, so a deployment using
this engine must size election timeouts accordingly — and a cluster whose
nodes are in-region will see far less, which is the configuration the engine
is actually for. Re-measure from an in-region instance before quoting a
production number; this one is an upper bound, not a representative one.

All four real cases passed, including the load-bearing durability case: a
term and vote written by one engine, read back by a FRESH engine sharing no
memory with the writer.

## Finding 8 — httplib rewrites the signed query string; harmless (verified live)

The mock-server work measured that cpp-httplib 0.27.0 does not send the
query string it was given: `ClientImpl::process_request` splits at `?`,
decodes into `Params`, and re-encodes. It leaves `, ! $ ' ( ) * ; /`
literal where Alibaba's rule percent-encodes them, and turns a space into
`+`. So the bytes signed and the bytes sent differ on **every multi-ID
`DescribeInstances`** — which is every real cluster — and on any OSS prefix
or continuation token containing `/`.

**Verified live against ECS in `ap-southeast-1`: accepted.** A two-ID
`DescribeInstances` signed with percent-encoded `,` and sent with a literal
`,` returns normally, which means Alibaba canonicalises from *parsed
parameters*, not raw query bytes — the SigV4-family behaviour. No client
change is needed and the mock is deliberately not byte-strict on the query,
since being stricter than the real service would manufacture failures.

Still unverified, and the one shape that could bite: a parameter value
containing a **space** (signed `%20`, sent as `+`). Nothing this provider
sends today contains one; an operator's `extra_tags` value is the only
route in. Worth a live check before that path is used.

## Finding 9 — ESS quorum manager, partially verified live (August 14, 2026)

Ran the three no-cost cases of `alibaba_quorum_manager_real_test` against
the live scaling group `asg-t4ne1kbdhc5xbzskizxm` in `ap-southeast-1`. All
three pass.

**Now verified against the real API:**
- `DescribeScalingGroups` — endpoint, ACS3 signature, and the response shape
  the constructor parses (the double-wrapping guess was right).
- `DescribeScalingInstances` — same, over an empty group.
- The tag-scan path that `decommission_node` uses to resolve a NodeId, and
  its idempotent resolve when no instance carries the id (Requirement 8.3).
- `assess_quorum` over a group with no kythira-tagged instances reports zero
  members, which also pins foreign/untagged exclusion against the real API.

**Still NOT verified, and this is the important half** — the group holds no
instances, so the paths that only exist when instances do were never
exercised:
- `ECS DescribeInstances` batching (the 100-ID `InstanceIds` JSON array).
  Note Finding 8 verified multi-ID *encoding* separately with fabricated
  ids, so the query-string half is settled; the response parsing is not.
- The `InService` lifecycle-state spelling, and the `Running` ECS status —
  no instance has ever been in either state under this code.
- `ModifyScalingGroup` DesiredCapacity+1 and, the riskiest single guess in
  the provider, `RemoveInstances`' capacity-decrement parameter.
- Pagination beyond a single short page.

Only the fourth case (`provision_then_decommission_a_real_instance`, which
launches a billable ECS instance) exercises any of these. Until it runs, the
manager's write path is documentation-derived — and the OSS content-type bug
is the standing reminder of what that is worth.

## Finding 10 — ESS write path: half-confirmed; blocked by account risk control

Ran `provision_then_decommission_a_real_instance` live (August 14, 2026).
It **failed**, and the failure is more informative than a pass would have
been.

**Confirmed correct — the riskiest remaining guess in the provider:**
`ModifyScalingGroup` with DesiredCapacity+1 IS the right call to trigger a
scale-out, and it *succeeded*. The activity log records "A user requests to
modify the specified scaling group, changing the Desired Capacity …
**Successful**". What failed is the scale-out activity ESS then ran.

**The failure is account-side, not ours:**
`ErrorCode: Forbidden.RiskControl` — "This operation is forbidden by Aliyun
RiskControl system." Isolated with a **free `RunInstances --DryRun true`**,
which creates nothing and was refused identically: the block is account-wide
on ECS instance creation, so it is not the scaling configuration, the image,
the instance type, the vSwitches or anything this repo controls. The console
surfaced no verification prompt, which suggests a support-side block rather
than a pending self-service step — typical of a new account with no billing
history. Remediation is account-level, not a code or config change.

**Status, August 14, 2026: payment/identity verification started by the
account owner; quoted at up to 3 business days.** That is the expected
trigger — an unverified new account is the textbook cause of this block, and
it also explains the absent console prompt (verification was pending rather
than failed). Once it completes, re-run the single case:

```sh
export KYTHIRA_ALIBABA_REGION=ap-southeast-1
export KYTHIRA_ALIBABA_SCALING_GROUP_ID=asg-t4ne1kbdhc5xbzskizxm
export KYTHIRA_ALIBABA_ACCESS_KEY_ID=... KYTHIRA_ALIBABA_ACCESS_KEY_SECRET=...
./build/tests/alibaba_quorum_manager_real_test --log_level=test_suite \
  --run_test='alibaba_quorum_manager_real/provision_then_decommission_a_real_instance'
```

Cheap pre-check that costs nothing and tells you whether the block has
lifted, without waiting ~10 minutes for a provision timeout:

```sh
aliyun --profile kythira-ci ecs RunInstances --RegionId ap-southeast-1 --DryRun true \
  --ImageId ubuntu_24_04_x64_20G_alibase_20260720.vhd --InstanceType ecs.e-c1m1.large \
  --VSwitchId vsw-t4nd0gfwgpoxkdeq48v0o --SecurityGroupId sg-t4n3lyvcd8qegudjvhvw --Amount 1
```

**Confirmed working, unexpectedly:** the capacity-rollback-on-provision-
timeout that Task 3 added *beyond* the requirements (a sibling-lesson
defensive measure, Req 7.2 mandates only the exceptional future). After the
timeout, `DesiredCapacity` and `TotalCapacity` are both back to 0 — no leak,
no lingering spend, on its first real exercise. Worth keeping.

**Still unverified, and needing an instance to exist:** `RemoveInstances`'
capacity-decrement parameter, the `InService`/`Running` state spellings, and
ECS `DescribeInstances` batch-response parsing. Re-run this single case once
risk control clears; the binary and infrastructure are already in place.

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

## Finding 6 — OIDC federation: CONFIRMED LIVE, with three corrections

Provisioned against the live account August 13, 2026 (role
`acs:ram::5633986662052576:role/kythira-ci-real-cloud-tests`, provider
`acs:ram::5633986662052576:oidc-provider/github-actions`). Three corrections
to what requirements.md 17.2 originally assumed, each learned by a failing
call rather than from documentation:

1. **CORRECTED — the product is `ims`, not `ram`.** OIDC identity-provider
   operations live under Identity Management Service.
   `aliyun ram CreateOIDCProvider` fails "not a valid api", and forcing it
   against `ram.aliyuncs.com/2015-05-01` is rejected *by the server* with
   `InvalidAction.NotFound` — so this is a genuine product split, not stale
   CLI metadata. The distinction matters: the first diagnosis (stale CLI)
   would have sent us chasing a CLI upgrade that could not have helped.
   WAS: the script called `ram CreateOIDCProvider`.
2. **CORRECTED — `Fingerprints` is mandatory**, unlike AWS's equivalent
   where the thumbprint is optional and auto-derived; omitting it fails
   `MissingFingerprints`. `provision-oidc-role.sh` computes it from the
   issuer's live TLS chain (last certificate in the presented chain, SHA-1,
   lowercase hex) rather than hardcoding a constant that a CA rotation would
   silently stale.
3. **CORRECTED — use the vendor's official action, not a hand-rolled
   exchange.** Alibaba publishes `aliyun/configure-aliyun-credentials-action`.
   The OCI job hand-rolls its UPST exchange because its only option was an
   unaudited third-party action; a first-party action inverts that trade.
   requirements.md 17.2 now specifies the official action and records why the
   OCI precedent does not transfer.

Also confirmed incidentally: trust-policy documents use `"Version": "1"`
(Alibaba's own versioning, not AWS's `"2012-10-17"` date), and custom
policies are capped at **5 versions** — so an idempotent provisioning script
must prune before `CreatePolicyVersion` or it eventually fails
`LimitExceeded`. That is the same constraint that pushed the AWS spec toward
inline policies over managed ones; here the script prunes the oldest
non-default version instead.

Not yet exercised: the *runtime* half — a real GitHub Actions job assuming
this role. That needs Task 7's suites to exist, and is the next live check
after them.

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
