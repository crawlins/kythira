# Design Document

## Overview

Two small, SDK-free headers turn an environment lookup into a validated
`oci_object_storage_config` or `alibaba_client_config`, or into a list of
problems. `cmd/raft_object_backup/main.cpp` calls them in the OCI and OSS
arms, prints the problems and exits 1 when there are any, and otherwise
builds the client inside a `try` so a constructor failure exits 2. The help
text and runbook gain a per-provider credentials table.

```
main.cpp OCI arm
  │
  ├─ oci_object_storage_config_from_env(getenv_lookup)
  │     └─ env_config_result<oci_object_storage_config>
  │           ├─ errors   → print each, exit 1        (Req 3)
  │           └─ warnings → print each, continue
  │
  └─ try { oci_object_storage_client{cfg} }           (namespace lookup, I/O)
        catch (std::exception&) → message + hint, exit 2   (Req 4)
     run_backup_cli(client, ...)                      (unchanged)
```

### Key design decision: environment only, `KYTHIRA_*` names

The Azure and GCS arms already take their non-SDK settings from the
environment, so OCI and OSS follow suit. Flags were rejected for secrets
because `argv` is readable by every local user through `ps`. A flag for the
region alone would give one provider two ways to set one value.

The `KYTHIRA_OCI_*` and `KYTHIRA_ALIBABA_*` names are the ones
`tests/oci_real_test_support.hpp`, `tests/alibaba_real_test_support.hpp` and
`.github/workflows/real-cloud-tests.yml` already use. An operator who has run
the real suites, or copied CI's export block, already has the right
environment.

### Key design decision: the mapping lives in a header, not in `main.cpp`

`main.cpp` cannot be unit-tested; `object_store_backup_cli.hpp` exists for
that reason. The new functions take an injected lookup, so a test can drive
every case without `setenv` or touching the process environment. They are
named for the config they build and not for the CLI, so a future `cmd/`
binary that constructs an OCI or Alibaba component (parity audit Q17) can
reuse them.

### Key design decision: never infer instance principal

Instance-principal auth begins by calling the instance metadata service.
Off OCI that call fails only after a connect timeout, with an error about
`169.254.169.254` rather than about credentials. Inferring it whenever the
API-key variables are absent would turn "you forgot `KYTHIRA_OCI_USER_ID`"
into a slow, misleading failure. It is chosen only by
`KYTHIRA_OCI_AUTH=instance_principal`, as `cmd/oci_heartbeat_writer` chooses
it by construction.

### Key design decision: validate presence, not content

Validation checks that the variables a mode needs are present and that the
key file can be read. It does not parse PEM, check fingerprint format or
check region spelling. `oci_signing::sign_request` and `alibaba_signing`
already decide whether material is usable (oci-cloud-provider Requirement
1.5), and a second, slightly different check here would be how the two drift
apart. Bad content therefore fails on the first request, with exit 2 and the
signer's message.

## Components and Interfaces

### Shared result type (`include/raft/env_config_result.hpp`)

```cpp
namespace kythira {

/// Looks up one environment variable. Empty values are returned as nullopt,
/// so every caller treats "set to empty" as unset (Requirement 3.3).
using env_lookup = std::function<std::optional<std::string>(std::string_view)>;

/// The process environment, through std::getenv.
[[nodiscard]] auto process_env_lookup() -> env_lookup;

template <typename Config>
struct env_config_result {
    Config config;                      // meaningful only when ok()
    std::vector<std::string> errors;    // each names variables, never values
    std::vector<std::string> warnings;  // same rule
    [[nodiscard]] auto ok() const -> bool { return errors.empty(); }
};

}  // namespace kythira
```

The result collects every error and does not throw on the first one, so the
CLI can print them all at once (Requirement 3.2).

### OCI (`include/raft/oci_client_config_env.hpp`)

```cpp
namespace kythira {

[[nodiscard]] auto oci_client_config_from_env(const env_lookup& env)
    -> env_config_result<oci_client_config>;

/// The above, plus KYTHIRA_OCI_NAMESPACE into namespace_name.
[[nodiscard]] auto oci_object_storage_config_from_env(const env_lookup& env)
    -> env_config_result<oci_object_storage_config>;

}  // namespace kythira
```

Algorithm:

1. Read every variable in Requirement 1.1 once.
2. Choose the mode from `KYTHIRA_OCI_AUTH`, or infer it (1.3). An unknown
   value is an error (1.4); validation of the other variables still runs
   under the inferred mode, so the message stays complete.
3. Resolve the private key:
   - Both sources set: an error naming both (1.2).
   - File source: read the whole file. If that fails, or the file is
     empty, record an error with the path and `std::strerror(errno)` (3.5).
     Reading the file is the caller's job per `oci_client_config.hpp`, and
     this function is that caller.
4. Collect the missing required names for the mode (1.5). Report them as
   one error, `missing required environment for OCI <mode> auth: A, B, C`.
   Write the private-key requirement as
   `KYTHIRA_OCI_PRIVATE_KEY_PEM or KYTHIRA_OCI_PRIVATE_KEY_FILE`.
5. For `instance_principal`, add a warning for each API-key or token
   variable that is set and will be ignored (1.5).
6. Fill the config. `api_timeout` keeps its default.

The passphrase is passed through in every mode that uses a key; the signer
already ignores it for an unencrypted key.

### OSS (`include/raft/alibaba_client_config_env.hpp`)

```cpp
namespace kythira {

[[nodiscard]] auto alibaba_client_config_from_env(const env_lookup& env)
    -> env_config_result<alibaba_client_config>;

}  // namespace kythira
```

The three required variables are collected into one error. The `STS.` prefix
check is a warning (2.2).

Both headers include only `<raft/oci_client_config.hpp>` (or the Alibaba
equivalent) and the standard library. `oci_object_storage_config` is a plain
aggregate defined in `oci_object_storage_client.hpp`, which pulls in httplib.
To keep the env header SDK-free and testable on every build leg, move the
struct into `include/raft/oci_object_storage_config.hpp` and include that from
the client header. That move is the only change to an existing library
header.

### CLI (`cmd/raft_object_backup/main.cpp`)

A helper reports a result and returns whether to continue:

```cpp
template <typename Config>
auto report(const kythira::env_config_result<Config>& r, std::string_view provider) -> bool {
    for (const auto& w : r.warnings) std::cerr << "raft_object_backup: warning: " << w << "\n";
    for (const auto& e : r.errors)   std::cerr << "raft_object_backup: " << e << "\n";
    if (!r.ok()) std::cerr << "  see `raft_object_backup --help` for " << provider << "'s variables\n";
    return r.ok();
}
```

The OCI arm:

```cpp
auto env = kythira::oci_object_storage_config_from_env(kythira::process_env_lookup());
if (!report(env, "oci-objectstorage")) return 1;
std::optional<kythira::oci_object_storage_client> client;
try {
    client.emplace(std::move(env.config));
} catch (const std::exception& e) {
    std::cerr << "raft_object_backup: could not reach OCI Object Storage: " << e.what() << "\n";
    if (env_namespace_was_unset) {
        std::cerr << "  the tenancy namespace lookup (GET /n/) runs first; set"
                     " KYTHIRA_OCI_NAMESPACE to skip it\n";
    }
    return 2;
}
return kythira::run_backup_cli(*client, args, std::cout, std::cerr);
```

The OSS arm does the same with `alibaba_client_config_from_env`. Its
constructor does no I/O today, but it gets the same `try` (Requirement 4.2).
So do the S3, Azure and GCS arms: their SDK clients can throw from their
constructors on a malformed SDK config.

As a final guard, `main` wraps `run_for_provider(args)` in a
`catch (const std::exception&)` that prints and returns 2. The `try` inside
each arm gives the targeted message; the outer one makes sure that a future
arm which forgets its own `try` still exits 2 instead of aborting.

### Help text (`include/raft/object_store_backup_cli.hpp`)

`print_backup_cli_usage` gains a parameter (or an overload, to keep existing
callers compiling) carrying the credential lines for compiled-in providers.
`main.cpp` owns those lines next to `k_providers`, because which providers
exist is `main.cpp`'s knowledge. An added `credentials_hint` member on
`provider_entry` holds them:

```
credentials (read from the environment; secrets are never taken as flags):
  s3                  the AWS SDK default chain (AWS_PROFILE, AWS_ACCESS_KEY_ID, ...)
  azure-blob          KYTHIRA_AZURE_STORAGE_ACCOUNT, plus the Azure SDK default chain
  gcs                 GOOGLE_CLOUD_PROJECT, plus Application Default Credentials
  oci-objectstorage   KYTHIRA_OCI_REGION, KYTHIRA_OCI_AUTH=api_key|security_token|
                      instance_principal, and per mode: ... (full list)
  oss                 KYTHIRA_ALIBABA_REGION, KYTHIRA_ALIBABA_ACCESS_KEY_ID,
                      KYTHIRA_ALIBABA_ACCESS_KEY_SECRET, [KYTHIRA_ALIBABA_SECURITY_TOKEN]
```

Providers not compiled in are left out of this section. They already appear
under "not compiled into this binary".

## Error handling

| Situation | Exit | Message names |
|---|---|---|
| Required variables missing | 1 | every missing variable, grouped by mode |
| Both key sources set | 1 | both variable names |
| Key file unreadable or empty | 1 | variable, path, `strerror` |
| Unknown `KYTHIRA_OCI_AUTH` | 1 | the accepted values (the given value is not secret, so it is echoed) |
| Client constructor throws | 2 | provider, the exception text, the namespace hint for OCI |
| Signer rejects key material | 2 | via `run_backup_cli`, unchanged |
| Service rejects credentials (401/403) | 2 | via `run_backup_cli`, unchanged |

## Testing Strategy

### `tests/cloud_config_env_unit_test.cpp` (every build leg)

This test has no SDK or httplib dependency, so it is registered
unconditionally next to `object_store_backup_cli_unit_test`. It uses a
map-backed `env_lookup`.

OCI cases:
1. API-key mode with every variable set maps each field.
2. Security-token mode maps the token and key and ignores user, tenancy and
   fingerprint.
3. Inference: a token present gives token mode; otherwise API-key mode; with
   no `KYTHIRA_OCI_AUTH`, never instance principal.
4. Instance principal with only the region set is OK. With stray API-key
   variables it is still OK and warns once per variable.
5. An empty environment gives exactly one "missing" error naming the region,
   tenancy, user, fingerprint and the key alternative.
6. Both key sources set gives an error naming both.
7. The key file source reads the file (a temp file). A missing path errors
   with the path. An empty file errors.
8. An unknown `KYTHIRA_OCI_AUTH` errors with the accepted values.
9. A variable set to `""` counts as unset.
10. The namespace variable fills `namespace_name`.

OSS cases:
11. Full mapping, with and without a token.
12. An empty environment names all three required variables in one error.
13. An `STS.` key with no token warns.

Secrecy:
14. With distinctive sentinel values in every secret variable, no error or
    warning in any case above contains a sentinel.

### Mock round trips

These extend the existing gated mock tests, so they inherit those tests'
gates and need no new CMake gating:

- `oci_object_storage_mock_conformance_test`: build the config with
  `oci_object_storage_config_from_env` from a map holding the test's key PEM
  and `KYTHIRA_OCI_ENDPOINT_OVERRIDE` set to the mock. Construct the client
  and call `list_keys`. Run once with `KYTHIRA_OCI_NAMESPACE` set (the mock
  sees no `GET /n/`) and once without. A second key-file variant writes the
  PEM to a temp file.
- `alibaba_oss_persistence_mock_test`: the same, with `set_credentials` and
  `KYTHIRA_ALIBABA_ENDPOINT_OVERRIDE`. A second case uses an STS token with
  `set_security_token`.

### Process-level CLI test

`tests/raft_object_backup_cli_process_test.cpp` follows the
`CA_SERVICE_PATH="$<TARGET_FILE:ca_service>"` pattern in
`tests/CMakeLists.txt`. It runs the built binary under a scrubbed
environment (`execve` with an explicit `envp`), so no developer credentials
leak in. It is registered only when `raft_object_backup` defines
`KYTHIRA_BACKUP_PROVIDER_OCI` or `_OSS`, and each case is gated on its own
provider.

1. `list --provider oci-objectstorage` with no OCI variables: exit 1, stderr
   names every required variable.
2. The same for `oss`.
3. OCI in API-key mode with a throwaway generated key,
   `KYTHIRA_OCI_ENDPOINT_OVERRIDE=http://127.0.0.1:<closed port>` and no
   namespace: a normal exit with status 2 (not death by `SIGABRT`), and
   stderr mentions `KYTHIRA_OCI_NAMESPACE`.
   This is the regression test for the `std::terminate` defect.
4. `--help` lists the credentials section for each compiled-in provider.

### What stays manual

A live run against a real tenancy with each OCI mode is not automated by this
spec. The real-cloud tier already exercises the clients with these variables.
The task list records a one-time manual check of the binary itself. It needs
no new IAM policy: the `KYTHIRA_OCI_OBJECT_BUCKET` and
`KYTHIRA_ALIBABA_OSS_BUCKET` buckets and their credentials are enough.
