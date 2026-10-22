# Implementation Plan

## Implementation Status

Spec only. No task has started.

## Overview

Add `--region` and `--endpoint` to `raft_object_backup`, validate the
endpoint in an SDK-free header, wire both into the S3 arm, reject them on
the other arms, and prove the wiring with unit, process and LocalStack
tests. `aws_s3_client` itself does not change.

## Tasks

- [ ] 1. Measure the SDK's region and endpoint sources
  - [ ] 1.1 In the linked aws-sdk-cpp, record which of `AWS_REGION`,
    `AWS_DEFAULT_REGION`, the shared profile and IMDS fill
    `S3ClientConfiguration::region` when it is left empty, and in what
    order. Measure by constructing a config under controlled environments,
    not by reading SDK docs
  - [ ] 1.2 Re-confirm that the library still carries no `AWS_ENDPOINT_URL`
    string (`strings libaws-cpp-sdk-core.a`), since a vcpkg bump could add
    it and change Requirement 3.3's wording
  - [ ] 1.3 Record both findings in design.md, and correct
    `aws_client_config::region`'s comment to match
  - _Requirements: 1.2, 4.2, 4.5_

- [ ] 2. Endpoint validator
  - [ ] 2.1 Add `include/raft/aws_endpoint_override.hpp` with
    `validate_s3_endpoint()` and `ignored_endpoint_variable_warnings()`, SDK
    free
  - [ ] 2.2 Write `tests/aws_endpoint_override_unit_test.cpp` covering the
    accepted, rejected and warning cases in the design, and register it
    unconditionally in `tests/CMakeLists.txt`
  - _Requirements: 3.1, 3.2, 3.3, 3.4, 5.1_

- [ ] 3. Parser and help text
  - [ ] 3.1 Add `region` and `endpoint` to `backup_cli_args` and their
    branches to `parse_backup_cli_args`, with empty values as usage errors
  - [ ] 3.2 Add the two option lines to `print_backup_cli_usage`, using the
    variable list from task 1
  - [ ] 3.3 Extend `tests/object_store_backup_cli_unit_test.cpp` with the
    parsing cases
  - _Requirements: 1.1, 3.5, 4.1, 5.1_

- [ ] 4. CLI wiring
  - [ ] 4.1 S3 arm: validate the endpoint, print warnings, exit 1 on a
    shape error, and copy `region` and the normalised endpoint into
    `aws_client_config`. Pass the endpoint into the construction-failure
    hint
  - [ ] 4.2 S3 arm without `--endpoint`: print
    `ignored_endpoint_variable_warnings(process_env_lookup())`
  - [ ] 4.3 Add the per-provider flag fields to `provider_entry`, reject
    either flag on every provider but `s3` in `main` before dispatch, and
    name the provider's own variable where one exists
  - [ ] 4.4 Update the S3 `credentials_hint`
  - _Requirements: 1.2, 1.3, 1.4, 1.5, 2.1, 2.2, 3.3, 4.2_

- [ ] 5. Process-level tests
  - [ ] 5.1 Add the four S3 cases in the design to
    `tests/raft_object_backup_cli_process_test.cpp`, gated on
    `KYTHIRA_BACKUP_PROVIDER_S3` (case 3 also on the OCI provider), with an
    in-process cpp-httplib server bound for `localhost`
  - [ ] 5.2 Pass the S3 provider macro and the AWS SDK link to the process
    test in `cmd/raft_object_backup/CMakeLists.txt`, and register the test
    when the S3 provider is compiled in, not only OCI or OSS
  - _Requirements: 5.2, 5.3, 5.5_

- [ ] 6. Documentation
  - [ ] 6.1 Add a LocalStack and a MinIO example to the
    `raft_object_backup` section of `doc/cloud_object_persistence.md`, and
    the S3 row of its credentials table
  - [ ] 6.2 Add a `doc/CHANGELOG.md` entry
  - _Requirements: 4.3, 4.4_

- [ ] 7. LocalStack S3 cycle in CI
  - [ ] 7.1 Confirm LocalStack 4.14.0 honours `If-None-Match: *` and
    `If-Match` on `PutObject`. If it does not, use a digest-pinned MinIO for
    this step instead, and record which was chosen and why
  - [ ] 7.2 Add a step to `.github/workflows/ci.yml` that starts the chosen
    service with a published port, creates source and destination buckets,
    seeds a node prefix, and runs `create`, `list`, `verify` and
    `restore-clone` with `--endpoint http://localhost:<port>`, checking each
    exit code. Fail on an unhealthy service rather than skip
  - [ ] 7.3 Run the step locally under rootless Podman as well as Docker
    (see the rootless-Podman sandbox recipe used by container-tests)
  - _Requirements: 5.4_

## Notes

- `AWS_ENDPOINT_URL` stays unread on purpose. CI exports it for Oracle's
  S3-compatible build cache, so honouring it would aim restores at the
  wrong cloud.
- A `--virtual-host` switch, a `--ca-file` option, and per-bucket endpoints
  are non-goals, each small enough to be its own change if needed.
- The OCI and OSS arms keep their `KYTHIRA_*_ENDPOINT_OVERRIDE` and
  `*_REGION` variables, and reject the new flags rather than map them.
