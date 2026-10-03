# Implementation Plan

## Implementation Status

Tasks 1-8 done (October 3, 2026). Task 9, the manual live check, is open: it
needs the real-suite OCI and Alibaba credentials, which the cloud sandbox
does not have.

Two choices made during implementation:

- The process-level test is registered from
  `cmd/raft_object_backup/CMakeLists.txt`, not `tests/CMakeLists.txt`.
  `tests/` is configured before `cmd/raft_object_backup/`, and only the latter
  knows which providers the binary carries.
- `alibaba_mock_server::set_security_token` now enforces the token on OSS
  requests, as its comment already said it did. Without that, task 6.2's STS
  case would pass whether or not the client sent the token.

## Overview

Add SDK-free environment-to-config mapping for OCI and Alibaba, wire it into
the `raft_object_backup` OCI and OSS arms, make every client-construction
failure exit 2, and document the variables in `--help` and the runbook.

## Tasks

- [x] 1. Shared result type
  - [x] 1.1 Add `include/raft/env_config_result.hpp` with `env_lookup`,
    `process_env_lookup()` (empty values read as unset) and
    `env_config_result<Config>`
  - _Requirements: 3.2, 3.3_

- [x] 2. OCI mapping
  - [x] 2.1 Move `oci_object_storage_config` into
    `include/raft/oci_object_storage_config.hpp` and include it from
    `oci_object_storage_client.hpp`. No behaviour change
  - [x] 2.2 Add `include/raft/oci_client_config_env.hpp` with
    `oci_client_config_from_env()` and `oci_object_storage_config_from_env()`:
    mode selection and inference, the key-source conflict, the key-file read,
    per-mode missing-variable collection, and instance-principal warnings
  - _Requirements: 1.1, 1.2, 1.3, 1.4, 1.5, 1.6, 3.1, 3.4, 3.5, 3.6_

- [x] 3. OSS mapping
  - [x] 3.1 Add `include/raft/alibaba_client_config_env.hpp` with
    `alibaba_client_config_from_env()`, including the `STS.` warning
  - _Requirements: 2.1, 2.2, 3.1, 3.4_

- [x] 4. Unit tests
  - [x] 4.1 Write `tests/cloud_config_env_unit_test.cpp` covering the 14
    cases in the design's Testing Strategy
  - [x] 4.2 Register it unconditionally in `tests/CMakeLists.txt` beside
    `object_store_backup_cli_unit_test`
  - _Requirements: 6.1, 6.4_

- [x] 5. CLI wiring
  - [x] 5.1 OCI arm: build the config from `process_env_lookup()`, report
    warnings and errors, exit 1 on errors
  - [x] 5.2 OSS arm: the same
  - [x] 5.3 Construct every arm's client inside a `try` that prints and
    exits 2, with the `KYTHIRA_OCI_NAMESPACE` hint on the OCI arm when the
    namespace was unset
  - [x] 5.4 Wrap `run_for_provider()` in `main` with a final
    `catch (const std::exception&)` that exits 2
  - _Requirements: 3.1, 3.2, 4.1, 4.2, 4.3_

- [x] 6. Mock round trips
  - [x] 6.1 Extend `oci_object_storage_mock_conformance_test` with
    env-built configs: inline key with and without `KYTHIRA_OCI_NAMESPACE`,
    and the key-file source
  - [x] 6.2 Extend `alibaba_oss_persistence_mock_test` with env-built configs:
    a static key, and an STS token
  - _Requirements: 6.2, 6.4_

- [x] 7. Process-level CLI test
  - [x] 7.1 Write `tests/raft_object_backup_cli_process_test.cpp`. It runs the
    binary from `$<TARGET_FILE:raft_object_backup>` under a scrubbed
    environment and covers the four cases in the design
  - [x] 7.2 Register it only when the OCI or OSS backup provider is compiled
    in, and gate each case on its provider
  - _Requirements: 4.1, 5.1, 6.3, 6.4_

- [x] 8. Help text and documentation
  - [x] 8.1 Add `credentials_hint` to `provider_entry` and a credentials
    section to `print_backup_cli_usage`, listing compiled-in providers only;
    keep existing callers compiling
  - [x] 8.2 Replace the "Credentials come from wherever..." paragraph in
    `doc/cloud_object_persistence.md` with a per-provider table, and say
    why secrets come only from the environment
  - [x] 8.3 Add a note to cloud-object-persistence task 14 pointing to this
    spec
  - [x] 8.4 Add a `doc/CHANGELOG.md` entry
  - _Requirements: 5.1, 5.2, 5.3_

- [ ] 9. Manual live check
  - [ ] 9.1 With the real-suite environment (`KYTHIRA_OCI_*`,
    `KYTHIRA_ALIBABA_*`), run `raft_object_backup list` against
    `KYTHIRA_OCI_OBJECT_BUCKET` and `KYTHIRA_ALIBABA_OSS_BUCKET`, and record
    the result here. Read-only; no new IAM policy needed
  - _Requirements: 1.1, 2.1_

## Notes

- `~/.oci/config` profiles, `ALIBABA_CLOUD_*` names, credential refresh and
  Alibaba RAM-role credentials are non-goals. Each can be its own spec.
- The new headers are deliberately named for the config they build and not
  for the CLI, so a future `cmd/` binary that constructs an OCI or Alibaba
  component (parity audit Q17) can reuse them.
