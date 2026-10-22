# Design Document

## Overview

Two provider-neutral flags are added to the parser, one SDK-free header
validates an endpoint, and the S3 arm copies both values into the
`aws_client_config` it already builds. `aws_s3_client` needs no change: it
already honours `region` and `endpoint_override`, switches to path style
under an override, and disables IMDS there.

```
parse_backup_cli_args(argv)
  └─ backup_cli_args { ..., region, endpoint }                (Req 1.1)

main()
  ├─ provider is not s3 and (region or endpoint) set
  │     → message naming the provider's variable, exit 1       (Req 2)
  └─ run_for_provider()
        S3 arm
          ├─ endpoint set → validate_s3_endpoint(endpoint)
          │     ├─ error   → message, exit 1                   (Req 3.1, 3.2)
          │     └─ warning → stderr (plaintext, non-loopback)  (Req 3.4)
          ├─ endpoint unset and AWS_ENDPOINT_URL[_S3] set
          │     → warning naming the variable                  (Req 3.3)
          ├─ cfg.region = region; cfg.endpoint_override = endpoint
          └─ run_with_client<aws_s3_client>(cfg, ...)           (unchanged)
```

### Key design decision: flags, not environment variables

The object-backup-oci-oss-credentials spec chose environment variables for
OCI and OSS because most of what those arms read is secret, and it rejected
a region flag so that one provider would not have two ways to set one value.
S3 is different on both counts:

- Neither the region nor the endpoint is a secret, so `argv` is an acceptable
  place for them.
- S3 already has a second source for the region, the SDK chain, and this
  tree cannot remove it. The flag simply takes precedence. For the endpoint
  there is no second source at all, because the linked SDK does not read
  `AWS_ENDPOINT_URL`.
- Flags are what an operator types for a one-off restore against a local
  LocalStack or MinIO, and they appear in the shell history that documents
  what the restore did.

The OCI and OSS arms keep their environment variables, and the flags are
rejected there (Requirement 2) rather than mapped. Mapping them would give
those providers the two sources the earlier spec rejected.

### Key design decision: do not read `AWS_ENDPOINT_URL`, but say so

The obvious alternative was to honour `AWS_ENDPOINT_URL` in the tool,
matching the AWS CLI. That variable is already set to an unrelated value in
CI: `.github/actions/oci-build-cache` writes Oracle's S3-compatible endpoint
into `$GITHUB_ENV` for the whole job, and real-cloud-tests.yml has to clear
it before the AWS CLI steps. An operator's shell can carry the same kind of
leftover. Honouring it would make the target of a restore depend on ambient
state, which is the failure the explicit flag avoids. A warning keeps the
operator from being surprised in the other direction, when they expect the
variable to work because the AWS CLI honours it.

### Key design decision: path style is implied, not configurable

`aws_s3_client` already uses path style whenever `endpoint_override` is set.
LocalStack at `http://localstack:4566` and MinIO at `http://minio:9000` both
need it, because virtual hosting would ask for `<bucket>.localstack`, a name
no DNS serves. A service that needs virtual hosting under a custom endpoint
is a non-goal; if one turns up, a `--virtual-host` switch would set a new
`aws_client_config` field and leave the default alone.

One consequence is recorded rather than hidden: `--endpoint
https://s3.us-west-2.amazonaws.com` reaches real S3 path style. AWS still
serves path-style requests, so this works, but it is not how the client
addresses AWS without the flag.

### Key design decision: validate the shape of the endpoint, not its reachability

The SDK accepts almost any string as `endpointOverride` and fails on the
first request, after a connect timeout, with an error that does not mention
the flag. `validate_s3_endpoint` checks only what can be decided locally:
scheme, host, port, path, query, fragment and userinfo. It does not resolve
the host or open a connection. That keeps the rule "configuration errors are
exit 1 before any network I/O" (object-backup-oci-oss-credentials
Requirement 3) true for S3, and leaves reachability to the first request,
which already exits 2 with the SDK's message.

The scheme is required because the SDK picks one when it is missing, and
the choice between TLS and plaintext should never be implicit. A path is
rejected because the SDK keeps it and puts the bucket after it, so
`http://minio:9000/s3` would make every key start with `s3/<bucket>/`.

### Key design decision: validation lives in a header

`main.cpp` cannot be unit-tested, so the validator goes in
`include/raft/aws_endpoint_override.hpp`, takes a `std::string_view`, and has
no SDK dependency. It is named for the setting rather than for the CLI, so
`chaos_node` or any other `cmd/` binary that later grows an S3 endpoint
option can reuse it.

## Components and Interfaces

### Parser (`include/raft/object_store_backup_cli.hpp`)

```cpp
struct backup_cli_args {
    // ... existing fields ...
    /// S3 only. Empty means "let the SDK region chain decide".
    std::string region;
    /// S3 only. Empty means "AWS's own endpoints". Validated by the S3 arm,
    /// not here, because only that arm knows the rules.
    std::string endpoint;
};
```

`parse_backup_cli_args` gains two branches beside `--provider`. An empty
value (`--region ""`) is a usage error, consistent with Requirement 3.5.
Repeating a flag keeps the last value, as every existing option does.

`print_backup_cli_usage` gains two option lines:

```
  --region <name>          s3 only: the region to sign for. Default: the
                           AWS SDK chain (AWS_DEFAULT_REGION, profile, ...)
  --endpoint <url>         s3 only: an S3-compatible service, e.g.
                           http://localhost:4566. Implies path-style
                           addressing. AWS_ENDPOINT_URL is NOT read
```

The exact variable list in the region line comes from task 1.

### Endpoint validator (`include/raft/aws_endpoint_override.hpp`)

```cpp
namespace kythira {

struct endpoint_check {
    /// Empty when the endpoint is usable.
    std::string error;
    /// Non-fatal notes, e.g. plaintext to a non-loopback host.
    std::vector<std::string> warnings;
    [[nodiscard]] auto ok() const -> bool { return error.empty(); }
};

/// Checks the shape of an S3 endpoint override (Requirement 3.1). Never
/// resolves or connects. Never echoes a value that carries userinfo.
[[nodiscard]] auto validate_s3_endpoint(std::string_view endpoint) -> endpoint_check;

/// Warnings for AWS_ENDPOINT_URL / AWS_ENDPOINT_URL_S3 when they are set
/// but no --endpoint was given (Requirement 3.3). Names the variable, never
/// its value. Takes the injected lookup from env_config_result.hpp.
[[nodiscard]] auto ignored_endpoint_variable_warnings(const env_lookup& env)
    -> std::vector<std::string>;

}  // namespace kythira
```

Parsing is a small hand-written split on `://`, `@`, the first `/`, `?`
and `#`, with bracket handling for IPv6 and `std::from_chars` for the port.
A URL library would be a new dependency for six rules. A trailing `/` is
accepted and stripped from the value passed to the SDK.

The loopback test for Requirement 3.4 is textual: `localhost`, any
`127.x.x.x` literal, and `[::1]`. A name that merely resolves to loopback
gets the warning, which errs on the side of saying too much.

### S3 arm (`cmd/raft_object_backup/main.cpp`)

```cpp
if (args.provider == "s3") {
    kythira::aws_client_config cfg;
    cfg.region = args.region;
    if (!args.endpoint.empty()) {
        auto check = kythira::validate_s3_endpoint(args.endpoint);
        for (const auto& w : check.warnings) { /* "warning: ..." */ }
        if (!check.ok()) { /* message */ return 1; }
        cfg.endpoint_override = normalised(args.endpoint);
    } else {
        for (const auto& w : kythira::ignored_endpoint_variable_warnings(
                 kythira::process_env_lookup())) { /* "warning: ..." */ }
    }
    return run_with_client<kythira::aws_s3_client>(std::move(cfg), "Amazon S3",
                                                   endpoint_hint(args), args);
}
```

`endpoint_hint` passes the endpoint into `run_with_client`'s construction
failure message, so a constructor that throws says which endpoint it was
given.

### Flag rejection for other providers (`main.cpp`)

`provider_entry` gains a nullable `const char* region_variable` and
`const char* endpoint_variable`, plus a `bool takes_region_endpoint_flags`
that is true only for `s3`. `main` checks it after the compiled-in check
and before `run_for_provider`, and prints, for example:

```
raft_object_backup: --endpoint applies to --provider s3 only
  oci-objectstorage reads its endpoint from KYTHIRA_OCI_ENDPOINT_OVERRIDE
```

Azure and GCS have no equivalent variable, so their message omits the
second line.

## Error Handling

| Condition | Exit | Where |
|---|---|---|
| `--region` / `--endpoint` with no value, or empty | 1 | parser |
| Either flag with a provider other than `s3` | 1 | `main` |
| Endpoint fails a shape rule | 1 | S3 arm |
| Endpoint unreachable, TLS failure, wrong region (301) | 2 | first request, SDK message |
| `AWS_ENDPOINT_URL[_S3]` set without `--endpoint` | warning only | S3 arm |
| `http` to a non-loopback host | warning only | S3 arm |

## Testing Strategy

### Unit (`tests/object_store_backup_cli_unit_test.cpp`, new cases)

Parsing: both flags present, absent (empty fields), missing value, empty
value, and last-wins on repetition.

### Unit (`tests/aws_endpoint_override_unit_test.cpp`, new, SDK-free)

Accepted: `http://localhost:4566`, `https://minio.internal:9000/`,
`http://127.0.0.1:9000`, `http://[::1]:4566`, `HTTPS://s3.example.com`.
Rejected, each with its own rule in the message: no scheme
(`localhost:4566`), `ftp://`, empty host (`http://:4566`), port 0, port
65536, a non-numeric port, unbracketed IPv6, a path (`/s3`), a query, a
fragment, and userinfo. The userinfo case asserts the password text is
absent from the error. Warnings: plaintext to `minio.internal` warns;
plaintext to `localhost`, `127.0.0.1` and `[::1]` does not; `https` never
does. `ignored_endpoint_variable_warnings`: neither variable set gives no
warning; each set gives one naming it; empty counts as unset; no warning
contains the value. Registered unconditionally, since the header needs no SDK.

### Process (`tests/raft_object_backup_cli_process_test.cpp`, new cases)

Gated on `KYTHIRA_BACKUP_PROVIDER_S3`. The test starts a cpp-httplib server
on `localhost` that records each request and answers `ListObjectsV2` with an
empty `ListBucketResult`. It runs the binary under the existing scrubbed
environment plus `AWS_ACCESS_KEY_ID`, `AWS_SECRET_ACCESS_KEY` and an empty
`HOME`, so neither `~/.aws` nor IMDS is reachable.

1. `list --provider s3 --endpoint http://localhost:<port> --region
   us-east-1` exits 0; the recorded request has path `/<bucket>` with
   `list-type=2` and `Host: localhost:<port>`, and its `Authorization`
   header names `us-east-1` in the credential scope (proving `--region`
   reached the signer).
2. `--endpoint localhost:<port>` exits 1 naming the scheme rule, and the
   server records no request.
3. `--provider oci-objectstorage --endpoint http://x` exits 1 naming
   `KYTHIRA_OCI_ENDPOINT_OVERRIDE` (gated on the OCI provider too).
4. With `AWS_ENDPOINT_URL=http://secret-host.example` set **and**
   `--endpoint http://localhost:<port>` given, the run reaches the local
   server and stderr carries no `AWS_ENDPOINT_URL` warning, proving the flag
   wins and the variable is not read.

The warning itself (the variable set, no `--endpoint`) is asserted in the
unit suite through `ignored_endpoint_variable_warnings`, not here: a
process run without `--endpoint` would aim the SDK at real AWS, and CI
should not depend on how that fails.

### CI (LocalStack S3 cycle)

A step in the job that already starts LocalStack for EC2 in
`.github/workflows/ci.yml`. It starts `localstack/localstack:4.14.0` (the
pinned, token-free tag) with `SERVICES=s3`, creates two buckets with
`awslocal` or a `curl` PUT, writes a small node prefix using an existing
test fixture, then runs `raft_object_backup create`, `list`, `verify` and
`restore-clone --endpoint http://localhost:4566` and checks each exit code.
Fail, not skip, when LocalStack never becomes healthy.

The engine's conditional writes (`If-None-Match: *`, `If-Match`) must be
honoured for `restore-clone` to be meaningful. Task 7 confirms LocalStack
4.14.0 does. If it does not, the step uses MinIO (`minio/minio`, pinned by
digest), which supports conditional writes, instead.

The step uses a published port and `localhost`, so it needs no compose
network and no static IP, and runs unchanged under rootless Podman.
