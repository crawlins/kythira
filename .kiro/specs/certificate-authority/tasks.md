# Implementation Plan — Certificate Authority Framework

## Status: Not Started

**Last Updated**: July 7, 2026

## Overview

Implement an in-process `certificate_authority` C++ library (root CA generation,
leaf issuance, negative-scenario presets, revocation/CRL), an RAII
`temp_cert_files` helper for handing PEM material to file-path-based transport
configs, migrate the existing certificate-consuming tests off their fake
placeholder PEM fixtures, and provide a `ca_service` CLI/container for
provisioning certificate material across multi-container Docker/Podman test
scenarios.

Beyond the local/in-process framework, also implement a `certificate_provider`
concept with a CSR-based issuance interface, an `aws_acm_pca_provider`
implementation backed by AWS Certificate Manager Private CA, a `--serve` mode
for `ca_service` that exposes that same provider abstraction over an
authenticated HTTP API, and example deployment packaging for running that
service on a cloud instance or in a long-running container.

## Task Dependency Graph

```json
{
  "waves": [
    {
      "wave": 1,
      "tasks": [1],
      "description": "Core certificate_authority: root CA generation, leaf issuance, negative-scenario presets, and build wiring"
    },
    {
      "wave": 2,
      "tasks": [2, 3],
      "description": "Revocation/CRL and temp_cert_files both depend only on task 1's pem_material shape; independent of each other"
    },
    {
      "wave": 3,
      "tasks": [4, 6, 15],
      "description": "Framework tests, ca_service oneshot CLI, and ca_test_fixture (in-process mode) — all depend only on tasks 1-3, independent of each other"
    },
    {
      "wave": 4,
      "tasks": [5, 7],
      "description": "Migration of existing tests (depends on 1, 3, and now 15 — see task 5) and Docker packaging for oneshot ca_service (depends on the binary from task 6)"
    },
    {
      "wave": 5,
      "tasks": [8],
      "description": "certificate_provider concept, sign_csr()/generate_key_and_csr(), and local_certificate_provider — depends on task 1's pem_material shape"
    },
    {
      "wave": 6,
      "tasks": [9, 10],
      "description": "CSR/provider test coverage (depends on 8) and aws_acm_pca_provider implementation (depends on 8) — independent of each other"
    },
    {
      "wave": 7,
      "tasks": [11, 12],
      "description": "aws_acm_pca_provider test coverage (depends on 10) and ca_service --serve mode (depends on 6, 8, 10) — independent of each other"
    },
    {
      "wave": 8,
      "tasks": [13, 14, 16],
      "description": "ca_service serve-mode integration test and cloud/long-running deployment packaging (both depend on task 12), and extending ca_test_fixture with network-service mode (depends on 12 and 15) — independent of each other"
    },
    {
      "wave": 9,
      "tasks": [17, 20],
      "description": "cpp_httplib_server real TLS termination fix and ca_test_fixture::renew() — both depend only on task 15 (for verification material); independent of each other"
    },
    {
      "wave": 10,
      "tasks": [18, 19, 21],
      "description": "HTTP hot-reload (depends on 17), CoAP hot-reload (depends on 15), and the ca_service renewal route (depends on 12) — independent of each other"
    },
    {
      "wave": 11,
      "tasks": [22],
      "description": "Cross-cutting hot-reload/renewal property tests, depending on 18, 19, 20, and 21"
    },
    {
      "wave": 12,
      "tasks": [23],
      "description": "certificate_authority::from_existing() factory — depends only on task 1"
    },
    {
      "wave": 13,
      "tasks": [24],
      "description": "ca_state_machine (apply/get_state/restore_from_snapshot, encryption helpers) — depends on task 23"
    },
    {
      "wave": 14,
      "tasks": [25],
      "description": "ca_cluster_node executable (Raft node + file_persistence + ca_state_machine + client HTTP API) — depends on task 24 and reuses the /v1/* route logic from task 12"
    },
    {
      "wave": 15,
      "tasks": [26],
      "description": "Multi-node ca_cluster_node tests (bootstrap, failover, disk-restart recovery) — depends on task 25"
    },
    {
      "wave": 16,
      "tasks": [27],
      "description": "3-AZ AWS deployment packaging/documentation for ca_cluster_node — depends on task 25"
    },
    {
      "wave": 17,
      "tasks": [28],
      "description": "acme_test_server (backed by certificate_authority) — depends only on task 1"
    },
    {
      "wave": 18,
      "tasks": [29],
      "description": "acme_certificate_provider (JWS/JWK helpers, http-01/dns-01 responders) — depends on task 8 (certificate_provider concept) and, for dns-01, the existing rfc2136_ldns_discovery"
    },
    {
      "wave": 19,
      "tasks": [30],
      "description": "End-to-end and unit test coverage for the ACME provider and test server — depends on 28 and 29"
    },
    {
      "wave": 20,
      "tasks": [31],
      "description": "LocalStack and real-EC2 test coverage for the 3-AZ ca_cluster_node AWS deployment — depends on tasks 25 and 27"
    }
  ]
}
```

## Tasks

- [ ] 1. Implement `certificate_authority` core in `include/raft/certificate_authority.hpp`
  / `include/raft/certificate_authority_impl.hpp` / `src/certificate_authority.cpp`
  - Add top-level `certificate_authority` CMake library target, built only
    `if(TARGET OpenSSL::SSL)`, linked against `OpenSSL::SSL OpenSSL::Crypto`,
    defining `KYTHIRA_HAS_OPENSSL` on consumers (mirror the `KYTHIRA_HAS_LDNS`
    pattern in the top-level `CMakeLists.txt`)
  - Define `key_algorithm`, `distinguished_name`, `ca_options`,
    `leaf_certificate_options`, `pem_material` per the design's header sketch;
    `certificate_authority` uses a pimpl (`struct impl`) so the public header has
    no OpenSSL includes
  - Ctor `certificate_authority(ca_options)`: generate an `EVP_PKEY` per
    `options.algorithm` (RSA via `EVP_PKEY_CTX` keygen for 2048/4096-bit;
    EC via `EVP_PKEY_CTX` keygen for P-256/P-384), build a self-signed `X509`
    with `basicConstraints=critical,CA:TRUE`, `keyUsage=critical,keyCertSign,cRLSign`,
    `subjectKeyIdentifier=hash`, sign with `EVP_sha256()`; serialize to
    `_root_pem` immediately; no filesystem/network I/O
  - `next_serial()`: seed a per-instance counter from
    `high_resolution_clock::now()` XORed with `this`'s address at construction;
    increment per issuance — guards against cross-instance/cross-process serial
    collisions under parallel `ctest`
  - `root_certificate_pem()`: returns `_root_pem`; no accessor for the CA
    private key
  - `issue(leaf_certificate_options)`: throw `std::invalid_argument` if both
    `dns_names` and `ip_addresses` are empty; otherwise generate a fresh leaf
    `EVP_PKEY`, build an `X509` with `basicConstraints=critical,CA:FALSE`,
    `subjectAltName` from `dns_names`/`ip_addresses`, `keyUsage`/
    `extendedKeyUsage` from `server_auth`/`client_auth`, sign with the CA's key,
    return `pem_material{cert_pem, key_pem, chain_pem, serial}` where
    `chain_pem` is leaf-PEM concatenated with `_root_pem`
  - `issue_expired(opts)` / `issue_not_yet_valid(opts)`: both funnel through a
    private `issue_with_window(opts, not_before, not_after)`; expired uses
    `[now-2d, now-1d]`, not-yet-valid uses `[now+1d, now+1d+opts.validity]`
  - Guard non-reentrant OpenSSL sequences with an internal `std::mutex` so
    `issue()`/ctor are safe under concurrent calls on one instance
  - Drain the OpenSSL error queue (`ERR_get_error`/`ERR_error_string_n`) and
    throw `std::runtime_error` on any crypto call failure
  - Add `DEPENDENCIES.md` entry: `OpenSSL ≥ 3.0 — EVP_PKEY key generation,
    X509/X509_CRL construction and signing, X509V3 extensions (already an
    optional project dependency)`
  - Verify: `cmake --build build` succeeds with and without OpenSSL present
  - _Requirements: 1.1–1.5, 2.1–2.6, 3.1–3.4, 8.1–8.3_

- [ ] 2. Implement revocation and CRL support on `certificate_authority`
  - Depends on task 1 (`pem_material.serial`, internal mutex, `_ca_key`/`_ca_cert`)
  - Track every issued serial in an internal set at the end of each successful
    `issue*()` call
  - `revoke(const pem_material&)`: throw `std::invalid_argument` if
    `cert.serial` is not in the tracked set; otherwise append
    `{serial, revoked_at=now}` to an internal revoked list
  - `crl_pem() const`: build a fresh `X509_CRL` from the revoked list on every
    call (`X509_CRL_new`, `X509_CRL_set_issuer_name`, one `X509_REVOKED` per
    entry via `X509_CRL_add0_revoked`, `set_lastUpdate`/`set_nextUpdate`,
    `X509_CRL_sign` with the CA key), serialize to PEM, return it
  - Verify: a property test loads an issued cert + its CRL into an
    `X509_STORE` with `X509_V_FLAG_CRL_CHECK` set and confirms
    `X509_V_ERR_CERT_REVOKED` after `revoke()`, and confirms verification
    succeeds before `revoke()` is called
  - _Requirements: 4.1–4.3_

- [ ] 3. Implement `temp_cert_files` RAII helper in `include/raft/certificate_authority.hpp`
  - Depends on task 1 (`pem_material` shape)
  - Ctor: build a unique directory name from `material.serial` plus a
    high-resolution timestamp under `std::filesystem::temp_directory_path()`;
    `create_directories`; write `cert.pem` (default permissions), `key.pem`
    (mode `0600`), and — when `material.chain_pem` is non-empty — `chain.pem`
  - Ctor failures (cannot create directory / write a file) propagate as
    `std::filesystem::filesystem_error`
  - Dtor: `std::filesystem::remove_all(_dir, ec)` using the `error_code`
    overload; swallow all errors; never throws
  - `cert_path()` / `key_path()` / `chain_path()` accessors return absolute
    paths; `chain_path()` returns `""` when no chain material was provided
  - Verify: unit test confirms key file mode is `0600`, directory is removed
    after destruction (including after a `BOOST_CHECK_THROW` block that
    constructs and destroys one on the stack), and two instances constructed in
    the same test process never collide on a path
  - _Requirements: 5.1–5.5_

- [ ] 4. Add framework test coverage: `tests/certificate_authority_unit_test.cpp`
  and `tests/certificate_authority_property_test.cpp`
  - Depends on tasks 1–3
  - Unit test: construct a CA per `key_algorithm` value; issue leaves with
    server-only, client-only, and dual EKU; parse the resulting certificate's
    extensions with `X509_get_ext_d2i` (not string-matching PEM) and assert
    `basicConstraints`, `keyUsage`, `extendedKeyUsage`, `subjectAltName` match
    the requested options; assert `issue()` throws `std::invalid_argument` when
    both SAN lists are empty
  - Property test: verify Properties 1–4 from the design directly against
    `X509_verify_cert()` — issued-leaf-chains-to-own-root, expired/not-yet-valid
    fail exactly on time window, cross-CA leaves fail to verify, revoked +
    CRL-checking fails while un-revoked or CRL-checking-disabled succeeds
  - Both files added to `tests/CMakeLists.txt`, linked against
    `certificate_authority`, gated `if(TARGET OpenSSL::SSL)`; two-argument
    `BOOST_AUTO_TEST_CASE(..., *boost::unit_test::timeout(N))` throughout
    (30 s unit, 90 s property)
  - Verify: `ctest --test-dir build -R certificate_authority` passes
  - _Requirements: 1.1–1.5, 2.1–2.6, 3.1–3.4, 4.1–4.3_

- [ ] 5. Migrate existing certificate-consuming tests off placeholder PEM fixtures
  - Depends on tasks 1, 3, and 15 (`ca_test_fixture`, in-process mode) — use
    the fixture rather than wiring `certificate_authority` + `temp_cert_files`
    by hand in each test file
  - `tests/http_ssl_mutual_tls_integration_test.cpp`: replace the hardcoded
    `server_cert_pem`/`server_key_pem`/`client_cert_pem`/`client_key_pem`/
    `ca_cert_pem` constants with a `ca_test_fixture` member; obtain the server
    leaf via `bootstrap_client("server", {"127.0.0.1", "localhost"},
    /*server_auth=*/true, /*client_auth=*/false)` and the client leaf via
    `bootstrap_client("client", {}, /*server_auth=*/false,
    /*client_auth=*/true)`; wire the returned `temp_cert_files` paths into
    `cpp_httplib_server_config`/`cpp_httplib_client_config` in place of
    `create_temp_cert_file(...)` calls
  - `tests/coap_certificate_validation_failure_property_test.cpp` and
    `tests/coap_dtls_handshake_property_test.cpp`: replace valid-certificate
    baseline fixtures the same way via `ca_test_fixture`; leave test cases that
    assert on malformed or truncated PEM content using their existing
    hand-authored invalid strings unchanged
  - `tests/http_ssl_certificate_loading_unit_test.cpp`: replace
    `expired_cert_pem` with `certificate_authority::issue_expired()` output
    (this file tests certificate *loading*, not a live handshake, so it uses
    `certificate_authority` directly rather than the fixture); leave
    `invalid_cert_pem` (deliberately unparseable) unchanged
  - Verify: `ctest --test-dir build -R "http_ssl|coap_certificate|coap_dtls"`
    passes, and the mutual-TLS integration test now completes an actual
    handshake rather than short-circuiting on unparseable material
  - _Requirements: 6.1–6.4_

- [ ] 6. Implement `ca_service` in `cmd/ca_service/main.cpp` + `cmd/ca_service/CMakeLists.txt`
  - Depends on tasks 1, 3 (uses `certificate_authority`; does not need
    `temp_cert_files`, since output is a fixed `--out-dir`, not a temp
    directory — writes files directly)
  - Hand-rolled argv parsing (consistent with other `cmd/*` executables):
    repeatable `--service <name>[:alt1,alt2,...]`, required `--out-dir <path>`,
    optional `--domain <suffix>`, `--validity-days <n>` (default 30),
    `--resolve-ips` flag; print usage and return non-zero on missing
    `--out-dir` or zero `--service` entries
  - Construct one `certificate_authority` with default `ca_options`; write
    `root_ca.pem`
  - Per `service_spec`: build `dns_names` from name + optional `--domain`
    suffix + `alt` names; when `--resolve-ips`, call `getaddrinfo()` per DNS
    name and add resolved addresses as `ip_addresses` (skip names that fail to
    resolve rather than failing the whole run); `issue()` a dual-purpose
    (`server_auth=true, client_auth=true`) leaf with validity
    `24h * validity_days`; write `<out-dir>/<name>/{cert.pem,key.pem (mode
    0600),chain.pem}`
  - Wrap all logic in `main()` in a try/catch; print exceptions to `stderr` and
    return non-zero; return 0 on success; no network listener, no long-lived
    loop
  - Add `cmd/ca_service` to the top-level build (mirroring how `cmd/dns_discovery_node`
    etc. are wired in), gated on the `certificate_authority` target existing
  - Verify: running `ca_service --out-dir <tmp> --service node1 --service node2:alt2`
    produces the documented layout, and each `<name>/cert.pem` chain-verifies
    against `root_ca.pem`
  - _Requirements: 7.1–7.5_

- [ ] 7. Package `ca_service` for Docker/Podman scenarios
  - Depends on task 6
  - `docker/ca_service/Dockerfile`: two-stage build mirroring
    `docker/dns_discovery_node/Dockerfile` (builder stage compiles
    `ca_service`; runtime stage copies the binary plus `libssl3`); no
    `HEALTHCHECK` (init-container pattern, gated on exit status not a probe)
  - `docker/ca-provisioning-compose.yml`: a `ca-service` container writing to a
    named volume, plus two illustrative dependent node stubs mounting that
    volume read-only with `depends_on: ca-service: condition:
    service_completed_successfully`; no static IPs; addressing by compose
    service name; must work unmodified under both Docker and rootless Podman
    (no `--privileged`, no host networking)
  - Verify: `docker compose -f docker/ca-provisioning-compose.yml up
    --abort-on-container-exit` (and the rootless-Podman equivalent via
    `compose_prefix()`/`$KYTHIRA_COMPOSE_COMMAND`) completes with `ca-service`
    exiting 0 before the dependent stubs start
  - _Requirements: 7.6–7.7_

- [ ] 8. Implement `certificate_provider` concept, CSR signing, and
  `local_certificate_provider` in `include/raft/certificate_provider.hpp`
  (+ `sign_csr()` added to `certificate_authority`)
  - Depends on task 1 (`pem_material`, `certificate_authority` internals)
  - Add `csr_signing_options` and `csr_material` structs per the design
  - `generate_key_and_csr(leaf_certificate_options)`: generate a key via the
    same `generate_key(key_algorithm)` helper `certificate_authority` uses
    internally, build an `X509_REQ` with `options.subject` and a
    `subjectAltName` extension via `X509_REQ_add_extensions`, self-sign with
    `X509_REQ_sign`; return `{private_key_pem, csr_pem}` — no
    `certificate_authority` instance required
  - `certificate_authority::sign_csr(csr_pem, csr_signing_options)`: load the
    CSR (`PEM_read_bio_X509_REQ`), verify its self-signature
    (`X509_REQ_verify`) and throw `std::invalid_argument` on failure, extract
    the embedded public key, build a leaf `X509` via the same
    `build_leaf_cert()` helper `issue_with_window()` uses, sign with the CA
    key, return `pem_material` with `private_key_pem` left empty
  - Refactor `issue_with_window()` to share `build_leaf_cert()` with
    `sign_csr()` rather than duplicating extension-setting logic
  - `local_certificate_provider`: non-owning `certificate_authority&` wrapper
    exposing `root_certificate_pem()`/`sign_csr()` as immediately-resolved
    `kythira::Future`s; `static_assert(certificate_provider<local_certificate_provider>)`
  - Verify: `cmake --build build` succeeds; a quick manual check that
    `sign_csr()` rejects a CSR whose signature doesn't match its own public key
  - _Requirements: 9.1–9.5_

- [ ] 9. Extend `certificate_authority_property_test.cpp` with CSR/provider
  coverage (Properties 7–8)
  - Depends on task 8
  - Property 7: for `csr = generate_key_and_csr(opts)` and
    `m = ca.sign_csr(csr.csr_pem, ...)`, `m.certificate_pem` chain-verifies
    against `ca.root_certificate_pem()` identically to `issue()` output
  - Property 8: assert `m.private_key_pem.empty()` for every `sign_csr()`
    result, both from the local `certificate_authority` and (once task 10
    lands) `aws_acm_pca_provider`
  - Add a case asserting `sign_csr()` throws `std::invalid_argument` on a CSR
    with a tampered/invalid self-signature
  - Verify: `ctest --test-dir build -R certificate_authority` still passes
  - _Requirements: 9.1, 9.4_

- [ ] 10. Implement `aws_acm_pca_provider` in
  `include/raft/aws_acm_pca_provider.hpp` / `_impl.hpp`
  - Depends on task 8 (`certificate_provider` concept, `csr_signing_options`,
    `pem_material`)
  - Add a separate `find_package(AWSSDK QUIET COMPONENTS acm-pca)` check in the
    top-level `CMakeLists.txt` defining `KYTHIRA_HAS_AWS_ACM_PCA` independently
    of `KYTHIRA_HAS_AWS_SDK`; add `"acm-pca"` to `vcpkg.json`'s `aws-sdk-cpp`
    feature list
  - Define `aws_acm_pca_provider_config` embedding `aws_client_config` plus
    `certificate_authority_arn`, `template_arn`, `signing_algorithm`,
    `validity`
  - `root_certificate_pem()`: `GetCertificateAuthorityCertificateRequest`,
    cache result under a mutex; wrap the call in
    `fiu_do_on("raft/aws/acm_pca/get_certificate_authority_certificate", ...)`
  - `sign_csr()`: `IssueCertificateRequest` (CSR bytes, `template_arn`,
    `signing_algorithm`, validity), wrapped in
    `fiu_do_on("raft/aws/acm_pca/issue_certificate", ...)`; then poll
    `GetCertificateRequest` with backoff bounded by `_config.aws.api_timeout`,
    wrapped in `fiu_do_on("raft/aws/acm_pca/get_certificate", ...)`; return
    `pem_material{cert, "", cert + chain, 0}` on success, reject the future
    with the AWS error message on failure or poll-timeout
  - `revoke(serial)`: `RevokeCertificateRequest`; propagate AWS errors (e.g. no
    CRL/OCSP configured on the target CA) directly to the caller
  - `static_assert(certificate_provider<aws_acm_pca_provider>)`
  - Verify: `cmake --build build` succeeds with and without the `acm-pca` SDK
    component present
  - _Requirements: 8.4–8.5, 10.1–10.7_

- [ ] 11. Add `aws_acm_pca_provider` test coverage (unit / LocalStack / real)
  - Depends on task 10
  - `tests/aws_acm_pca_provider_unit_test.cpp`: config validation and
    `fiu_enable("raft/aws/acm_pca/...")`-forced error-path assertions, no live
    AWS access required, gated `#ifdef KYTHIRA_HAS_AWS_ACM_PCA`
  - `tests/aws_acm_pca_provider_localstack_test.cpp`: gated like
    `aws_quorum_manager_localstack_test.cpp`; probe `acm-pca` support first and
    skip the whole suite if the target LocalStack doesn't support it (ACM
    Private CA emulation is a LocalStack Pro feature) — do not fail the build
    or the test run on community-edition LocalStack
  - `tests/aws_acm_pca_provider_real_test.cpp`: opt-in, requires
    `$KYTHIRA_TEST_ACM_PCA_ARN`; skip when unset; tagged and excluded from the
    default `ctest` run, matching `aws_quorum_manager_real_ec2_test.cpp`
  - Verify: `ctest --test-dir build -R aws_acm_pca_provider_unit` passes
    without any AWS credentials configured
  - _Requirements: 10.1–10.7_

- [ ] 12. Implement `ca_service --serve` mode
  - Depends on tasks 6 (existing `cmd/ca_service/main.cpp` oneshot CLI), 8
    (`local_certificate_provider`), 10 (`aws_acm_pca_provider`)
  - Extend `cli_options` with `--serve <host:port>`, `--provider
    {local|aws-acm-pca}`, `--acm-pca-arn`, `--aws-region`,
    `--aws-endpoint-override`, `--auth-token` (or `$CA_SERVICE_AUTH_TOKEN`),
    `--tls-cert`/`--tls-key`; reject `--serve` combined with
    `--out-dir`/`--service` with a usage error
  - Refuse to start in `--serve` mode with no auth token configured (fail
    closed)
  - Construct `httplib::Server` (or `SSLServer` when TLS flags are given);
    register a `set_pre_routing_handler` enforcing `Authorization: Bearer
    <token>` on every route, returning `401` otherwise
  - Routes: `GET /healthz`, `GET /v1/root-ca`, `POST /v1/certificates` (CSR +
    JSON metadata in, `{certificate_pem, chain_pem}` JSON out), `POST
    /v1/certificates/revoke` and `GET /v1/crl` (local provider only; `501` for
    `aws-acm-pca`)
  - Install `SIGINT`/`SIGTERM` handlers calling `server.stop()`; `main()`
    blocks in `server.listen(...)` until shutdown
  - Verify: manual `curl` against a locally started `ca_service --serve
    127.0.0.1:8443 --provider local --auth-token test` exercises all routes,
    confirms `401` without the header and `200`/expected JSON with it
  - _Requirements: 11.1–11.6_

- [ ] 13. Add `ca_service` serve-mode integration test
  - Depends on task 12
  - `tests/ca_service_serve_integration_test.cpp`: start `ca_service --serve`
    as a child process with `--provider local` and a known `--auth-token`;
    exercise `/healthz`, `/v1/root-ca`, `/v1/certificates` (using a CSR from
    `generate_key_and_csr()`); assert Properties 8–9 (no private key in the
    response body; `401` without the auth header); send `SIGTERM` and confirm
    clean shutdown
  - Verify: `ctest --test-dir build -R ca_service_serve` passes
  - _Requirements: 11.1–11.6_

- [ ] 14. Package `ca_service --serve` for cloud and long-running-container deployment
  - Depends on task 12 (reuses the `docker/ca_service/Dockerfile` image built
    in task 7 unmodified — no new image)
  - `docker/ca-service-server-compose.yml`: long-running `ca-service` container
    running `--serve 0.0.0.0:8443 --provider local`, `HEALTHCHECK` against
    `/healthz`, addressed by compose service name, no static IPs
  - `docker/ca_service/ca_service.service`: sample systemd unit —
    `ExecStart=/usr/local/bin/ca_service --serve 0.0.0.0:8443 ...`,
    `Restart=on-failure`, `EnvironmentFile=` supplying
    `CA_SERVICE_AUTH_TOKEN` and (for `--provider aws-acm-pca`) AWS
    region/CA ARN
  - `docker/ca_service/ecs-task-definition.json`: sample ECS task definition
    running the same container image with `--serve`, a task role scoped to
    `acm-pca:GetCertificateAuthorityCertificate`,
    `acm-pca:IssueCertificate`, `acm-pca:GetCertificate` for the
    `aws-acm-pca` provider case
  - Verify: `docker compose -f docker/ca-service-server-compose.yml up -d`
    (and the rootless-Podman equivalent) reports the container healthy via
    `/healthz`
  - _Requirements: 12.1–12.6_

- [ ] 15. Implement `ca_test_fixture` (in-process mode) in `tests/ca_test_fixture.hpp`
  - Depends on tasks 1, 3 (`certificate_authority`, `temp_cert_files`); does
    NOT depend on `ca_service`/serve mode yet — that's task 16
  - Define `ca_test_fixture_options` and `ca_test_fixture` per the design's
    header sketch; `_issued` holds `unique_ptr<temp_cert_files>` so returned
    `const temp_cert_files&` references stay valid as more clients are
    bootstrapped
  - Ctor (`start_network_service = false`, the default): construct `_ca` only;
    no process, no network
  - `bootstrap_client(client_id, dns_names, ip_addresses, server_auth,
    client_auth, validity)`: call `_ca.issue({...})`, wrap in a new
    `temp_cert_files` owned by `_issued`, return a reference to it
  - `root_certificate_pem()`: return `_ca.root_certificate_pem()`
  - Leave `start_network_service = true` unimplemented for now — throw
    `std::logic_error("not yet supported")` if requested, replaced by task 16
  - Verify: `ctest --test-dir build -R ca_test_fixture` (new
    `tests/ca_test_fixture_unit_test.cpp`) passes, covering Property 10
    (distinct, non-impersonatable identities from repeated `bootstrap_client()`
    calls)
  - _Requirements: 13.1, 13.3 (in-process path), 13.4, 13.5 (in-process path), 13.6_

- [ ] 16. Extend `ca_test_fixture` with network-service mode
  - Depends on tasks 12 (`ca_service --serve`) and 15
  - Implement `start_network_service = true`: launch `ca_service --serve
    <serve_bind_address> --provider local --auth-token <fixture-generated
    token>` as a child process (reusing the process-launch approach already
    established by `docker_chaos::os::real_exec`-style helpers where
    reasonable, though this fixture runs outside the docker_chaos harness);
    poll `/healthz` with backoff bounded by `startup_timeout`; on timeout, kill
    the child and throw `std::runtime_error`; fetch and cache `/v1/root-ca`
  - Implement the service-mode path of `bootstrap_client()`: call
    `generate_key_and_csr({...})` locally, `POST` the CSR and metadata to
    `/v1/certificates` with the fixture's bearer token, combine the local
    private key with the returned `certificate_pem`/`chain_pem` into a
    `pem_material`, and materialize it the same way as the in-process path
  - Dtor: terminate the child process (best-effort) after the base class-style
    cleanup of `_issued`
  - Update `tests/ca_test_fixture_unit_test.cpp` to run its full case set
    against both modes (parameterize or duplicate the test suite)
  - Verify: `ctest --test-dir build -R ca_test_fixture` passes with
    `start_network_service = true`, and `ca_service_serve_integration_test.cpp`
    (task 13) MAY be simplified to use `ca_test_fixture` instead of managing
    the child process itself
  - _Requirements: 13.1, 13.2, 13.3 (service path), 13.5 (service path)_

- [ ] 17. Fix `cpp_httplib_server` to perform real TLS termination via `httplib::SSLServer`
  - Depends on task 15 (`ca_test_fixture`, for the test rewrites below)
  - Add `std::unique_ptr<httplib::SSLServer> _ssl_server` alongside the
    existing `_http_server`, both under `#ifdef CPPHTTPLIB_OPENSSL_SUPPORT`
    where needed; construct `_ssl_server` (not `_http_server`) when
    `_config.enable_ssl` is true
  - Rewrite `configure_ssl_server()` to construct `httplib::SSLServer` from
    `ssl_cert_path`/`ssl_key_path`, then apply cipher-suite/TLS-version
    configuration and (when `require_client_cert`) CA loading + verify mode to
    the *live* `SSL_CTX*` obtained via `ssl_context()` — remove the
    unconditional `"...not fully implemented..."` throw entirely; keep all
    existing genuine-failure checks (missing/mismatched cert-key pair, invalid
    cipher suite, missing CA path)
  - Update `setup_endpoints()`/`start()`/`stop()` to operate on whichever of
    `_http_server`/`_ssl_server` is active (small private `active_server()`
    accessor; `SSLServer` derives from `Server` so route-registration code is
    unchanged)
  - Confirm exact `httplib::SSLServer` constructor/accessor names
    (`ssl_context()`, `is_valid()`) against the vendored cpp-httplib version
  - Rewrite `tests/http_ssl_mutual_tls_integration_test.cpp`,
    `tests/http_ssl_configuration_property_test.cpp`, and
    `tests/http_ssl_context_configuration_unit_test.cpp` to use
    `ca_test_fixture::bootstrap_client()` for valid-certificate cases and
    assert successful construction + a completed handshake, removing the
    `catch (ssl_configuration_error&) { /* expected, not fully implemented */ }`
    branches for those cases; genuine-failure test cases keep asserting
    `ssl_configuration_error`
  - Verify: `ctest --test-dir build -R "http_ssl|http_client_comprehensive|http_server"`
    passes with SSL actually established (not skipped/tolerated)
  - _Requirements: 14.1–14.6_

- [ ] 18. Implement `reload_tls_material()` / `enable_auto_reload()` for `cpp_httplib_server` and `cpp_httplib_client`
  - Depends on task 17
  - `cpp_httplib_server::reload_tls_material()`: validate the new cert/key pair
    with the existing `validate_certificate_key_pair()` helper first; on
    success, call `SSL_CTX_use_certificate_chain_file` /
    `SSL_CTX_use_PrivateKey_file` / `SSL_CTX_check_private_key` on
    `_ssl_server->ssl_context()`, and (when `require_client_cert`)
    `SSL_CTX_load_verify_locations` for the CA path; throw
    `ssl_configuration_error` without touching the live context on any
    validation failure
  - Mirror `reload_tls_material()` on `cpp_httplib_client` for
    `client_cert_path`/`client_key_path`/`ca_cert_path`
  - `enable_auto_reload(poll_interval)`: `std::jthread` polling
    `std::filesystem::last_write_time()` on the configured cert path, calling
    `reload_tls_material()` on change, catching and logging failures via the
    transport's existing metrics/logger path, never stopping the loop on a
    caught failure
  - `disable_auto_reload()`: request-stop and join the `jthread`
  - Verify: manual test holds a connection open across a `reload_tls_material()`
    call and confirms it keeps working; a new connection afterward presents
    the new cert
  - _Requirements: 16.1, 16.3–16.7_

- [ ] 19. Implement `reload_tls_material()` / `enable_auto_reload()` for `coap_server` and `coap_client`
  - Depends on task 15 (for test verification material); implementation itself
    has no dependency on the CA framework beyond that
  - Extract the existing PKI-config-building logic at
    `coap_transport_impl.hpp:676` / `:2238` into a reusable
    `build_pki_config(config)` helper if not already isolated
  - `reload_tls_material()`: call `build_pki_config()` with the current
    `_config` (post file-path update) and re-invoke
    `coap_context_set_pki(_coap_context, &pki_config)`; throw
    `certificate_validation_error` if it returns failure
  - Experimentally verify whether the vendored libcoap safely re-applies PKI
    config to a context with active DTLS sessions; if not, fall back to
    opening a fresh `coap_context_t*` and migrating the listening endpoint,
    draining the old context's sessions as they close naturally (design this
    fallback only if the experiment shows it's needed)
  - `enable_auto_reload()`/`disable_auto_reload()`: same `std::jthread` +
    mtime-polling shape as task 18
  - Verify: an established DTLS association survives a `reload_tls_material()`
    call; a new association afterward uses the new cert
  - _Requirements: 16.2–16.7_

- [ ] 20. Implement `ca_test_fixture::renew()` and `temp_cert_files::replace_atomically()`
  - Depends on task 15
  - `temp_cert_files::replace_atomically(const pem_material&)` (private,
    used only by `renew()`): write `cert.pem.tmp`/`key.pem.tmp`/
    `chain.pem.tmp` in the fixture's directory, then
    `std::filesystem::rename()` each over its non-`.tmp` counterpart
  - `ca_test_fixture` tracks, per `client_id` passed to `bootstrap_client()`,
    the options used and the index into `_issued`
  - `renew(client_id)`: look up the stored options, re-issue (in-process or
    via the running service, matching whichever mode `bootstrap_client()`
    used), call `replace_atomically()` on the corresponding `temp_cert_files`,
    return a reference to it; throw `std::invalid_argument` for an unknown
    `client_id`
  - Verify: `ctest --test-dir build -R ca_test_fixture` covers Property 14 —
    identity preserved, validity window/serial advanced across `renew()`
  - _Requirements: 15.1, 15.2_

- [ ] 21. Implement `ca_service`'s `POST /v1/certificates/renew` route
  - Depends on task 12 (`ca_service --serve`)
  - Set the serve-mode `SSL_CTX`'s verify mode to `SSL_VERIFY_PEER` (request,
    not require, a client certificate) so bearer-token routes keep working
    for callers presenting none
  - In the `/v1/certificates/renew` handler: retrieve the peer certificate for
    the current connection (confirm the exact cpp-httplib accessor against the
    vendored version); return `401` if absent or if it fails to chain-verify
    against the active provider's `root_certificate_pem()`
  - On success, extract the presented certificate's subject/SAN, and issue a
    fresh certificate with identical subject/SAN and a new validity window via
    the same `certificate_provider::sign_csr()` path `/v1/certificates` uses
    (caller supplies a fresh CSR in the request body)
  - Verify: manual `curl --cert ... --key ...` against a running
    `ca_service --serve` renews successfully; a request with an unrelated
    certificate gets `401`
  - _Requirements: 15.3–15.5_

- [ ] 22. Add cross-cutting hot-reload/renewal property test coverage
  - Depends on tasks 18, 19, 20, 21
  - `tests/http_transport_reload_property_test.cpp`: Properties 11–13 for
    `cpp_httplib_server` — in-flight connection survives reload, new
    connection after reload sees the new cert, invalid reload leaves the old
    cert serving; `enable_auto_reload()` picks up a `ca_test_fixture::renew()`
    file swap within a bounded number of poll intervals
  - `tests/coap_transport_reload_property_test.cpp`: the same three
    properties for `coap_server`/`coap_client` over DTLS
  - Extend `tests/ca_service_serve_integration_test.cpp` with Property 14
    coverage for the renewal route (subject/SAN preserved, unrelated cert
    rejected with `401`)
  - Verify: `ctest --test-dir build -R "reload|ca_service_serve"` passes
  - _Requirements: 16.4 (Property 11–12), 16.3 (Property 13), 15.1/15.3/15.4 (Property 14)_

- [ ] 23. Implement `certificate_authority::from_existing()`
  - Depends on task 1
  - Static factory parsing supplied `ca_cert_pem`/`ca_key_pem` via
    `PEM_read_bio_X509`/`PEM_read_bio_PrivateKey`, verifying the key matches
    the cert's public key, and constructing a `certificate_authority` whose
    internal `_ca_key`/`_ca_cert`/`_root_pem` come from the supplied material;
    `instance_seed`/`_serial_counter` are still freshly computed at
    construction, per task 1's existing scheme
  - Throw `std::invalid_argument` on unparseable PEM or a key/cert mismatch
  - Verify: round-trip test — `issue()` from an original instance, extract its
    root cert/key PEM (via a test-only accessor or by exporting at
    construction time in the test), reconstruct via `from_existing()`, issue
    again, and confirm both leaves chain-verify against the same root
  - _Requirements: 17.9_

- [ ] 24. Implement `ca_state_machine` in `include/raft/ca_state_machine.hpp`
  - Depends on task 23 (`from_existing`, used by `ca_cluster_node` in task 25,
    not by the state machine itself, but the type shapes are shared)
  - Define `ca_command_type`, `ca_ledger_entry`, and `ca_state_machine` per the
    design sketch; implement `apply()`/`get_state()`/`restore_from_snapshot()`
    satisfying `kythira::state_machine<ca_state_machine, std::uint64_t>`,
    verified by a `static_assert`
  - `apply()`: dispatch on `ca_command_type`; `bootstrap_ca` sets
    `_bootstrapped`/`_root_cert_pem`/`_encrypted_ca_key_pem` once, returning a
    structured error result (not a thrown exception) on a duplicate attempt;
    `record_issuance` appends a `ca_ledger_entry`; `record_revocation` sets
    `revoked_at` on the matching entry by serial, erroring if not found
  - Encode/decode commands and `get_state()`/`restore_from_snapshot()` via
    boost::json, matching the encoding convention already used by
    `file_persistence.hpp`
  - Implement the AES-256-GCM encrypt/decrypt helpers for the CA private key
    (PBKDF2-derived key from an operator-supplied passphrase; ciphertext +
    nonce + tag stored together, base64-encoded in `_encrypted_ca_key_pem`)
  - Verify: `tests/ca_state_machine_unit_test.cpp` — Property 15 (two
    instances fed the same command sequence produce byte-identical
    `get_state()`), duplicate-bootstrap rejection, and a
    `restore_from_snapshot(get_state())` round-trip
  - _Requirements: 17.1–17.4_

- [ ] 25. Implement `ca_cluster_node` in `cmd/ca_cluster_node/main.cpp` + `cmd/ca_cluster_node/CMakeLists.txt`
  - Depends on task 24 (`ca_state_machine`); reuses the `/v1/*` route handler
    logic already written for task 12 (`ca_service --serve`) and task 21 (the
    renewal route)
  - Define `ca_cluster_node_config` mirroring `cmd/chaos_node/config.hpp`'s
    `node_config` (`node_id`, `rpc_address`/`rpc_port`, `http_port`,
    `data_dir`, `peers`, election/heartbeat timing) plus `unseal_key_file` and
    `bootstrap_ca`
  - Wire `node<Types>` (Raft), `ca_state_machine`, and `file_persistence`
    (rooted at `data_dir`) together; start the Raft RPC transport on
    `rpc_port` and an `httplib::SSLServer`-based client API on `http_port`
  - Every `/v1/*` handler checks `raft_node.is_leader()` first; on `false`,
    look up the known leader's `node_id` and resolve it to an `http_address`
    via the configured `ca_cluster_peer_info` list, then respond `308` with a
    `Location` header pointing there (preserving method/body for redirected
    `POST`s); WHEN no leader is known yet, respond `503` with
    `{"error": "no_known_leader"}` instead
  - WHEN `--bootstrap-ca` is set and, after this node has caught up on
    replication, `!state_machine.has_root_material()`: generate a
    `certificate_authority` locally, encrypt its key with the configured
    unseal passphrase, `submit_command(bootstrap_ca{...})`
  - On becoming leader (including re-becoming leader after this process's own
    restart): decrypt `encrypted_bootstrap_material()` with the unseal
    passphrase and construct `certificate_authority::from_existing(...)`;
    treat a decryption/authentication-tag failure as fatal at startup
  - `/v1/certificates` (and `/renew`): validate the CSR, call the held
    `certificate_authority::sign_csr()`, then
    `submit_command(record_issuance{...})`, responding to the HTTP client only
    after that future resolves
  - Default `ca_cluster_node_config.peers`/documentation SHALL describe and
    recommend a 3-node cluster (Requirement 17.11) — the smallest odd cluster
    size tolerating one node failure
  - Verify: a single-node `ca_cluster_node` (trivial 1-node Raft "cluster")
    bootstraps, issues, and restarts, recovering state from `file_persistence`
    on disk
  - _Requirements: 17.5–17.11_

- [ ] 26. Add multi-node `ca_cluster_node` test coverage
  - Depends on task 25
  - `tests/ca_cluster_node_test.cpp`: bring up a 3-node cluster (in-process or
    via subprocesses, following whichever pattern
    `raft_multi_node_test_fixture.hpp` already establishes for multi-node
    tests), each with `file_persistence` pointed at its own temp `data_dir`
  - Bootstrap via one node's `--bootstrap-ca`; confirm the other two converge
    to the same root cert without their own `--bootstrap-ca`
  - Issue a certificate against the leader; confirm a follower's
    `/v1/certificates` request gets `308` with a `Location` header pointing at
    the leader that succeeds when followed, and that a request made with no
    leader currently known gets `503` (Requirement 17.7)
  - Property 16: after bootstrap, grep every node's on-disk log/snapshot files
    under `data_dir` for a plaintext private-key PEM marker; assert none found
  - Property 17: issue a certificate, kill the leader process, wait for a new
    election, confirm the new leader's ledger contains the pre-failover
    issuance (e.g. by renewing it successfully against the new leader)
  - Kill and restart one follower pointed at the same `data_dir`; confirm it
    recovers without a full resync of already-committed entries
  - Use timeouts at the upper end of the project's "network tests" guideline
    (60–180 s) given this test involves multi-node elections and real disk I/O
  - _Requirements: 17.1, 17.4, 17.5, 17.7, 17.8_

- [ ] 27. Package/document 3-AZ AWS deployment for `ca_cluster_node`
  - Depends on task 25
  - `docker/ca_cluster_node/ecs-task-definitions/`: three sample ECS task
    definitions (or one parameterized template plus a note on the per-AZ
    values that differ), each pinned to a distinct subnet/AZ, each node's
    `--peers` listing all three nodes' RPC and HTTP addresses
  - `docker/ca_cluster_node/ca_cluster_node.service`: systemd unit analogous
    to task 14's, for the manual (non-ECS) 3-EC2-instance-in-3-AZs path
  - Document the automated alternative: an `aws_ec2_quorum_manager_config`
    snippet with three placement groups named by AZ (`target_count = 1`
    each, `subnet_by_group` per AZ) — no new code, since
    `aws_ec2_quorum_manager` already supports this shape (as exercised by
    `tests/aws_quorum_manager_unit_test.cpp`'s `ec2_construction` suite)
  - Verify: the three sample ECS task definitions/systemd units differ only in
    per-AZ values (subnet/AZ, `node_id`, `--peers`); a fresh reader can deploy
    a working 3-AZ cluster from the examples without guessing any value
  - _Requirements: 17.12_

- [ ] 28. Implement `acme_test_server` in `tests/acme_test_server.hpp`
  - Depends on task 1 (`certificate_authority`) only — no dependency on the
    ACME client, so this can be built and tested standalone first
  - Implement the RFC 8555 endpoints needed for the happy path: `/directory`,
    `HEAD /new-nonce`, `POST /new-account`, `POST /new-order`,
    `GET /authz/{id}`, `POST /challenge/{id}`, `POST /finalize/{order-id}`,
    `GET /cert/{id}`, each backed by an internal order/authorization/challenge
    state table (in-memory, keyed by opaque IDs) and a single owned
    `certificate_authority` instance that does all actual signing
  - Verify every JWS-signed request's signature against the calling account's
    registered public key (RFC 7515 compact serialization parsing +
    `EVP_DigestVerify`) before any state change; reject with the matching RFC
    8555 problem-document type (`badSignature`, `accountDoesNotExist`,
    `badNonce`) otherwise
  - Implement real challenge validation by default (`POST /challenge/{id}`
    triggers an outbound HTTP GET to the identifier's well-known URL for
    `http-01`, or a DNS TXT query for `dns-01`); add the
    `fiu_do_on("raft/acme/test_server/skip_challenge_validation", ...)` bypass
  - `directory_url()`/`root_certificate_pem()` accessors for use by
    `acme_certificate_provider_config` and test assertions
  - Verify: `tests/acme_test_server_unit_test.cpp` — malformed JWS, unknown
    account, nonce reuse, and the fault-injection bypass each produce the
    expected result
  - _Requirements: 18.7, 18.8_

- [ ] 29. Implement `acme_certificate_provider` in `include/raft/acme_certificate_provider.hpp` / `_impl.hpp`
  - Depends on task 8 (`certificate_provider` concept, `csr_signing_options`);
    the `dns_01` path additionally depends on the already-implemented
    `rfc2136_ldns_discovery` (no new task — reuse, not reimplementation)
  - Implement the JWS/JWK helpers (`detail::jws_sign`, base64url
    encode/decode, RFC 7638 JWK thumbprint) using OpenSSL `EVP_DigestSign`/
    `EVP_DigestVerify` and `boost::json`
  - Implement `sign_csr()`'s full sequence: directory → nonce → account
    (create or reuse) → order → per-authorization challenge handling → poll →
    finalize → poll → certificate download, per the design's component 22
    sketch
  - Implement the `http_01` responder (short-lived `httplib::Server` on
    `/.well-known/acme-challenge/<token>`) and the `dns_01` responder (reusing
    `rfc2136_ldns_discovery`'s UPDATE/sign/send helpers against
    `_acme-challenge.<identifier>.`); both torn down unconditionally once the
    authorization reaches a terminal state (Property 19)
  - `root_certificate_pem()`: return the top-most cert of the most recently
    downloaded chain; document the real-CA caveat from Requirement 18.6
  - `static_assert(certificate_provider<acme_certificate_provider>)`
  - Handle `badNonce` with a single automatic retry (RFC 8555 §6.5); surface
    every other problem-document error as a rejected future
  - Verify: `cmake --build build` succeeds; manual test against a running
    `acme_test_server` completes a full `http_01` issuance
  - _Requirements: 18.1–18.6, 18.10_

- [ ] 30. Add end-to-end and negative-path test coverage for ACME support
  - Depends on tasks 28 and 29
  - `tests/acme_certificate_provider_test.cpp`: full `http_01` and `dns_01`
    issuance against `acme_test_server` (the `dns_01` case needs a reachable
    RFC 2136 DNS server — reuse the BIND9 fixture already used by the
    `rfc2136_ldns_discovery` docker-chaos scenarios, or tag this case for the
    docker-chaos suite rather than the default `ctest` run if no local DNS
    server is available in every CI environment); asserts Property 18
    (chain-of-trust) and Property 19 (responder/record teardown, both on
    success and on a deliberately-failed challenge)
  - Negative paths: invalid JWS from a tampered provider (simulated via a
    test-only hook), a challenge that never validates (wrong token), and an
    order that expires before finalization — each asserted against
    `acme_test_server`'s corresponding problem-document response
  - Verify: `ctest --test-dir build -R acme` passes (docker-chaos-tagged
    `dns_01` case excluded from the default run if applicable, per above)
  - _Requirements: 18.7, 18.9_

- [ ] 31. Add LocalStack and real-EC2 test coverage for the 3-AZ `ca_cluster_node` AWS deployment
  - Depends on tasks 25 (`ca_cluster_node`) and 27 (3-AZ packaging artifacts);
    reuses the existing `aws_ec2_quorum_manager` unmodified — no new AWS
    provisioning code
  - `tests/ca_cluster_node_aws_localstack_test.cpp`: gated like
    `aws_quorum_manager_localstack_test.cpp`; configure
    `aws_ec2_quorum_manager` with the 3-AZ topology (one placement group per
    AZ, `target_count = 1`) against LocalStack; assert `RunInstances` is
    called once per AZ group with the correct subnet, and that terminating a
    simulated instance triggers a replacement `RunInstances` targeting the
    same AZ group. Document plainly in the test file's header comment that
    this tier validates provisioning-API behavior only — LocalStack does not
    run real compute, so it cannot exercise `ca_cluster_node` itself
  - `tests/ca_cluster_node_real_ec2_test.cpp`: opt-in, gated behind
    `$KYTHIRA_TEST_CA_CLUSTER_AMI` (skip when unset); provision 3 real EC2
    instances via `aws_ec2_quorum_manager` across 3 real AZs, each running
    `ca_cluster_node` via `user_data`; bootstrap, issue a certificate, then
    terminate one instance and assert: quorum survives on the remaining 2,
    `aws_ec2_quorum_manager` replaces the instance in the same AZ, the
    replacement rejoins with the identical CA identity via normal Raft
    catch-up, and the pre-termination issuance is still present in every
    node's ledger afterward (Property 17, demonstrated on real infrastructure)
  - Tag `ca_cluster_node_real_ec2_test` for exclusion from the default `ctest`
    run, matching `aws_quorum_manager_real_ec2_test.cpp`; note in the task
    that this test has real per-run AWS cost and should run on a
    scheduled/manual job, not per-PR
  - Verify: `ctest --test-dir build -R ca_cluster_node_aws_localstack` passes
    without real AWS credentials
  - _Requirements: 17.12_

## Notes

- Tasks 2 and 3 are independent of each other but both require task 1's
  `pem_material` shape to be stable first — implement task 1's public struct
  layout carefully since tasks 2, 3, 5, and 6 all consume it.
- `ca_service` deliberately does not use `temp_cert_files`: its output must
  outlive the process (it's read by other containers after `ca_service` exits),
  so it writes directly to the caller-supplied `--out-dir` with no RAII cleanup.
- The `mtls-node1`/`mtls-node2` stubs in `docker/ca-provisioning-compose.yml`
  are illustrative only — building an actual mTLS chaos test scenario that
  consumes this material is future work, tracked separately from this spec.
- Property-test verification (task 4) is the primary correctness gate for the
  whole framework: if `X509_verify_cert()` accepts what `issue()` produces and
  rejects what the negative presets produce, every downstream consumer (task 5
  migrations, task 6's `ca_service`) inherits that correctness for free.
- The CSR is the deliberate seam between "local" and "cloud" issuance (task 8):
  `sign_csr()`, `IssueCertificate` (task 10), and `POST /v1/certificates`
  (task 12) all take a CSR in and a certificate out, so nothing above
  `certificate_provider` needs to know or care which backend is signing.
- AWS ACM Private CA has an ongoing per-CA cost and its LocalStack emulation is
  Pro-only (task 11) — unlike the EC2/ASG quorum managers, there is no
  free/local way to exercise `aws_acm_pca_provider`'s success paths in default
  CI; the unit tier (fault-injection-forced error paths) is what runs by
  default, and the LocalStack/real tiers are opt-in.
- `ca_service --serve` (task 12) and the oneshot mode (task 6) share one
  binary and one `certificate_authority`/`certificate_provider` foundation;
  they are two `main()` code paths selected by whether `--serve` is present,
  not two executables.
- Task 17 (real TLS termination for `cpp_httplib_server`) was discovered, not
  planned: designing hot-reload (task 18) surfaced that `configure_ssl_server()`
  currently always throws "not fully implemented" and the server never
  actually runs `httplib::SSLServer`. Task 17 is a prerequisite fix to
  `http_transport` itself, not just CA-framework plumbing — it widens this
  spec's blast radius beyond `include/raft/certificate_authority*` into
  `include/raft/http_transport*` and its three existing SSL test files.
- Tasks 18/19 (hot-reload) and 20 (`ca_test_fixture::renew()`) are
  deliberately decoupled: neither transport's `reload_tls_material()` knows
  where the refreshed files came from, and `renew()` doesn't know who (if
  anyone) is polling for changes. Any atomic same-path file replacement — from
  `renew()`, from a real `ca_service` rotation, or from an operator's own
  script — is picked up the same way.
- Tasks 23–26 (`ca_cluster_node`) are a third, independent way to run the CA,
  alongside the single-process `ca_service --serve` (task 12) and the oneshot
  init-container (task 6) — all three share `certificate_authority` and the
  `certificate_provider`/CSR abstraction (task 8), but `ca_cluster_node` is the
  only one that survives a process crash without losing its root CA identity
  or issuance history, because its state lives in a replicated, disk-persisted
  Raft log rather than one process's memory. The load-bearing constraint
  throughout tasks 23–26 is that `ca_state_machine::apply()` must stay
  deterministic (Property 15) — every non-deterministic crypto operation
  (key generation, CSR signing) happens once on the leader, outside `apply()`,
  and only the finished result is ever committed to the log.
- `ca_cluster_node` does not replace `ca_service`; it's a heavier-weight
  option for deployments that need the CA itself to be fault-tolerant. A test
  suite that just needs valid certificates should keep using
  `ca_test_fixture`/`ca_service --serve` (tasks 15–16), which have no Raft
  cluster to stand up.
- 3 nodes / 3 AZs (task 27) is the documented default, not an enforced
  minimum or maximum — `ca_state_machine`/`ca_cluster_node` place no
  structural limit on cluster size. The automated AZ-spread path deliberately
  reuses `aws_ec2_quorum_manager` unmodified rather than adding a
  CA-specific provisioner, the same scope boundary already drawn for the
  single-process `ca_service` in Requirement 12/task 14.
- `acme_certificate_provider` (tasks 28–30) is Kythira's fourth
  `certificate_provider` implementation (after local, AWS ACM Private CA, and
  — architecturally, not literally the same interface — `ca_cluster_node`'s
  own signer). Task 28 (`acme_test_server`) is ordered first and depends only
  on task 1 specifically so ACME wire-protocol testing doesn't have to wait
  on the client half being done — the two can, in principle, be built by
  different people in parallel once task 1 and task 8 both exist, converging
  at task 30.
- The `dns_01` challenge path is a deliberate reuse, not a new DNS client:
  it calls into `rfc2136_ldns_discovery`'s existing UPDATE/sign/send code
  against a different record name. If that code needs a small refactor to
  expose a `detail::send_rfc2136_update()` usable from both call sites, do
  that refactor as part of task 29 rather than duplicating the UPDATE-packet
  construction logic.
- `ca_test_fixture` (tasks 15–16) is deliberately split into two tasks so that
  task 5's migration work can start as soon as the in-process mode exists
  (wave 3/4), without waiting on `ca_service --serve` (wave 7) to land first.
  Task 16 then upgrades the same fixture in place — call sites written against
  task 15 do not change when task 16 adds `start_network_service = true`.
