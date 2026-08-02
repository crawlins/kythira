# CoAP test flakiness — investigation record

Status as of 2026-08-02. This is a record of what was measured, what was
tried, and what turned out to be wrong. It exists because the same
investigation has now been attempted several times from analysis alone, and
each attempt produced a plausible diagnosis that the data later refuted.

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

So the tests that block CI do not reproduce under the CoAP label alone. The
conditions that produce CI's failures are still unidentified; measuring the
full suite under `ci.yml`'s own selection is the open next step.

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

1. **What conditions actually reproduce CI's failures?** Not `-L coap` on a
   GitHub runner, and not EC2 at any concurrency. The next measurement to run
   is the full suite under `ci.yml`'s selection
   (`label=all`, `exclude_labels='^(slow|performance|verbose|benchmark|docker)$'`).
   Coverage-instrumented and arm64 legs are slower than the `clang++-18 x64`
   Release configuration measured so far, and CI failures have appeared on
   those legs — a timing-sensitivity hypothesis worth testing directly.
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
