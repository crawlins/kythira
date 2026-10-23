# Requirements Document

## Introduction

`cmd/raft_object_backup` (cloud-object-persistence Requirement 10.6) backs
up and restores a Raft node's object-store state. Its S3 arm builds a
default-constructed `kythira::aws_client_config{}`
(`cmd/raft_object_backup/main.cpp`, `run_for_provider()`), so both the
region and the endpoint come from the AWS SDK alone:

- **Region** comes from the SDK's default chain only. `aws_client_config`
  documents `AWS_DEFAULT_REGION`; the shared profile and, on EC2, IMDS are
  also consulted. There is no way to name the region on the command line.
- **Endpoint** cannot be set at all. The tool has no flag for it, and the
  aws-sdk-cpp build this repo links ignores `AWS_ENDPOINT_URL`: the comment
  above the "Clear the build cache's Oracle endpoint" step in
  `.github/workflows/real-cloud-tests.yml` (about line 534) records that
  `strings libaws-cpp-sdk-core.a` shows no such string.

So `raft_object_backup` cannot reach LocalStack, MinIO, or any other
S3-compatible service, and an operator on a host with no region configured
has to edit `~/.aws/config` before a restore.

The client already supports both settings. `aws_client_config` has `region`
and `endpoint_override`, and `aws_s3_client` switches to path-style
addressing and disables IMDS whenever `endpoint_override` is set
(`aws_s3_client.hpp`, "Addressing"). That path is unit-tested against a
hostname mock (`endpoint_override_uses_path_style_addressing`). The gap is
only that the CLI never fills those fields in.

This spec adds `--region` and `--endpoint` to the tool, wires them into the
S3 arm, validates them before any network call, and documents them.

### Non-goals

- **Reading `AWS_ENDPOINT_URL` / `AWS_ENDPOINT_URL_S3` in the tool.** CI's
  job environment sets `AWS_ENDPOINT_URL` to Oracle's S3-compatible build
  cache (see the workflow comment above). A tool that honoured it would
  silently send a restore to the wrong cloud. The tool warns instead
  (Requirement 3.3).
- **A flag to choose the addressing style.** Path-style whenever an endpoint
  is set is the client's existing rule, and LocalStack and MinIO both serve
  it. A `--virtual-host` switch for services that need virtual hosting under
  a custom endpoint can be its own change.
- **A custom CA bundle (`--ca-file`).** A MinIO with a private CA needs the
  CA in the system trust store for now.
- **Different endpoints for source and destination.** The tool builds one
  client per run, so one endpoint serves every bucket a verb touches.
  Copying between two S3-compatible services is out of scope.
- **Region or endpoint flags for the OCI and OSS arms.** Those arms already
  read `KYTHIRA_OCI_REGION`, `KYTHIRA_OCI_ENDPOINT_OVERRIDE`,
  `KYTHIRA_ALIBABA_REGION` and `KYTHIRA_ALIBABA_ENDPOINT_OVERRIDE`
  (object-backup-oci-oss-credentials). Giving them a second source would
  reopen the "two ways to set one value" problem that spec rejected.
- **Credential flags.** Credentials still come from the SDK chain only. The
  endpoint and region are not secrets, which is why they may be flags.

## Glossary

- **S3 arm**: the `--provider s3` branch of `run_for_provider()` in
  `cmd/raft_object_backup/main.cpp`.
- **S3-compatible service**: any server speaking the S3 REST API at an
  address other than AWS's, such as LocalStack or MinIO.
- **SDK region chain**: wherever aws-sdk-cpp finds a region when
  `ClientConfiguration::region` is left empty. Which variables it reads in
  the linked version is measured by task 1, not assumed.
- **Configuration error**: an error the operator fixes by retyping the
  command. The CLI reports it with exit code 1, like a usage error.

## Requirements

### Requirement 1: `--region` and `--endpoint` flags

**User Story:** As an operator, I want to name the S3 region and endpoint on
the command line, so that I can restore from LocalStack, MinIO or a specific
AWS region without editing SDK configuration files.

#### Acceptance Criteria

1. `parse_backup_cli_args` SHALL accept `--region <name>` and
   `--endpoint <url>` for every verb and store them in two new
   `backup_cli_args` fields. A flag given without a value SHALL be a usage
   error, as for every other option.
2. WHEN `--region` is given THEN the S3 arm SHALL set
   `aws_client_config::region` to it. WHEN it is absent THEN the region SHALL
   come from the SDK region chain, exactly as today.
3. WHEN `--endpoint` is given THEN the S3 arm SHALL set
   `aws_client_config::endpoint_override` to it, so the client uses
   path-style addressing and skips IMDS. WHEN it is absent THEN the client
   SHALL use AWS's regional endpoints with virtual-host addressing, exactly
   as today.
4. THE two flags SHALL be independent. `--endpoint` without `--region` SHALL
   sign with whatever region the SDK region chain yields (LocalStack and a
   default MinIO accept any region). `--region` without `--endpoint` SHALL
   target that AWS region.
5. A tool run with neither flag SHALL behave exactly as before this spec.

### Requirement 2: Flags on providers that do not take them

**User Story:** As an operator, I want a clear error if I pass `--endpoint`
to a provider that ignores it, so that I never believe I am talking to one
service while talking to another.

#### Acceptance Criteria

1. WHEN `--region` or `--endpoint` is given with a provider other than `s3`
   THEN the CLI SHALL exit 1 before constructing a client, naming the flag,
   the provider, and that provider's own variable for the same setting where
   one exists (`KYTHIRA_OCI_REGION`, `KYTHIRA_OCI_ENDPOINT_OVERRIDE`,
   `KYTHIRA_ALIBABA_REGION`, `KYTHIRA_ALIBABA_ENDPOINT_OVERRIDE`).
2. THE flags SHALL NOT be silently ignored by any arm.

### Requirement 3: Validation before any network call

**User Story:** As an operator in an outage, I want a mistyped endpoint
rejected at once with a reason, not after a connect timeout.

#### Acceptance Criteria

1. THE S3 arm SHALL validate `--endpoint` before constructing the client. A
   valid value SHALL have:
   - a scheme of `http` or `https` (case-insensitive), which is required: the
     SDK's scheme default would otherwise decide between TLS and plaintext
     silently;
   - a non-empty host: a DNS name, an IPv4 literal, or a bracketed IPv6
     literal;
   - an optional port from 1 to 65535;
   - no path other than an optional single `/`, no query and no fragment.
     The client appends the bucket to the path, so anything else would be
     sent as part of every key;
   - no userinfo (`user:pass@`). Credentials come from the SDK chain only,
     and a secret in `argv` is visible through `ps`.
2. WHEN validation fails THEN the CLI SHALL exit 1 with one message naming
   `--endpoint` and the rule broken. The message SHALL echo the value except
   when it contains userinfo, in which case it SHALL say so without printing
   the value.
3. WHEN `--endpoint` is absent and `AWS_ENDPOINT_URL` or
   `AWS_ENDPOINT_URL_S3` is set and non-empty THEN the S3 arm SHALL print a
   warning on stderr that the variable is ignored and `--endpoint` is the way
   to set one. This SHALL NOT be an error and SHALL NOT change the target.
   The warning SHALL name the variable and SHALL NOT print its value.
4. WHEN the endpoint scheme is `http` and the host is not a loopback address
   or `localhost` THEN the S3 arm SHALL print a warning on stderr that object
   data will travel unencrypted. This SHALL NOT be an error: a LocalStack or
   MinIO on a private network is a legitimate target.
5. `--region` SHALL be checked for presence only: an empty value is a usage
   error, and its spelling is not checked. AWS validates real regions on the
   first request, and MinIO accepts arbitrary region names, so a local
   spelling check would reject valid configurations.

### Requirement 4: Help text and documentation

**User Story:** As an operator, I want `--help` and the runbook to show how
to reach an S3-compatible service, so that I do not have to read source
during a restore.

#### Acceptance Criteria

1. `raft_object_backup --help` SHALL list `--region` and `--endpoint` under
   options, saying they apply to `--provider s3` only and that an endpoint
   implies path-style addressing.
2. THE S3 line of the credentials section SHALL say that the region comes
   from `--region` or the SDK chain, and that `AWS_ENDPOINT_URL` is not read.
3. `doc/cloud_object_persistence.md` SHALL gain a short LocalStack and MinIO
   example under the `raft_object_backup` section.
4. `doc/CHANGELOG.md` SHALL gain an entry.
5. `aws_client_config::region`'s comment SHALL name the variables task 1
   finds the SDK actually reads.

### Requirement 5: Testing

**User Story:** As a maintainer, I want tests that prove the flags reach the
wire, so that the S3 arm cannot quietly fall back to AWS again.

#### Acceptance Criteria

1. Unit tests SHALL cover parsing of both flags (present, absent, missing
   value) and every endpoint validation rule in 3.1, including that a
   userinfo value never appears in the error text.
2. A process-level test SHALL run the built binary with `--provider s3
   --endpoint http://localhost:<port>` against an in-process HTTP server and
   assert that a `list` request arrives path-style (`/<bucket>?list-type=2`
   with `Host: localhost:<port>`) and that the run exits 0. It SHALL use the
   hostname `localhost`, not `127.0.0.1`, because the SDK falls back to path
   style for IP literals on its own (`aws_s3_client.hpp`, "Addressing"), so
   an IP-only test would still pass if path-style addressing were lost.
3. THE process-level test SHALL also cover: a bad endpoint exits 1 before any
   connection; `--endpoint` with `--provider oci-objectstorage` exits 1
   naming `KYTHIRA_OCI_ENDPOINT_OVERRIDE`; and with `AWS_ENDPOINT_URL` set,
   `--endpoint` still wins and no warning is printed. The warning for the
   variable set without `--endpoint` SHALL be unit-tested through an
   injected environment, never by letting a test run reach real AWS.
4. A CI step SHALL run a full `create`, `list`, `verify` and `restore-clone`
   cycle against LocalStack's S3 service through `--endpoint`, failing (not
   skipping) when LocalStack is unreachable, in the same way as the existing
   EC2 LocalStack step in `.github/workflows/ci.yml`. It SHALL follow
   CLAUDE.md's container rules: no static IPs, no privileged mode, and it
   must run under rootless Podman as well as Docker.
5. Tests in 5.1 to 5.3 SHALL need no cloud account and SHALL be registered
   with CTest only when the S3 provider is compiled in.
