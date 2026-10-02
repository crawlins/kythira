# Requirements Document

## Introduction

`cmd/raft_object_backup` (cloud-object-persistence Requirement 10.6, task 14)
is the tool operators use to back up and restore a Raft node's object-store
state when they have no compiler to hand. It supports five providers. Three of
them can authenticate today; two cannot.

- **S3** builds `aws_client_config{}` and the AWS SDK's default chain supplies
  credentials.
- **Azure Blob** reads `KYTHIRA_AZURE_STORAGE_ACCOUNT` and the Azure SDK
  supplies credentials.
- **GCS** reads `GOOGLE_CLOUD_PROJECT` and Application Default Credentials
  supply the rest.
- **OCI Object Storage** builds a default-constructed
  `oci_object_storage_config` (`cmd/raft_object_backup/main.cpp:172`). Region,
  tenancy, user, fingerprint and key are all empty. OCI has no SDK in this
  tree (`oci_client_config.hpp`), so nothing else fills them in.
- **Alibaba OSS** builds a default-constructed `alibaba_client_config`
  (`main.cpp:179`). Every request fails with `alibaba_oss_client:
  access_key_id is empty` (`alibaba_oss_client.hpp:350`).

Neither config struct reads the environment, by design: both headers say that
sourcing values is the caller's job. `raft_object_backup` is the caller and
never does it. The CLI unit test covers argument parsing and a mock-store
cycle only, so nothing caught this. The parity audit recorded it as S1
(`audits/parallel-implementation-parity-audit.md`), and it was re-verified on
`main` at `9c738d2`.

The re-verification also found a second defect on the same path. The OCI
client resolves the tenancy namespace **in its constructor** (`GET /n/`,
`oci_object_storage_client.hpp:250-252`). `main.cpp` constructs the client
outside `run_backup_cli`'s `try`, and `main` has no handler of its own. Any
failure there, including the missing credentials above, escapes `main` and
ends in `std::terminate`. The process dies by `SIGABRT` (a shell reports
134) instead of exiting with the documented exit 2 and a message.

`doc/cloud_object_persistence.md` says that credentials "come from wherever
each provider's engine already reads them". For OCI and OSS that is nowhere.

This spec gives both arms a documented way to receive credentials, makes the
OCI arm fail with a message instead of aborting, and corrects the docs.

### Non-goals

- **Parsing `~/.oci/config` profiles or `~/.aliyun/config.json`.** Both are
  reasonable follow-ups. Neither is needed to make the tool work, and each
  brings its own file format and profile rules.
- **Reading the vendor CLIs' environment names** (`OCI_CLI_*`,
  `ALIBABA_CLOUD_*`). This spec uses the `KYTHIRA_OCI_*` and
  `KYTHIRA_ALIBABA_*` names the real-cloud suites and CI already export.
  Accepting two families invites a credential set assembled half from each.
- **Credential refresh.** The tool runs one operation and exits. A security
  token that expires mid-run fails the run with exit 2, which is the right
  answer. Refresh for long-lived processes is parity-audit gap Q4.
- **Alibaba RAM-role (ECS metadata) credentials and OCI resource
  principals.** Each needs a metadata client this tree does not have for that
  provider. OCI instance principals already exist (`oci_federation.hpp`) and
  are in scope.
- **Credential command-line flags.** Secrets on `argv` are visible to every
  user on the host through `ps` and `/proc`. This spec uses the environment
  only, like the Azure and GCS arms.
- **Changing the S3, Azure or GCS arms**, beyond listing their existing
  variables in `--help` (Requirement 5).

## Glossary

- **OCI arm**: the `--provider oci-objectstorage` branch of
  `run_for_provider()` in `cmd/raft_object_backup/main.cpp`.
- **OSS arm**: the `--provider oss` branch of the same function.
- **Auth mode (OCI)**: one of the three modes `oci_client_config` already
  supports: `api_key` (tenancy, user, fingerprint, private key),
  `security_token` (a pre-obtained UPST plus its session private key) and
  `instance_principal` (`use_instance_principal = true`).
- **Environment lookup**: a callable from a variable name to an optional
  value. Production passes one backed by `std::getenv`; tests pass a map.
- **Configuration error**: an error the operator fixes by setting or
  correcting a variable. The CLI reports it with exit code 1, as it does a
  usage error.

## Requirements

### Requirement 1: OCI credentials from the environment

**User Story:** As an operator restoring a node on OCI, I want
`raft_object_backup` to read my OCI credentials from the environment, so
that `--provider oci-objectstorage` can authenticate at all.

#### Acceptance Criteria

1. THE OCI arm SHALL read these variables:

   | Variable | `oci_client_config` field |
   |---|---|
   | `KYTHIRA_OCI_REGION` | `region` (required in every mode) |
   | `KYTHIRA_OCI_AUTH` | selects the auth mode (1.3) |
   | `KYTHIRA_OCI_TENANCY_ID` | `tenancy_id` |
   | `KYTHIRA_OCI_USER_ID` | `user_id` |
   | `KYTHIRA_OCI_FINGERPRINT` | `fingerprint` |
   | `KYTHIRA_OCI_PRIVATE_KEY_PEM` | `private_key_pem` (the key itself) |
   | `KYTHIRA_OCI_PRIVATE_KEY_FILE` | `private_key_pem` (read from this path) |
   | `KYTHIRA_OCI_PRIVATE_KEY_PASSPHRASE` | `private_key_passphrase` |
   | `KYTHIRA_OCI_SECURITY_TOKEN` | `security_token` |
   | `KYTHIRA_OCI_NAMESPACE` | `oci_object_storage_config::namespace_name` |
   | `KYTHIRA_OCI_ENDPOINT_OVERRIDE` | `endpoint_override` |

   The names match `tests/oci_real_test_support.hpp` and the real-cloud CI
   workflow, so an environment that runs the real suites also runs the tool.
2. WHEN both `KYTHIRA_OCI_PRIVATE_KEY_PEM` and `KYTHIRA_OCI_PRIVATE_KEY_FILE`
   are set THEN the CLI SHALL report a configuration error naming both. It
   SHALL NOT pick one.
3. `KYTHIRA_OCI_AUTH` SHALL accept `api_key`, `security_token` and
   `instance_principal`. WHEN it is unset THEN the mode SHALL be inferred:
   `security_token` if `KYTHIRA_OCI_SECURITY_TOKEN` is set, otherwise
   `api_key`. Instance principal SHALL never be inferred. Off an OCI
   instance its metadata fetch fails only after a network timeout, and an
   operator who simply forgot a variable should not have to wait for that.
4. WHEN `KYTHIRA_OCI_AUTH` holds any other value THEN the CLI SHALL report a
   configuration error listing the accepted values.
5. THE required variables per mode SHALL be:
   - `api_key`: region, tenancy, user, fingerprint, and one private-key
     source.
   - `security_token`: region, security token, and one private-key source.
   - `instance_principal`: region only. Any API-key or token variable that is
     also set SHALL produce a warning on stderr that it is ignored. This is
     not an error.
6. WHEN `KYTHIRA_OCI_NAMESPACE` is set THEN the client SHALL use it and skip
   the `GET /n/` lookup. WHEN it is unset THEN behaviour SHALL be unchanged.

### Requirement 2: OSS credentials from the environment

**User Story:** As an operator restoring a node on Alibaba Cloud, I want
`raft_object_backup` to read my OSS credentials from the environment, so
that `--provider oss` can authenticate at all.

#### Acceptance Criteria

1. THE OSS arm SHALL read these variables:

   | Variable | `alibaba_client_config` field |
   |---|---|
   | `KYTHIRA_ALIBABA_REGION` | `region` (required) |
   | `KYTHIRA_ALIBABA_ACCESS_KEY_ID` | `access_key_id` (required) |
   | `KYTHIRA_ALIBABA_ACCESS_KEY_SECRET` | `access_key_secret` (required) |
   | `KYTHIRA_ALIBABA_SECURITY_TOKEN` | `security_token` (optional, STS) |
   | `KYTHIRA_ALIBABA_ENDPOINT_OVERRIDE` | `endpoint_override` (optional) |

   The names match `tests/alibaba_real_test_support.hpp` and
   `scripts/ci-cloud-credentials/alibaba/README.md`.
2. An access key ID that begins with `STS.` with no security token set SHALL
   produce a warning on stderr that STS keys need
   `KYTHIRA_ALIBABA_SECURITY_TOKEN`. This is not an error, because the prefix
   is Alibaba's convention and not a documented contract.

### Requirement 3: Configuration is checked before any network call

**User Story:** As an operator in an outage, I want one message that lists
everything missing, so that I fix the environment once and not one variable
per attempt.

#### Acceptance Criteria

1. THE OCI and OSS arms SHALL validate their variables before constructing a
   client and before any network I/O.
2. WHEN validation fails THEN the CLI SHALL print one message naming **every**
   missing or conflicting variable for the selected mode, and SHALL exit 1.
3. A variable set to the empty string SHALL count as unset.
4. No message, warning or log line SHALL contain the value of
   `KYTHIRA_OCI_PRIVATE_KEY_PEM`, `KYTHIRA_OCI_PRIVATE_KEY_PASSPHRASE`,
   `KYTHIRA_OCI_SECURITY_TOKEN`, `KYTHIRA_ALIBABA_ACCESS_KEY_SECRET` or
   `KYTHIRA_ALIBABA_SECURITY_TOKEN`. Messages name variables, never their
   contents.
5. WHEN `KYTHIRA_OCI_PRIVATE_KEY_FILE` cannot be read THEN the CLI SHALL
   report a configuration error naming the variable, the path and the OS
   error. An empty file SHALL count as unreadable.
6. Validation SHALL NOT parse the private key. Malformed key material is
   reported by the signer on the first request, which is exit 2. This keeps
   one place, `oci_signing`, that decides whether a key is usable
   (oci-cloud-provider Requirement 1.5).

### Requirement 4: Client construction failures exit 2

**User Story:** As an operator, I want any failure to reach the object store
to produce the documented exit code and a message, so that a script around
the tool can tell "could not do it" from a crash.

#### Acceptance Criteria

1. WHEN constructing a provider client throws THEN the CLI SHALL print
   `raft_object_backup: ` followed by the exception's message and SHALL exit
   2. It SHALL NOT reach `std::terminate`.
2. THIS SHALL hold for every arm, not only OCI. Any constructor that does I/O
   now or later is covered.
3. THE OCI namespace lookup failure message SHALL say that the lookup can be
   skipped with `KYTHIRA_OCI_NAMESPACE`.

### Requirement 5: Help text and documentation

**User Story:** As an operator, I want `--help` and the runbook to say which
variables each provider reads, so that I do not have to read source code
during a restore.

#### Acceptance Criteria

1. `raft_object_backup --help` SHALL gain a "credentials" section listing,
   for each **compiled-in** provider, the variables it reads. That includes
   the existing `KYTHIRA_AZURE_STORAGE_ACCOUNT` and `GOOGLE_CLOUD_PROJECT`,
   and the SDK chain for S3, Azure and GCS.
2. `doc/cloud_object_persistence.md` SHALL replace the sentence claiming
   credentials come "from wherever each provider's engine already reads
   them" with a per-provider table matching the help text. It SHALL state
   that secrets are taken from the environment only, and why.
3. THE cloud-object-persistence spec's task 14 SHALL gain a note pointing to
   this spec.

### Requirement 6: Testing

**User Story:** As a maintainer, I want tests that prove credentials reach
the signer, so that this cannot regress silently a second time.

#### Acceptance Criteria

1. Unit tests SHALL cover the environment-to-config mapping for both
   providers through an injected environment lookup: every OCI mode,
   inference, the key-source conflict, the file source, empty-as-unset, the
   all-missing message listing every name, and the absence of secret values
   from every message.
2. Mock-server tests SHALL build each config from an injected environment,
   construct the real client against `oci_mock_server` (with
   `set_signing_key_pem`) and `alibaba_mock_server` (with `set_credentials`),
   and complete a list call. Both mocks verify signatures, so a passing call
   proves the credentials reached the signer. The OCI test SHALL run once
   with `KYTHIRA_OCI_NAMESPACE` set and once without.
3. A process-level test SHALL run the built `raft_object_backup` binary with
   an empty OCI and OSS environment and assert exit 1 and every missing name
   on stderr. It SHALL also run the OCI arm with valid-looking credentials
   against an endpoint override pointing at a closed loopback port, and
   assert a normal exit with status 2, not death by `SIGABRT`. This test
   SHALL be registered only when the provider is compiled in.
4. These tests SHALL need no cloud account and SHALL be registered with
   CTest.
