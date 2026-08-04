# CoAP test flakiness — investigation record

**Status: root cause identified and fixed** (2026-08-03, PR #140). This is a
record of what was measured, what was tried, and what turned out to be wrong.
It exists because the same investigation had been attempted several times from
analysis alone, and each attempt produced a plausible diagnosis that the data
later refuted.

**Summary for anyone arriving fresh:** the coap tests were never unstable.
Their per-case Boost timeouts were sized for a Release build, and CI's Coverage
job runs an instrumented Debug build several times slower, so budgets expired
mid-test and cases died with SIGALRM. See [The resolution](#the-resolution).

Read the [Findings](#findings) before proposing a fix, and the
[What was tried and failed](#what-was-tried-and-failed) section before
repeating an approach.

## The problem

CI runs fail intermittently on `coap_*` tests, on a rotating subset, and this
blocks merges of changes that touch no CoAP code. Over one session three
separate PRs went red on different subsets:

| PR | Contents | Failed on |
|---|---|---|
| #114 | `http_transport_impl.hpp` + docs | `coap_concept_conformance`, `coap_confirmable_message`, `coap_cbor_end_to_end`, `coap_duplicate_detection` |
| #130 | GCP backends | `coap_concurrent_processing`, `coap_confirmable_message`, `coap_connection_reuse` |
| #134 | **one new workflow file, no code** | `coap_confirmable_message`, `coap_connection_reuse`, `coap_duplicate_detection`, `coap_cbor_end_to_end` |

#134 is the control: it adds a single dispatch-only workflow and changes no
code, yet fails on CoAP tests. Whatever this is, it is independent of what is
being changed.

Repo-wide over that period, the last 15 CI runs were 9 failure / 3 success /
3 cancelled — roughly a 20% pass rate.

Note that CI runs `ctest --repeat until-pass:3`, so any test it reports as
failed has already failed three times consecutively.

## The resolution

Fixed in [PR #140](https://github.com/crawlins/kythira/pull/140). Two changes,
both driven by the measurements below:

1. **`KYTHIRA_TEST_TIMEOUT_SCALE`** (`tests/test_timeout_scale.hpp`) scales both
   the Boost per-case budgets and the CTest `TIMEOUT` properties. `ci.yml`'s
   Coverage job passes 4; every other build defaults to 1 and is behaviourally
   identical. One knob drives both, because scaling either alone just moves the
   kill from Boost to CTest.
2. **Numeric endpoints** in the coap tests, so requests skip `getaddrinfo()`.
   `coap_connection_reuse_property_test` used `node1/2/3.example.com` — names
   that do not exist, under a real delegated domain — and `send_rpc()` resolves
   inside its recursive mutex, so N requests were N serialised live NXDOMAIN
   lookups.

Measured under the coverage profile, same runner and selection, no `--repeat`:

| test | before | after |
|---|---:|---:|
| `coap_duplicate_detection_property_test` | 80% | **0%** |
| `coap_concurrent_processing_property_test` | 70% | **0%** |
| `coap_connection_reuse_property_test` | 70% | **0%** |
| `coap_confirmable_message_property_test` | 60% | **0%** |
| `coap_get_joined_multicast_groups_test` | 40% | **0%** |
| `coap_content_format_property_test` | 40% | **0%** |
| `coap_cbor_end_to_end_test` | 30% | **0%** |
| `coap_future_resolution_property_test` | 30% | **0%** |
| `coap_thread_safety_property_test` | 20% | **0%** |
| `coap_concept_conformance_test` | 10% | **0%** |

(before n=10, after n=8.)

Two things worth carrying forward:

- **The scaling alone was not sufficient.** With the 4x scale but the hostnames
  still in place, `connection_reuse` remained at 33%, failing at exactly
  240.04s — `timeout(60) x 4` to the centisecond. An intermediate commit raised
  that file's budgets to 120s to cover it; once the DNS cost was removed at
  source, that bump was reverted rather than left in. Widening a budget to pay
  for a cost you can delete is how the four commits in
  [Prior attempts](#prior-attempts-for-context) came about.
- **`connection_oriented_example_test` showed 1 failure in 8** after runs where
  it had none before. At n=8 that is a single event and most likely noise, but
  it is recorded rather than dropped for not fitting.

## Findings

Measured with `.github/workflows/coap-flake-measure.yml` (added in #134),
20 iterations per configuration, on GitHub-hosted `ubuntu-24.04` runners.

### 1. The CoAP suite is largely stable in isolation

`main` @ `44dbb13`, `-L coap -j$(nproc)`, 20 runs of 26 tests:

| Test | Failures | Rate |
|---|---:|---:|
| `coap_thread_safety_property_test` | 2/20 | 10% |

Nothing else failed once. In particular `coap_connection_reuse_property_test`
and `coap_cbor_end_to_end_test` — both of which have failed real CI runs —
failed **zero times in 20 runs**.

So the tests that block CI do not reproduce under the CoAP label alone.

### 1b. Nor do they reproduce under the full suite, in a Release build

`main`, `label=all` with `ci.yml`'s own `-LE
'^(slow|performance|verbose|benchmark|docker)$'`, 12 complete runs of **401**
tests, `-j$(nproc)`, `clang++-18` Release:

| Test | Failures | Rate |
|---|---:|---:|
| `ca_cluster_node_rpc_tls_restart_test` | 1/12 | 8% |
| `coap_dtls_connection_establishment_property_test` | 1/12 | 8% |
| `integration_test` | 1/12 | 8% |

None of the tests that block CI appear at all. Note this configuration omits
`--repeat until-pass:3`, so it should surface *more* flakiness than CI, not
less.

(The run was cancelled at the 240-minute job limit after 13 of 20 iterations;
12 parsed. Use 10 iterations for a full-suite measurement.)

### 1c. The remaining uncontrolled variable is the build profile

Every measurement so far used `-DCMAKE_BUILD_TYPE=Release` — the fastest
configuration. `ci.yml`'s Coverage job builds `-DENABLE_COVERAGE=ON
-DCMAKE_BUILD_TYPE=Debug`: instrumented *and* Debug, so substantially slower.

Across the CI failures observed in one session:

| PR | Failing jobs |
|---|---|
| #130 | **Coverage**, clang x64 |
| #134 | **Coverage**, g++ x64, Proxygen |
| #135 | clang x64, g++ x64 |
| #137 | **Coverage**, clang x64 |
| #132 | **Coverage** |

Coverage appears in four of five. CoAP tests are timing-sensitive by
construction — retransmission schedules, ACK timeouts, SIGALRM limits — so a
build that runs several times slower is a plausible cause in a way that the
test logic is not.

**Leading hypothesis: the failures are specific to slow build configurations,
not to the CoAP tests being inherently unstable.** This is consistent with
every negative result above, and with why four separate "raise the timeout"
commits each half-worked.

If it holds, the fix is not in the tests at all: it is either the Coverage
job's timeout budget or CTest `TIMEOUT` values scaled for instrumented builds.
The `profile: coverage` input on the measurement job exists to test exactly
this — dispatch it against `main` and compare with the Release numbers above.

### 2. Environment fidelity matters more than expected

The same measurement was first run on an EC2 `m7i.8xlarge` rather than a
GitHub runner. It produced a completely different failure set:

| Test | EC2, `-j32` | EC2, `-j1` | GitHub runner, `-j` |
|---|---:|---:|---:|
| `coap_thread_safety_property_test` | 90% | 100% | 10% |
| `coap_get_joined_multicast_groups_test` | 70% | 25% | 0% |
| `coap_duplicate_detection_property_test` | 60% | 25% | 0% |
| `coap_confirmable_message_property_test` | 60% | 0% | 0% |

The EC2 numbers are not a proxy for CI and should not be used as one. They do
show that contention roughly triples the failure rate for three of these
tests, while `coap_thread_safety_property_test` is unaffected by concurrency —
it fails deterministically there for a different reason (see Finding 4).

### 3. `-L coap` is harsher than CI, not equivalent

Running only the CoAP label packs CoAP tests onto every `-j` slot, so port and
socket contention is far denser than `ci.yml`'s mixed workload. This is useful
as a stress configuration but must not be mistaken for reproducing CI.

### 4. `coap_thread_safety_property_test` has a real crash, not a flake

On EC2 it failed 4/4 serially with:

```
test_concurrent_rpc_requests:          SIGALRM (timeout while executing function)
test_concurrent_configuration_checks:  memory access violation at address 0x1ac:
                                       no mapping at fault address
```

immediately after libcoap logs `coap_pdu_encode_header: unsupported protocol`.
The small fault address indicates a null-pointer-plus-field dereference, not
heap corruption. On GitHub runners the same test fails 10% of the time, where
`until-pass:3` masks it almost entirely.

This is a genuine bug and is tracked separately from the flakiness work. It is
*not* the cause of the CI failures above — those tests are different.

Update after PR #140: this test also went from 20% to 0% under coverage, which
suggests the crash is *triggered* by timing pressure rather than caused by it.
That makes it harder to reproduce on CI, and means reproducing it deliberately
(EC2, or a deliberately low `KYTHIRA_TEST_TIMEOUT_SCALE`) is now the practical
route to investigating it.

## What was tried and failed

PR #133 (closed) attempted three changes. A matched before/after measurement —
20 iterations each, same selection, same runner class — showed it made things
substantially **worse**:

| Test | `main` | #133 |
|---|---:|---:|
| `coap_duplicate_detection_property_test` | 0/20 | **15/20 (75%)** |
| `coap_confirmable_message_property_test` | 0/20 | **11/20 (55%)** |
| `coap_thread_safety_property_test` | 2/20 (10%) | **10/20 (50%)** |

Every regression was in a file it modified. The causes are worth recording so
they are not repeated:

**Bounding retransmission broke the tests' premise.** The change reduced
`max_retransmit` from 4 to 1 for tests that point a client at an endpoint with
no listener, reasoning that the ~62s retransmission schedule was incidental
overhead racing the test timeout. It is not incidental:
`coap_duplicate_detection_property_test` detects duplicates, and the duplicates
come *from* retransmissions — cutting the count removed the very thing under
test. `coap_confirmable_message_property_test` has the same shape.

The general lesson: before changing a test's configuration to make it faster or
more deterministic, check whether that configuration *is the subject of the
test*. Checking its assertions is not sufficient — those passed.

**The RAII thread-join change tracked a regression too**
(`coap_thread_safety_property_test`, 2/20 → 10/20). The reasoning behind it
still looks sound in isolation — a Boost timeout unwinding a case with
joinable `std::thread`s calls `std::terminate()`, aborting the binary rather
than failing one case, which `bc39d04` documents in its own comments — but the
measured effect was negative and the mechanism was never established.

**The diagnosis was aimed at the wrong target throughout.** It was built on
the assumption that the tests failing CI were inherently unstable. Finding 1
shows they are not, under the label they were measured in.

## Prior attempts, for context

Four commits in one month adjusted timeouts or iteration counts, none with a
measurement showing the change helped:

- `92d824b` raise timeouts for coap tests slowed by OpenSSL init cost
- `bc39d04` shrink concurrent send counts in coap connection-reuse test
- `9727d38` reduce iteration counts, raise timeouts for slow CI runners
- `5a9c5ff` recalibrate two more coap property tests for real async I/O

`bc39d04` reduced `test_concurrent_request_handling_property` from 10×20 = 200
real sends to 4×3 = 12, and raised its timeout from 60s to 90s. That is a
large coverage reduction accepted in exchange for stability that did not
arrive. If the underlying problem is fixed, this is worth revisiting.

## Method notes

Two failure modes of the measurement job itself, both of which produced output
that resembled a clean result:

- **JUnit written to the wrong directory** (fixed in #135). `ctest
  --output-junit` resolves relative to `--test-dir`, while a shell `>` redirect
  resolves relative to CWD, so the XML landed outside the uploaded artifact and
  the summary reported `Completed runs parsed: 0` — indistinguishable from "no
  failures".
- **An empty `label` input does not clear the label filter** (fixed in #136).
  GitHub substitutes an input's declared default whenever the supplied value is
  empty, so `-f label=""` silently ran `-L coap` — 26 tests rather than the 401
  `ci.yml` runs. Use the literal `all`.

Both are worth remembering generally: an instrument whose empty output looks
like a passing result will mislead quietly.

## Open questions

1. ~~**Does the coverage profile reproduce CI's failures?**~~ **Answered: yes.**
   Under `profile=coverage` the blocking tests failed at 100/90/80/60%, against
   0% for the same tests in Release on the same runner and selection. That
   confirmed build-configuration timing as the cause and put the fix in timeout
   budgets rather than test logic. See [The resolution](#the-resolution).
   **Update (2026-08-04): the same problem existed on g++ Release, and the
   original fix did not cover it.** PR #140 applied the scale only in
   `ci.yml`'s Coverage job, leaving the four `Build & Test` legs at scale 1.
   Measured on g++-13 x64 Release, 9 runs of the full suite:

   | test | clang Release | g++ Release |
   |---|---:|---:|
   | `coap_confirmable_message_property_test` | 0% | **89%** |
   | `coap_duplicate_detection_property_test` | 0% | **89%** |
   | `coap_connection_reuse_property_test` | 0% | **67%** |
   | `coap_concurrent_processing_property_test` | 0% | **67%** |
   | `coap_future_resolution_property_test` | 0% | **56%** |

   Same mechanism: g++ Release runs these tests 1.3-5.0x slower than clang
   (median 1.4x), and the budgets were implicitly sized against clang, the
   fastest configuration in the matrix. The scale is now applied to every
   `Build & Test` leg, not only Coverage.

   This is why three consecutive PRs -- #144, #145, #146, two of which changed
   no test code at all -- failed on `Build & Test (g++-13, x64)` while `main`
   passed: the leg was genuinely broken, and only ever passed by luck.

   **arm64 remains untested.** It is covered by the same 4x, but whether that
   is sufficient on those slower runners has not been measured.
2. **What is null at `coap_thread_safety_property_test.cpp:309`,** and is
   `coap_pdu_encode_header: unsupported protocol` cause or symptom?
3. **Is the ephemeral-port migration worth finishing?** `7edf99b` introduced
   `coap_server::bound_port()` for exactly this class of problem, but only 2 of
   37 CoAP test files use it; six still hardcode 5683 and two share 19683.
   Under `-j` that is a real collision hazard — but Finding 1 shows it is not
   what is failing CI, so it should be justified on its own merits rather than
   as a flakiness fix.

## Working rule

No change to a CoAP test timeout or iteration count without a before/after
failure rate from `.github/workflows/coap-flake-measure.yml`, gathered on the
same runner class and selection. At 20 iterations a shift of one or two runs is
inside binomial noise; treat it as no change.

This rule is what caught #133. Without it, a change that tripled the failure
rate of three tests would have been merged as a stability fix.
