# Requirements Document

## Introduction

Kythira's HTTP and CoAP transports support TLS/mTLS and DTLS
(`cpp_httplib_client_config` / `cpp_httplib_server_config` in
`include/raft/http_transport.hpp`; `coap_client_config` / `coap_server_config` in
`include/raft/coap_transport.hpp`), but the test suite currently exercises them with
hand-authored placeholder PEM text. `http_ssl_certificate_loading_unit_test.cpp`,
`http_ssl_mutual_tls_integration_test.cpp`, and
`coap_certificate_validation_failure_property_test.cpp` embed base64 blobs whose
payload is repeated filler bytes (e.g. `J5J5J5...`), not a real key pair — they are
shaped like PEM but are not valid X.509 DER once decoded. Tests built on this
fixture either cannot complete a real TLS/DTLS handshake or only exercise
config-validation code paths, leaving mutual-TLS and DTLS certificate-validation
behavior effectively untested end-to-end.

This document specifies a Certificate Authority (CA) framework — an in-process
C++ library that generates real, cryptographically valid root CAs and leaf
certificates on demand — and a companion `ca_service` executable that provisions
matching certificate material to multiple containers in Docker/Podman-based test
scenarios. Together they let any test exercising a certificate-consuming feature
(HTTP mutual TLS, CoAP DTLS, and future features) obtain valid certificates for the
happy path, and precisely-shaped invalid ones (expired, not-yet-valid, wrong
hostname, untrusted issuer, revoked) for negative paths, without hand-maintaining
base64 fixtures.

The framework also extends beyond a single test process: `ca_service` can run as
a long-running, network-reachable service — in a docker-compose container, on a
cloud instance (e.g. EC2), or in a cloud container platform (e.g. ECS) — so Raft
nodes on separate hosts that don't share a filesystem volume can still obtain
certificates from a common CA. And because Kythira already integrates with AWS
for cluster provisioning (`aws_ec2_quorum_manager`, `aws_asg_quorum_manager`), the
framework defines a pluggable `certificate_provider` abstraction so the same
issuance call sites (including `ca_service`'s network API) can be backed by
either the local in-process CA or AWS Certificate Manager Private CA, following
the same optional-dependency and fault-injection conventions already used by the
existing AWS integrations.

## Glossary

- **CA**: Certificate Authority — an entity that signs certificates, establishing
  a trust relationship for anything holding the CA's own certificate.
- **Root CA**: A self-signed CA certificate/key pair; the trust anchor.
- **Leaf certificate**: An end-entity certificate issued and signed by a CA,
  presented by a server or client during a handshake.
- **SAN**: `subjectAltName` — the X.509v3 extension carrying the DNS names and/or
  IP addresses a certificate is valid for; modern TLS stacks validate the peer's
  identity against SAN, not the deprecated Common Name field.
- **EKU**: `extendedKeyUsage` — the X.509v3 extension restricting a certificate to
  specific purposes, e.g. `id-kp-serverAuth` or `id-kp-clientAuth`.
- **mTLS**: Mutual TLS — both server and client present certificates and validate
  each other's.
- **DTLS**: Datagram TLS — TLS over UDP, used by `coap_transport` when
  `enable_dtls` is set.
- **CRL**: Certificate Revocation List — a CA-signed list of revoked certificate
  serial numbers with revocation timestamps.
- **PEM**: Privacy-Enhanced Mail encoding — the base64-with-headers text format
  used to represent certificates and keys in the configs referenced above.
- **Init container**: A compose service that runs to completion and whose success
  (`service_completed_successfully`) gates the startup of dependent services —
  used here so a `ca_service` container can provision certificates before the
  nodes that need them start.
- **`certificate_authority`**: The framework's main class, described in
  Requirements 1–4.
- **`temp_cert_files`**: The RAII helper described in Requirement 5.
- **`ca_service`**: The standalone provisioning executable described in
  Requirement 7 (oneshot mode) and Requirement 11 (network-servable mode).
- **CSR**: Certificate Signing Request — a PEM-encoded request carrying a
  public key and subject information, submitted to a CA for signing. The
  requester's private key never leaves the requester.
- **`certificate_provider`**: The concept described in Requirement 9 that
  abstracts "something that turns a CSR into a signed certificate," satisfied
  by both `local_certificate_provider` and `aws_acm_pca_provider`.
- **ACM Private CA**: AWS Certificate Manager Private Certificate Authority —
  AWS's managed private PKI service, accessed via the `Aws::ACMPCA::ACMPCAClient`
  SDK client.
- **Bearer token**: A shared-secret HTTP `Authorization: Bearer <token>` header
  value used to authenticate callers of `ca_service`'s network API.
- **ECS**: Amazon Elastic Container Service — used in this document only as an
  illustrative cloud container target for a long-running `ca_service`.
- **`ca_test_fixture`**: The test-fixture class described in Requirement 13
  that bundles CA setup and client certificate issuance into two primitives.
- **Certificate pinning**: Trusting a TLS connection by comparing the
  server's presented certificate (or, here, the root of its chain) against a
  known-good fingerprint distributed out-of-band, instead of relying on
  normal chain-of-trust verification. Used in Requirement 19 to solve the
  bootstrap chicken-and-egg problem: a first-time caller has nothing signed
  by the CA yet to chain-verify against.
- **mDNS**: Multicast DNS (RFC 6762) — link-local, zero-configuration name
  resolution for `.local` hostnames, commonly provided by Avahi (Linux) or
  Bonjour/`mDNSResponder` (macOS). Already used elsewhere in this project by
  `poco_peer_discovery`. Relevant to Requirement 20's Case 1.
- **RFC 8738**: The ACME extension that adds `ip`-type identifiers (as
  opposed to RFC 8555's original `dns`-type only), validated via `http-01`
  or `tls-alpn-01` — never `dns-01`, since a bare IP address has no DNS zone
  to place a challenge record in. Relevant to Requirement 20's Case 2.
- **ACME**: Automatic Certificate Management Environment (RFC 8555) — the
  protocol Let's Encrypt and many other public CAs use. Domain control is
  proven via a challenge (`http-01` or `dns-01`) rather than by presenting an
  existing certificate (contrast with EST/EST-coaps, Requirement 15).
- **JWS / JWK**: JSON Web Signature (RFC 7515) / JSON Web Key (RFC 7517) — the
  signing format and key representation ACME uses to authenticate every
  request with the client's account key.
- **`acme_certificate_provider`**: The `certificate_provider` implementation
  described in Requirement 18 that speaks ACME to a real or test CA.
- **`acme_test_server`**: The in-process mock ACME server described in
  Requirement 18, used to test `acme_certificate_provider` without a real,
  rate-limited, publicly-routable CA.

## Requirements

### Requirement 1: Root CA generation

**User Story:** As a test author, I want to create a self-signed root CA
in-process with a single call, so that I can issue certificates for a test
without depending on an external CA or committing key material to the repo.

#### Acceptance Criteria

1. A `certificate_authority` class SHALL be provided in
   `include/raft/certificate_authority.hpp`. Constructing an instance SHALL
   generate a fresh, self-signed X.509v3 root CA certificate and key pair
   entirely in-process; construction SHALL perform no filesystem or network I/O.
2. `ca_options` SHALL configure the subject distinguished name, key algorithm
   (`rsa_2048`, `rsa_4096`, `ecdsa_p256`, `ecdsa_p384`; default `ecdsa_p256` for
   fast generation), and validity period (default 365 days). The generated root
   certificate SHALL carry `basicConstraints = critical, CA:TRUE` and
   `keyUsage = critical, keyCertSign, cRLSign`.
3. `root_certificate_pem()` SHALL return the root CA certificate as PEM text. No
   public accessor SHALL expose the CA's private key in PEM form — it is
   consumed internally for signing and never needs to leave the process.
4. Each `certificate_authority` instance SHALL draw serial numbers from a
   monotonically increasing counter seeded uniquely per instance (e.g. combining
   a high-resolution clock reading with the instance's address), so that two
   instances constructed concurrently — whether in the same process or in
   parallel `ctest` binaries — never emit colliding serial numbers for
   certificates with the same subject.
5. Construction and issuance SHALL be safe to call concurrently from multiple
   threads on the same instance (an internal mutex SHALL guard the non-reentrant
   parts of the underlying crypto calls), so the class can be shared by test
   fixtures that spin up multiple simulated nodes in parallel.

### Requirement 2: Leaf certificate issuance

**User Story:** As a test author, I want to issue server and client leaf
certificates signed by a test CA, with the SANs and key usages my scenario needs,
so that I can exercise real TLS/mTLS/DTLS handshakes instead of mocking them.

#### Acceptance Criteria

1. `certificate_authority::issue(leaf_certificate_options)` SHALL return a
   `pem_material` value containing a freshly generated private key and a leaf
   certificate signed by the calling instance's root CA. `pem_material` SHALL
   carry `certificate_pem`, `private_key_pem`, and `chain_pem` (the leaf
   certificate followed by the root certificate, PEM-concatenated, suitable for
   configs that expect a full chain file).
2. `leaf_certificate_options` SHALL configure: subject common name, key
   algorithm, a list of DNS SAN names, a list of IP SAN addresses, `server_auth`
   and `client_auth` booleans, `not_before`, and `validity`.
3. WHEN `server_auth` is `true`, the issued certificate's `keyUsage` SHALL
   include `digitalSignature, keyEncipherment` and its `extendedKeyUsage` SHALL
   include `id-kp-serverAuth`.
4. WHEN `client_auth` is `true`, the issued certificate's `extendedKeyUsage`
   SHALL additionally include `id-kp-clientAuth`. Both booleans MAY be `true` to
   produce a dual-purpose certificate.
5. IF both `dns_names` and `ip_addresses` are empty THEN `issue()` SHALL throw
   `std::invalid_argument` — a leaf certificate with no SAN entries cannot be
   validated against a hostname by any modern TLS stack, so this is treated as a
   caller error rather than a silently-produced unusable fixture.
6. Issued leaf certificates SHALL carry `basicConstraints = critical, CA:FALSE`.

### Requirement 3: Negative-scenario presets

**User Story:** As a test author, I want ready-made ways to produce a certificate
that fails validation for a specific, well-known reason, so that I can write
negative tests (expired, not-yet-valid, wrong issuer) without hand-crafting
invalid PEM content.

#### Acceptance Criteria

1. `certificate_authority::issue_expired(leaf_certificate_options)` SHALL
   produce a `pem_material` whose `notBefore` and `notAfter` are both in the
   past (default: issued 2 days ago, expired 1 day ago), otherwise following the
   same options handling as `issue()`.
2. `certificate_authority::issue_not_yet_valid(leaf_certificate_options)` SHALL
   produce a `pem_material` whose `notBefore` is in the future (default:
   +1 day), otherwise following the same options handling as `issue()`.
3. Two independently constructed `certificate_authority` instances SHALL by
   default draw distinct subject distinguished names and independent key
   material, so that a leaf certificate issued by one instance fails chain
   validation against a trust store rooted at the other instance's root
   certificate — this SHALL be sufficient to construct "untrusted issuer" test
   scenarios without a dedicated preset method.
4. Wrong-hostname scenarios SHALL be achievable using `issue()` alone, by
   supplying `dns_names`/`ip_addresses` that deliberately do not match the
   address under test; no dedicated preset method is required for this case.

### Requirement 4: Revocation and CRL

**User Story:** As a test author, I want to revoke a certificate I previously
issued and obtain an up-to-date CRL, so that I can test that revocation checking
actually rejects a revoked peer instead of merely asserting on a hardcoded
"revoked" string.

#### Acceptance Criteria

1. `certificate_authority::revoke(const pem_material&)` SHALL mark the
   certificate identified by that `pem_material`'s serial number as revoked, with
   a revocation time of "now".
2. `certificate_authority::crl_pem()` SHALL return a PEM-encoded, CA-signed CRL
   listing every revoked serial number and its revocation time. Calling it again
   after a subsequent `revoke()` SHALL return a CRL reflecting the updated
   revoked set.
3. WHEN `revoke()` is called with a `pem_material` whose serial number was not
   issued by this `certificate_authority` instance, it SHALL throw
   `std::invalid_argument`.

### Requirement 5: PEM delivery to file-path-based configuration

**User Story:** As a test author, I want to turn generated PEM material into
files on disk without managing cleanup myself, so that I can populate
`cpp_httplib_server_config::ssl_cert_path` / `coap_server_config::cert_file` and
similar fields directly.

#### Acceptance Criteria

1. A `temp_cert_files` RAII class SHALL be provided, constructed from a
   `pem_material`. It SHALL materialize the non-empty fields of
   (`certificate_pem`, `private_key_pem`, `chain_pem`) as files under a
   uniquely-named directory beneath the system temporary directory.
2. The private key file SHALL be created with owner-only read/write permissions
   (mode `0600`).
3. The unique directory name SHALL incorporate a per-instance uniquifier (e.g.
   the certificate's serial number combined with a high-resolution timestamp),
   so that parallel `ctest` invocations (`-j$(nproc)`) never collide on the same
   path.
4. The destructor SHALL remove the directory and all files it created,
   best-effort — filesystem errors during cleanup SHALL be swallowed and SHALL
   NOT propagate out of the destructor.
5. `cert_path()`, `key_path()`, and `chain_path()` accessors SHALL return
   absolute file paths (the last returning an empty string if no chain material
   was provided), directly assignable to the transport config fields named
   above.

### Requirement 6: Adoption in existing certificate-consuming tests

**User Story:** As a maintainer, I want the tests that currently rely on fake
placeholder PEM content to use real, valid certificates from the new framework,
so that mutual-TLS and DTLS code paths are exercised with material capable of
completing an actual handshake.

#### Acceptance Criteria

1. `tests/http_ssl_mutual_tls_integration_test.cpp` SHALL be migrated to obtain
   its server certificate, client certificate, and CA certificate from a
   `certificate_authority` + `temp_cert_files` pair instead of the hardcoded
   placeholder PEM constants, so the mutual-TLS handshake it exercises uses
   cryptographically valid material.
2. `tests/coap_certificate_validation_failure_property_test.cpp` and
   `tests/coap_dtls_handshake_property_test.cpp` SHALL be migrated similarly for
   their valid-certificate baseline cases. Property-test cases that specifically
   assert on malformed or truncated PEM content SHALL retain their hand-authored
   invalid strings — those test parser robustness, not the CA framework, and are
   out of scope for migration.
3. `tests/http_ssl_certificate_loading_unit_test.cpp`'s `expired_cert_pem`
   fixture SHALL be replaced by the output of
   `certificate_authority::issue_expired()`.
4. Existing assertions that check only structural/error-path behavior (e.g.
   "loading an unparseable string throws") SHALL be preserved unchanged — only
   the *valid*-certificate fixtures are replaced by framework output.

### Requirement 7: `ca_service` provisioning executable

**User Story:** As a test author writing a Docker/Podman-based multi-container
scenario (in the style of the existing `docker_chaos` DNS-discovery scenarios), I
want a container that provisions a shared root CA and per-service leaf
certificates before the scenario's nodes start, so that every container gets
matching, mutually-trusted certificate material without a shared secrets file
checked into the repo.

#### Acceptance Criteria

1. `ca_service` SHALL be provided as `cmd/ca_service/main.cpp`, built as an
   independent executable (following the `cmd/<name>/{CMakeLists.txt,main.cpp}`
   layout already used by `cmd/dns_discovery_node` etc.), linked against the
   `certificate_authority` library.
2. It SHALL accept one or more repeatable `--service <name>[:alt1,alt2,...]`
   arguments, a required `--out-dir <path>` argument, and optional `--domain
   <suffix>`, `--validity-days <n>` (default 30), and `--resolve-ips` flags.
3. For each `--service` entry it SHALL issue one dual-purpose (`server_auth` and
   `client_auth`) leaf certificate whose DNS SANs are the service name
   (optionally suffixed with `--domain`) plus any `alt` names given. WHEN
   `--resolve-ips` is passed, it SHALL additionally resolve each DNS name via
   `getaddrinfo` at run time and add the resulting addresses as IP SAN entries —
   consistent with the project's rule against embedding static/pre-known IP
   addresses in container test infrastructure. Without `--resolve-ips`, no IP
   SAN entries SHALL be added.
4. Output under `--out-dir` SHALL be laid out as `root_ca.pem` at the top level,
   and one subdirectory per `--service` entry containing `cert.pem`, `key.pem`,
   and `chain.pem`.
5. `ca_service` SHALL run to completion and exit `0` on success. On any failure
   (invalid arguments, an underlying crypto error, or an unwritable output
   directory) it SHALL print a diagnostic message to `stderr` and exit non-zero.
   It SHALL NOT run as a long-lived daemon or open any network listener.
6. `docker/ca_service/Dockerfile` SHALL build the `ca_service` executable
   following the two-stage build pattern used by the existing
   `docker/dns_discovery_node/Dockerfile`.
7. `docker/ca-provisioning-compose.yml` SHALL demonstrate a `ca-service`
   container writing its output to a named volume, plus two dependent
   node-service stubs that mount that volume read-only and declare
   `depends_on: ca-service: condition: service_completed_successfully`. No
   static IP addresses SHALL be configured in this file; inter-container
   addressing SHALL be by compose service name, and the file SHALL work
   unmodified under both Docker and rootless Podman per the project's container
   runtime compatibility rules.

### Requirement 8: Build integration

**User Story:** As a maintainer, I want the CA framework to build (or cleanly not
build) using the same optional-dependency conventions already used for libldns
and Poco DNSSD, so it doesn't force a new hard dependency on environments that
don't have OpenSSL.

#### Acceptance Criteria

1. The top-level `CMakeLists.txt` SHALL expose a `certificate_authority` library
   target, built only when `OpenSSL::SSL` is available (the project already runs
   `find_package(OpenSSL QUIET)`), and SHALL define `KYTHIRA_HAS_OPENSSL` for
   consumers of that target — mirroring the existing `KYTHIRA_HAS_LDNS` /
   `KYTHIRA_HAS_POCO_DNSSD` pattern.
2. WHEN OpenSSL is not detected, the `certificate_authority` target, the
   `ca_service` executable, and any test target that depends on them SHALL
   simply not be defined; the rest of the build SHALL succeed unaffected.
3. `DEPENDENCIES.md` SHALL gain an entry documenting the OpenSSL requirement for
   this component (the version already required elsewhere in the project:
   OpenSSL 3.x, detected via `find_package(OpenSSL QUIET)`).
4. A separate `find_package(AWSSDK QUIET COMPONENTS acm-pca)` check SHALL define
   `KYTHIRA_HAS_AWS_ACM_PCA` independently of `KYTHIRA_HAS_AWS_SDK` (which
   already guards `aws_ec2_quorum_manager`/`aws_asg_quorum_manager` via `core
   ec2 autoscaling iam s3 sts`). Environments with the core AWS SDK but without
   the `acm-pca` component SHALL still build everything except
   `aws_acm_pca_provider`.
5. `vcpkg.json`'s existing `aws-sdk-cpp` port entry SHALL gain `"acm-pca"` in
   its `features` list.

### Requirement 9: `certificate_provider` concept and CSR-based signing

**User Story:** As a developer, I want a common interface for "something that
turns a CSR into a signed certificate," so that tests and the network
`ca_service` API can swap between the local in-process CA and a cloud-hosted CA
without changing call sites.

#### Acceptance Criteria

1. `certificate_authority` SHALL gain
   `sign_csr(std::string csr_pem, csr_signing_options options) -> pem_material`.
   It SHALL parse the CSR's embedded public key and subject, apply
   SAN/keyUsage/EKU/validity from `csr_signing_options` (the same fields as
   `leaf_certificate_options` minus the key-algorithm selector, since the key
   already exists inside the CSR), sign the resulting certificate with the CA's
   key, and return a `pem_material` whose `private_key_pem` field is empty —
   the CA never sees the requester's private key.
2. A free function `generate_key_and_csr(leaf_certificate_options options) ->
   csr_material` (where `csr_material` is `{private_key_pem, csr_pem}`) SHALL be
   provided in `include/raft/certificate_authority.hpp`, so any caller can
   generate a key pair and CSR locally without needing a `certificate_authority`
   instance at all.
3. A `certificate_provider` concept SHALL be defined in
   `include/raft/certificate_provider.hpp`, requiring, for an lvalue `P& p`,
   `std::string csr_pem`, and `csr_signing_options options`:
   `{ p.root_certificate_pem() } -> std::same_as<kythira::Future<std::string>>`
   and
   `{ p.sign_csr(csr_pem, options) } -> std::same_as<kythira::Future<pem_material>>`.
4. A `local_certificate_provider` adapter class SHALL be provided, wrapping a
   `certificate_authority&` (non-owning reference) and satisfying
   `certificate_provider`, returning immediately-resolved futures around the
   underlying synchronous `certificate_authority` calls — verified by
   `static_assert(certificate_provider<local_certificate_provider>)`.
5. `issue()` / `issue_expired()` / `issue_not_yet_valid()` (Requirements 2–3)
   SHALL remain unchanged and SHALL continue to generate the leaf's key
   in-process; `sign_csr()` is an additive capability, not a replacement.

### Requirement 10: AWS ACM Private CA integration

**User Story:** As an operator who already has a private CA provisioned in AWS
Certificate Manager, I want Kythira's tests and tools to request certificates
from it using the same AWS SDK integration conventions as the existing
EC2/ASG quorum managers, so certificates trusted outside the test process can be
obtained the same way local test certificates are.

#### Acceptance Criteria

1. `aws_acm_pca_provider` SHALL be provided in
   `include/raft/aws_acm_pca_provider.hpp` / `_impl.hpp`, compiled only when
   `KYTHIRA_HAS_AWS_ACM_PCA` is defined (Requirement 8.4), and SHALL satisfy
   `certificate_provider<aws_acm_pca_provider>`, verified by a `static_assert`.
2. `aws_acm_pca_provider_config` SHALL embed the existing `aws_client_config`
   (`region`, `endpoint_override`, `api_timeout`, `credentials_provider`) plus
   `certificate_authority_arn` (the ARN of a pre-existing ACM Private CA),
   `template_arn` (default
   `"arn:aws:acm-pca:::template/EndEntityCertificate/V1"`),
   `signing_algorithm` (default `"SHA256WITHRSA"`), and `validity`. This
   component SHALL NOT create or delete a Private CA — provisioning one is an
   out-of-band operator action, given its ongoing per-CA cost.
3. `root_certificate_pem()` SHALL call `GetCertificateAuthorityCertificate` for
   `certificate_authority_arn` and return the CA certificate PEM, cached after
   the first successful call.
4. `sign_csr(csr_pem, options)` SHALL call `IssueCertificate` with the CSR
   bytes, `template_arn`, `signing_algorithm`, and validity. Because ACM Private
   CA issuance is asynchronous, it SHALL then poll `GetCertificate` — bounded by
   `api_timeout` total, with backoff between polls — until the certificate is
   available or the timeout elapses, and return
   `pem_material{certificate_pem, "", chain_pem}`, where `chain_pem` is the
   issued certificate concatenated with the response's `CertificateChain`.
5. Each AWS API call SHALL be wrapped in a
   `fiu_do_on("raft/aws/acm_pca/<operation>", ...)` fault-injection point,
   following the pattern already used by `aws_ec2_quorum_manager`.
6. AWS API errors (throttling, access denied, CA in an invalid state) SHALL
   reject the returned future with an exception carrying the AWS error message;
   they SHALL NOT be silently swallowed.
7. `revoke(serial)` MAY be provided, calling ACM Private CA's
   `RevokeCertificate`. Because that API requires the target CA to already have
   a CRL or OCSP configuration (an out-of-band operator setup, like the CA ARN
   itself), a call against a CA without one configured SHALL surface the
   resulting AWS error to the caller rather than falling back to any local
   behavior.

### Requirement 11: `ca_service --serve` network API mode

**User Story:** As a developer running Raft nodes on separate hosts — including
across a cloud-instance boundary where no shared filesystem volume exists — I
want to fetch certificates from a running `ca_service` over the network instead
of only from a shared docker-compose volume, so nodes on different hosts can
still obtain certificates trusted by a common CA.

#### Acceptance Criteria

1. `ca_service` SHALL accept a `--serve <bind-address>:<port>` argument, mutually
   exclusive with the existing oneshot `--out-dir` mode (Requirement 7). WHEN
   `--serve` is given, `--out-dir` and `--service` SHALL be rejected with a
   usage error.
2. WHEN in serve mode, `ca_service` SHALL accept a `--provider {local|aws-acm-pca}`
   argument (default `local`) selecting the `certificate_provider`
   implementation from Requirements 9–10. `aws-acm-pca` mode SHALL additionally
   accept `--acm-pca-arn`, `--aws-region`, and `--aws-endpoint-override`
   arguments, mapped onto `aws_acm_pca_provider_config`.
3. It SHALL expose an HTTP API built directly on the vendored `httplib::Server`
   (independent of the raft-RPC-specific `http_transport` abstraction), with:
   - `GET /healthz` → `200 OK` once the selected provider is ready to serve
     requests.
   - `GET /v1/root-ca` → `200` with the provider's root/CA certificate PEM as
     the body.
   - `POST /v1/certificates` → body is a CSR PEM plus JSON metadata
     (`dns_names`, `ip_addresses`, `server_auth`, `client_auth`,
     `validity_days`); on success returns `200` with JSON body
     `{"certificate_pem": "...", "chain_pem": "..."}`.
   - `POST /v1/certificates/revoke` → body `{"serial": "..."}`. Under the
     `local` provider it SHALL revoke via the underlying `certificate_authority`;
     under `aws-acm-pca` without a CRL/OCSP configured (Requirement 10.7) it
     SHALL return `501 Not Implemented`.
   - `GET /v1/crl` → `local` provider only; returns the current CRL PEM.
     `501 Not Implemented` under `aws-acm-pca`.
4. Every request SHALL require an `Authorization: Bearer <token>` header
   matching `--auth-token` (or `$CA_SERVICE_AUTH_TOKEN`); requests without a
   matching token SHALL receive `401 Unauthorized`. `--serve` mode SHALL refuse
   to start if no token is configured (fail closed), since it may be reachable
   outside a single docker-compose network.
5. `--serve` mode SHALL optionally accept `--tls-cert`/`--tls-key` to terminate
   TLS itself (material for local/dev use can come from the same
   `certificate_authority`). WHEN neither is given, it SHALL serve plain HTTP
   and log a warning that the endpoint is unencrypted — suitable only for a
   private network (e.g. inside one docker-compose network), not for the
   cross-host cloud use covered by Requirement 12.
6. `--serve` mode SHALL run until terminated by `SIGINT`/`SIGTERM`, shutting
   down the HTTP listener cleanly.

### Requirement 12: Cloud and long-running-container deployment packaging

**User Story:** As an operator, I want to run `ca_service --serve` as a
persistent service on a cloud VM or in a long-running container — not just as a
docker-compose init container — so hosts outside a single compose network can
still obtain certificates from a common CA.

#### Acceptance Criteria

1. `docker/ca_service/Dockerfile` (built for the oneshot mode in
   Requirement 7.6) SHALL be reused unmodified for serve mode: the same image
   SHALL support both the default oneshot `ENTRYPOINT` invocation and an
   overridden command running `ca_service --serve ...`. No second image SHALL
   be introduced.
2. `docker/ca-service-server-compose.yml` SHALL demonstrate the long-running
   mode: a `ca-service` container running `--serve 0.0.0.0:8443 --provider
   local`, with a `HEALTHCHECK` against `/healthz`, addressed by compose
   service name on its network (no static IP) — consumable by node containers
   on the same network, or, when the port is published, by hosts outside it.
3. A sample systemd unit file, `docker/ca_service/ca_service.service`, SHALL be
   provided showing `ca_service --serve` run directly on a Linux host (e.g. an
   EC2 instance): `ExecStart` invoking the built binary, `Restart=on-failure`,
   and an `EnvironmentFile` supplying `CA_SERVICE_AUTH_TOKEN` and (when
   `--provider aws-acm-pca` is used) the AWS region and CA ARN.
4. WHEN `--provider aws-acm-pca` is used on an EC2 instance, credential
   resolution SHALL rely on the same default AWS credentials provider chain
   already used by `aws_ec2_quorum_manager` / `aws_asg_quorum_manager`
   (instance-profile / IAM role). No new credential-handling code SHALL be
   introduced for this component.
5. A sample ECS task definition, `docker/ca_service/ecs-task-definition.json`,
   SHALL be provided illustrating deployment of the same container image as a
   long-running ECS service/task, with a task role granting the minimum ACM
   Private CA permissions needed
   (`acm-pca:GetCertificateAuthorityCertificate`, `acm-pca:IssueCertificate`,
   `acm-pca:GetCertificate`) when `--provider aws-acm-pca` is used.
6. Provisioning the EC2 instance or ECS cluster itself (auto-scaling, load
   balancing, DNS registration for the deployed `ca_service`) is explicitly OUT
   OF SCOPE for this spec. Requirement 12 delivers example configuration
   artifacts an operator applies manually or via their own infrastructure
   tooling, not an automated provisioner — unlike `aws_ec2_quorum_manager`,
   which actively provisions Raft-node instances, `ca_service` is treated as
   pre-existing supporting infrastructure that tests and nodes point at.

### Requirement 13: Test fixture primitives for CA setup and client bootstrapping

**User Story:** As a test author, I want one primitive that sets up a
certificate authority for my test and another that hands me a ready-to-use,
signed certificate for a client under test, so that wiring up TLS/mTLS/DTLS for
a test scenario doesn't require re-deriving the CA-construct → issue →
materialize-to-files sequence by hand every time.

#### Acceptance Criteria

1. A `ca_test_fixture` class SHALL be provided in `tests/ca_test_fixture.hpp`
   (following the convention of other shared test fixtures living directly
   under `tests/`, e.g. `raft_multi_node_test_fixture.hpp`), constructible with
   zero required arguments. Construction SHALL set up a certificate authority
   ready for issuance — by default an in-process `certificate_authority`
   (Requirement 1), with no filesystem or network I/O — mirroring the
   setup-on-construction pattern already used by fixtures such as
   `LocalNetworkFixture`.
2. `ca_test_fixture::options` SHALL allow opting into a real, network-reachable
   CA: WHEN `start_network_service` is set, the constructor SHALL launch the
   `ca_service` executable (Requirement 11) as a child process in `--serve`
   mode bound to an ephemeral local port, and SHALL block until `/healthz`
   reports ready or a bounded startup timeout elapses (throwing
   `std::runtime_error` on timeout). The destructor SHALL terminate that child
   process.
3. `ca_test_fixture::bootstrap_client(client_id, dns_names, ip_addresses = {},
   server_auth = true, client_auth = true, validity = 30 days)` SHALL issue one
   leaf certificate for the given identity and return a ready-to-use
   `temp_cert_files` (Requirement 5) — obtained via the in-process
   `certificate_authority` when no network service was started, or via an HTTP
   call to the running `ca_service`'s `/v1/certificates` route (submitting a
   CSR generated locally with `generate_key_and_csr`, per Requirement 9, so the
   private key never leaves the caller) when one was. The result SHALL be
   directly usable to populate `cpp_httplib_server_config` /
   `cpp_httplib_client_config` / `coap_server_config` / `coap_client_config`
   path fields.
4. The fixture SHALL retain ownership of every `temp_cert_files` it has handed
   out via `bootstrap_client()` for its own lifetime, so a test that keeps only
   the path strings from a returned reference does not have its certificate
   files deleted out from under a still-running test.
5. `ca_test_fixture::root_certificate_pem()` SHALL return the CA's root
   certificate PEM regardless of whether the fixture is running in-process or
   via a network service, so a test can populate a peer's trust store
   (`ca_cert_path` / `ca_file`) without depending on which mode produced it.
6. Calling `bootstrap_client()` with two different `client_id` values SHALL
   produce two certificates that both chain-verify against the same
   `root_certificate_pem()`, with distinct subjects and independent key
   material such that a peer holding one cannot be mistaken for or impersonate
   the other. This SHALL be covered by a property test.

### Requirement 14: Real TLS termination for `cpp_httplib_server`

**User Story:** As a maintainer, I want `cpp_httplib_server` to actually
terminate TLS when `enable_ssl` is set, instead of validating configuration and
then throwing "not fully implemented," so that HTTP mutual TLS is a real,
working code path — and so certificate hot-reload (Requirement 16) has a live
TLS context to reload into.

This requirement exists because implementing hot-reload surfaced a pre-existing
gap: `cpp_httplib_server` currently constructs a plain `httplib::Server` (never
`httplib::SSLServer`), and `configure_ssl_server()` unconditionally throws
`kythira::ssl_configuration_error("...cpp-httplib SSL server integration is
not fully implemented...")` whenever `enable_ssl` is `true`, after validating
configuration against a scratch `SSL_CTX*` that is immediately discarded. The
existing tests tolerate this by catching `ssl_configuration_error` with a
comment noting it as an expected outcome.

#### Acceptance Criteria

1. WHEN `cpp_httplib_server_config::enable_ssl` is `true`, `cpp_httplib_server`
   SHALL construct and run an `httplib::SSLServer` (not `httplib::Server`) as
   its underlying listener, consuming `ssl_cert_path`/`ssl_key_path`.
2. The existing validation helpers (`validate_certificate_key_pair`,
   `configure_ssl_context`, cipher-suite and TLS-version-range validation)
   SHALL be reused rather than duplicated, and SHALL now apply to the
   `SSL_CTX*` actually used by the running server (obtained via
   `httplib::SSLServer::ssl_context()`), not a scratch context that is
   discarded after validation.
3. WHEN `require_client_cert` is `true`, the live `SSL_CTX*` SHALL have
   `SSL_CTX_load_verify_locations(ca_cert_path)` applied and
   `SSL_CTX_set_verify(SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, ...)`
   set — the same policy today's discarded validation context already
   computes; this requirement is a wiring fix, not a new policy.
4. `configure_ssl_server()` SHALL no longer unconditionally throw
   `ssl_configuration_error` announcing incomplete implementation; it SHALL
   throw only for genuine configuration problems (missing/mismatched
   cert-key pair, invalid cipher suite, missing CA path when
   `require_client_cert` is set), exactly as today's validation logic already
   distinguishes.
5. `tests/http_ssl_mutual_tls_integration_test.cpp`,
   `tests/http_ssl_configuration_property_test.cpp`, and
   `tests/http_ssl_context_configuration_unit_test.cpp` SHALL be updated to
   assert successful construction and a completed handshake for valid
   configurations, using material from `ca_test_fixture` (Requirement 13); the
   branches that currently tolerate `ssl_configuration_error` as an expected
   outcome for "SSL server not fully implemented" SHALL be removed for the
   cases they were masking.
6. `cpp_httplib_client`'s existing TLS behavior (already functional via
   `httplib::Client`'s built-in OpenSSL support for `https://` URLs) is
   unaffected by this requirement — no client-side change is required to
   achieve a working mutual-TLS round trip once the server side is fixed.

### Requirement 15: Certificate renewal primitives

**User Story:** As a test author, or an operator running `ca_service --serve`
long-term, I want a way to obtain a renewed certificate for an identity I
previously bootstrapped — and, modeled on how EST (RFC 7030) and EST-coaps
(RFC 9148) authenticate a re-enrollment request using the caller's own current
certificate — a way to do so without re-proving identity via a separate shared
secret, so renewal doesn't require re-running whatever out-of-band process
issued the original certificate.

#### Acceptance Criteria

1. `ca_test_fixture::renew(client_id)` SHALL re-issue a certificate for a
   `client_id` previously passed to `bootstrap_client()`, using the exact same
   options (`dns_names`, `ip_addresses`, `server_auth`, `client_auth`,
   `validity`) captured at that call, and SHALL atomically replace the
   material at the same `temp_cert_files` paths previously returned for that
   `client_id` — write to a sibling temp file in the same directory, then
   `std::filesystem::rename` over `cert.pem`/`key.pem`/`chain.pem` (an atomic
   same-filesystem replace, so a concurrent reader never observes a
   partially-written file).
2. `renew()` called with a `client_id` never passed to `bootstrap_client()`
   SHALL throw `std::invalid_argument`.
3. `ca_service --serve` SHALL expose `POST /v1/certificates/renew`, modeled on
   RFC 7030 §4.2.2 (`simplereenroll`) / RFC 9148 (`/est/sren`): the request
   SHALL be authenticated by presenting the caller's current (about-to-expire)
   client certificate over mutual TLS, rather than the `Authorization: Bearer`
   token required by every other route. The returned certificate SHALL carry
   the same subject and SAN entries as the presented certificate (a renewal,
   not an identity change) with a fresh validity window. This route is
   explicitly EST-*inspired*, not a claim of RFC 7030/9148 protocol
   compliance — it does not implement CMC encoding, CSR attribute negotiation,
   or EST's other resources (`/cacerts`, `/simpleenroll`, full ASN.1 message
   framing).
4. `POST /v1/certificates/renew` SHALL reject with `401` a request whose
   presented client certificate does not chain-verify against the active
   provider's own root — a caller cannot use this route to move from "holds a
   cert from this CA" to "obtains a cert for an unrelated identity" by
   presenting a certificate from elsewhere.
5. For the `aws-acm-pca` provider, `POST /v1/certificates/renew` MAY simply
   perform a fresh `IssueCertificate` call using the presented certificate's
   subject/SAN — ACM Private CA has no distinct renewal API, matching how
   ACME (RFC 8555) itself treats renewal as "just issue again" rather than as
   a dedicated operation.

### Requirement 16: Hot-reload certificate support for `http_transport` and `coap_transport`

**User Story:** As an operator running Kythira nodes for longer than a
certificate's validity period, I want the running transport to pick up a
renewed certificate without dropping existing connections or restarting the
node, so certificate rotation doesn't require a maintenance window.

#### Acceptance Criteria

1. `cpp_httplib_server` (depends on Requirement 14 for a live TLS context)
   SHALL provide `reload_tls_material()`, which re-reads
   `ssl_cert_path`/`ssl_key_path`/`ca_cert_path` from disk and applies them to
   the live `SSL_CTX*` (`SSL_CTX_use_certificate_chain_file`,
   `SSL_CTX_use_PrivateKey_file`, and, when `require_client_cert`,
   `SSL_CTX_load_verify_locations`) without closing the listening socket or
   dropping any established connection.
2. `coap_server` (and `coap_client`, for its own presented certificate under
   mutual DTLS) SHALL provide the equivalent `reload_tls_material()`,
   re-invoking `coap_context_set_pki()` on the existing, live `_coap_context`
   with refreshed file paths, without tearing down the context or any
   existing DTLS association.
3. `reload_tls_material()` on both transports SHALL validate the new material
   before applying it to the live context. WHEN the new cert/key pair is
   invalid, mismatched, or unreadable, the call SHALL throw and the transport
   SHALL continue serving with its previous, still-valid material — reload is
   all-or-nothing, with no partially-applied state.
4. New TLS/DTLS handshakes initiated after a successful
   `reload_tls_material()` call SHALL present the newly loaded certificate.
   Handshakes/sessions already established before the call SHALL be
   unaffected and SHALL NOT be forcibly re-negotiated or dropped by the
   reload itself.
5. Both transports SHALL optionally support
   `enable_auto_reload(std::chrono::seconds poll_interval)`, starting a
   background thread that periodically checks the configured cert file's
   modification time and calls `reload_tls_material()` when it has changed
   since the last successful reload — turning a `ca_test_fixture::renew()` (or
   `ca_service`-driven) file replacement into unattended rotation with no
   caller-side polling loop required.
6. `disable_auto_reload()` SHALL stop the background thread cleanly (joined,
   not detached) before returning.
7. A failed automatic reload attempt (the Requirement 16.3 failure case,
   triggered from the background thread) SHALL be reported via the existing
   `metrics_type`/logger mechanism already used elsewhere in each transport,
   and SHALL NOT terminate the poll loop — a transient bad read (e.g. a
   rotation source that doesn't replace files atomically, despite
   Requirement 15.1's design) SHALL be retried on the next poll rather than
   permanently disabling reload.

### Requirement 17: CA state as a replicated Kythira Raft state machine, persisted to disk

**User Story:** As an operator, I want the CA service's state — its root CA
material, its issuance ledger, and its revocation list — replicated across a
Kythira Raft cluster and persisted to disk, so the CA survives node failures
and restarts without losing its identity or its issuance history, using
Kythira's own consensus and persistence machinery rather than a single
process's memory (the gap identified when describing `certificate_authority`'s
state: today it is entirely in-memory, and a `ca_service --serve` restart
forgets the CA key/cert and every certificate it ever issued).

#### Acceptance Criteria

1. `ca_state_machine` SHALL be provided in `include/raft/ca_state_machine.hpp`,
   implementing Kythira's `state_machine` concept (`apply`, `get_state`,
   `restore_from_snapshot`, as defined in `include/raft/types.hpp`). Per that
   concept's determinism requirement, `apply()` SHALL only record
   already-computed facts supplied within the command bytes — it SHALL NOT
   generate key material, compute a certificate signature, or invoke any other
   non-deterministic cryptographic primitive during `apply()`. Every replica
   applying the same command bytes SHALL reach identical state.
2. Three command types SHALL be supported, boost::json-encoded consistent with
   the encoding `file_persistence.hpp` already uses for log entries:
   - `bootstrap_ca` — the root CA certificate PEM plus its private key,
     encrypted per Requirement 17.4. Committed at most once per cluster
     lifetime; `apply()` SHALL deterministically reject (returning an error
     result, not throwing) a second `bootstrap_ca` command once root material
     already exists.
   - `record_issuance` — serial, subject, SAN entries, validity window, and
     the full issued certificate PEM. The issued leaf's *private key* SHALL
     NEVER appear in this command or anywhere else in the replicated log.
   - `record_revocation` — serial and revoked-at timestamp.
3. `get_state()`/`restore_from_snapshot()` SHALL (de)serialize the full ledger
   (every recorded issuance and revocation) plus the encrypted bootstrap
   material, so a newly joined node obtains identical state via Kythira's
   existing snapshot-installation path — no CA-specific bootstrap-transfer
   mechanism is needed.
4. The CA's private key SHALL be encrypted (AES-256-GCM, key derived via
   PBKDF2 from an operator-supplied passphrase) before being included in the
   `bootstrap_ca` command, so the value committed to the Raft log — and
   therefore written to disk by the persistence layer — never contains the
   private key in plaintext. Every node in the cluster SHALL be configured
   with the same passphrase out-of-band (e.g. `--unseal-key-file` or
   `$CA_CLUSTER_UNSEAL_KEY`), analogous to how HashiCorp Vault's
   integrated-storage nodes are each configured with the same unseal
   material. Losing this passphrase makes the persisted CA key unrecoverable;
   this SHALL be documented plainly, not silently risked.
5. A `ca_cluster_node` executable SHALL be provided
   (`cmd/ca_cluster_node/main.cpp`, following the
   `cmd/<name>/{CMakeLists.txt,main.cpp}` convention already used by
   `cmd/chaos_node`/`cmd/dns_discovery_node`), wiring together a Kythira
   `node<Types>` (Raft consensus), `ca_state_machine`, and `file_persistence`
   (the existing file-backed persistence engine already used by
   `cmd/chaos_node`) for on-disk log and snapshot storage.
6. `ca_cluster_node` SHALL expose the same client-facing HTTP API as
   `ca_service --serve` (`/healthz`, `/v1/root-ca`, `/v1/certificates`,
   `/v1/certificates/renew`, `/v1/certificates/revoke`, `/v1/crl`) on a port
   separate from the Raft-internal RPC port used for consensus traffic among
   cluster members — mirroring the `rpc_port`/`http_port` separation already
   used by `cmd/chaos_node`. Each node's configured peer list SHALL carry
   every peer's client-facing HTTP address alongside its Raft RPC address (the
   two are not derivable from each other), so a node redirecting per
   Requirement 17.7 can build the leader's full request URL.
7. WHEN a client-facing request arrives at a node that is not the current
   Raft leader AND that node has a known leader, it SHALL respond `308
   Permanent Redirect` with a `Location` header pointing at the leader's
   client-facing HTTP address for the same path — `308` (unlike `301`/`302`)
   preserves the request method and body, so a redirected `POST
   /v1/certificates` still arrives at the leader as a `POST` with its CSR
   body intact, letting an HTTP client that auto-follows redirects complete
   the request transparently. WHEN that node has no known leader (e.g. an
   election is in progress), it SHALL respond `503 Service Unavailable`
   instead, since there is nowhere to redirect to — mirroring the existing
   `reason: not_leader` pattern already present in `raft.hpp` for that case
   only.
8. WHEN the leader processes `POST /v1/certificates` (or `/renew`, or a
   revocation), it SHALL perform the actual cryptographic operation (sign the
   CSR; mark revoked) locally, using an in-memory CA signer reconstructed once
   from the decrypted `bootstrap_ca` material, and SHALL then call
   `submit_command()` with a `record_issuance`/`record_revocation` command
   carrying the already-computed result. The HTTP response SHALL NOT be sent
   until `submit_command()`'s returned future resolves — i.e. until the fact
   has been replicated to a majority and applied — so a client never observes
   an issuance that a subsequent leader failover could "forget."
9. `certificate_authority` SHALL gain a factory,
   `certificate_authority::from_existing(ca_cert_pem, ca_key_pem) ->
   certificate_authority`, constructing an instance around already-existing CA
   material instead of always generating a fresh root. `ca_cluster_node` SHALL
   use this factory to reconstruct the signer from decrypted, replicated
   bootstrap material — on the initial leader, and again on every subsequent
   leader after a failover.
10. A `--bootstrap-ca` flag on `ca_cluster_node` SHALL, on a cluster with no
    existing root material, generate a fresh `certificate_authority` locally
    and submit the resulting `bootstrap_ca` command exactly once. Nodes
    started without this flag SHALL wait to receive the root material via
    normal log replication or snapshot installation, and SHALL refuse
    client-facing requests until it arrives.
11. The default `ca_cluster_node` cluster size SHALL be 3 nodes — the smallest
    odd cluster size that tolerates one node failure while keeping quorum
    overhead minimal. Documentation, example configuration, and the test
    coverage in Requirement 17's task plan SHALL target a 3-node cluster;
    operators needing to tolerate more than one simultaneous failure MAY
    configure a larger odd-sized cluster, but 3 is the shipped default.
12. WHEN a `ca_cluster_node` cluster is deployed on AWS, its 3 nodes SHALL be
    placed in 3 different Availability Zones — one node per AZ — so that a
    single AZ outage cannot take down a quorum (2 of 3 nodes). This SHALL be
    achievable two ways, both documented: (a) manually, by deploying each
    node into a distinct subnet (one per AZ) with its `peers` configuration
    listing all three nodes' RPC and HTTP addresses per Requirement 17.6; or
    (b) via the project's existing `aws_ec2_quorum_manager` (already
    implemented, unrelated to this spec), configured with one placement group
    per AZ (`target_count = 1` each, `subnet_by_group` set to a distinct
    per-AZ subnet), for operators who want Kythira to automatically replace a
    failed node's EC2 instance in its own AZ. Option (b) reuses existing
    code; this spec SHALL NOT introduce a second, CA-specific EC2
    provisioning mechanism.

### Requirement 18: ACME certificate provider and test ACME server

**User Story:** As an operator, I want Kythira nodes able to obtain
certificates from a real-world ACME CA (Let's Encrypt, or any RFC
8555-compliant CA) using the same `certificate_provider` abstraction as the
local and AWS-backed providers; and as a test author, I want a self-contained
mock ACME server so I can exercise that path without depending on a real,
rate-limited, publicly-routable CA.

#### Acceptance Criteria

1. `acme_certificate_provider` SHALL be provided in
   `include/raft/acme_certificate_provider.hpp` / `_impl.hpp`, satisfying
   `certificate_provider<acme_certificate_provider>` (verified by
   `static_assert`), implementing RFC 8555's client-side flow: directory
   discovery, account creation (or reuse of a previously created account
   key), order creation, authorization/challenge retrieval, challenge
   completion, order finalization with a CSR, and certificate download.
2. `acme_certificate_provider_config` SHALL carry: `directory_url`; an
   account key (generated fresh if none is supplied — ECDSA P-256 by default,
   matching `certificate_authority`'s own default); `contact` (e.g.
   `mailto:` URIs, optional); the preferred `challenge_type` (`http_01` or
   `dns_01`); and challenge-type-specific settings — an HTTP responder
   bind address for `http_01`, or DNS server/zone/TSIG settings (the same
   shape already used by `rfc2136_ldns_discovery`'s config) for `dns_01`.
3. WHEN `challenge_type` is `http_01`, `sign_csr()` SHALL, for each
   identifier needing validation, serve the required key-authorization
   response at `/.well-known/acme-challenge/<token>` (via a short-lived
   `httplib::Server`) for the duration of validation, and SHALL stop serving
   it once the corresponding authorization reaches a terminal state.
4. WHEN `challenge_type` is `dns_01`, `sign_csr()` SHALL publish a `TXT`
   record at `_acme-challenge.<identifier>.` carrying the required
   key-authorization digest via RFC 2136 DNS UPDATE, reusing the
   update/sign/send helpers already implemented for `rfc2136_ldns_discovery`
   rather than a second implementation of them, and SHALL remove that record
   (best-effort) once the authorization reaches a terminal state.
5. `sign_csr()` SHALL poll authorization and order status with backoff,
   bounded by a configurable total timeout, and SHALL reject the returned
   future with a descriptive error on challenge/order failure, an unmet
   challenge, or a timeout — mirroring how `aws_acm_pca_provider::sign_csr()`
   already handles ACM Private CA's own asynchronous issuance.
6. `root_certificate_pem()` SHALL return the top-most certificate of the
   chain most recently returned by the ACME server's certificate-download
   endpoint. Because real-world ACME CAs distribute trust roots out-of-band
   (e.g. via OS/browser trust stores) rather than through the ACME API
   itself, this SHALL be documented as best-effort/informational when talking
   to a real CA, and SHALL only be treated as a full, authoritative trust
   anchor when talking to the test ACME server (Requirement 18.7), whose
   chain terminates at its own self-contained root.
7. A test ACME server, `acme_test_server`, SHALL be provided as an in-process
   fixture (`tests/acme_test_server.hpp`, mirroring the shape of
   `ca_test_fixture`) implementing the RFC 8555 endpoints needed to drive the
   full happy-path flow (`/directory`, new-nonce, new-account, new-order,
   authorization retrieval, challenge response, finalize, certificate
   download) plus standard failure modes (invalid JWS signature, expired
   order, failed challenge validation). Internally it SHALL issue
   certificates via its own `certificate_authority` instance (Requirement 1)
   — it SHALL NOT re-implement certificate signing, only ACME's wire protocol
   around the framework's existing signing engine.
8. `acme_test_server` SHALL support both real challenge validation (actually
   performing an HTTP GET to the challenged identifier's well-known URL, or
   an actual DNS query for the TXT record) for full end-to-end tests, and a
   fault-injectable bypass
   (`fiu_do_on("raft/acme/test_server/skip_challenge_validation", ...)`,
   following the project's existing fault-injection convention) for tests
   that only need to exercise the ACME state machine without standing up
   real HTTP/DNS infrastructure.
9. `acme_certificate_provider` and `acme_test_server` SHALL be exercised
   together in a property test verifying the same chain-of-trust property
   already established for the other providers: a certificate obtained via
   `acme_certificate_provider` against a running `acme_test_server`
   chain-verifies against that server's root.
10. JWS request signing (RFC 7515) and JWK encoding (RFC 7517) SHALL be
    implemented using the project's existing dependencies — OpenSSL EVP
    signing and `boost::json` (already used by `file_persistence.hpp`) — with
    a small base64url helper. No new external library dependency SHALL be
    introduced for ACME support.

### Requirement 19: Server-certificate fingerprint pinning for first-contact bootstrap trust

**User Story:** As an operator distributing a bearer token to a new instance so
it can request its first certificate from `ca_service --serve` /
`ca_cluster_node`, I want to distribute the server's certificate fingerprint
through that same out-of-band channel, so the instance can confirm it's
actually talking to the intended CA server before it has anything signed by
that CA to chain-verify against.

#### Acceptance Criteria

1. WHEN `ca_service --serve` or `ca_cluster_node` is started with TLS enabled
   (`--tls-cert`/`--tls-key`, or the `local` provider's self-issued default
   per Requirement 11.5), it SHALL compute and print, at startup, the SHA-256
   fingerprint of the root/issuing CA certificate backing its TLS listener —
   for the `local` provider this is `certificate_authority::root_certificate_pem()`;
   for an operator-supplied external certificate, the external issuer's root.
   This SHALL be the *root's* fingerprint, not the leaf/serving certificate's
   own — the leaf changes across hot-reload (Requirement 16) while the root
   does not, so pinning the root keeps a distributed fingerprint valid across
   rotations.
2. `ca_service` / `ca_cluster_node` SHALL additionally support a
   `--print-root-fingerprint` mode that computes and prints this fingerprint
   and exits immediately, without starting the HTTP listener, so an operator
   can capture it in a provisioning script without parsing startup logs.
3. A pinned-verification bootstrap helper SHALL be provided
   (`include/raft/ca_bootstrap_client.hpp`) that, given a base URL, a bearer
   token, and an expected root fingerprint — all delivered through the same
   out-of-band channel used for the token itself (e.g. the same
   `EnvironmentFile` / `user_data` / secrets-manager entry) — performs: connect
   over TLS, retrieve the server's presented certificate chain, compute the
   SHA-256 fingerprint of the chain's root certificate, and accept the
   connection only on an exact match — bypassing normal system-trust-store
   chain verification for this specific bootstrap connection, since there is
   no prior trust relationship to chain-verify against.
4. WHEN the pinned fingerprint does not match, the helper SHALL abort the
   connection and raise an error identifying the mismatch (expected vs.
   observed fingerprint). It SHALL NOT fall back to unpinned or otherwise
   weakened verification.
5. Upon a successful pinned-fingerprint connection, the helper SHALL fetch
   `GET /v1/root-ca` over that already-validated channel and return the
   resulting root certificate PEM to the caller. Callers SHALL use that
   fetched root PEM for normal chain verification on all subsequent
   connections — including the bootstrap request that actually obtains the
   first certificate, and every request thereafter. Fingerprint pinning is a
   one-time bootstrap technique, not an ongoing trust model, so it is never
   affected by the server's leaf certificate rotating under Requirement 16.
6. Pinned-verification (this requirement) and bearer-token authentication
   (Requirement 11.4) SHALL be independent and composable: pinning
   establishes that the client can trust the server's identity; the token
   establishes that the server can trust the client's request. Both SHALL be
   satisfied before a `POST /v1/certificates` bootstrap request is considered
   fully authenticated in either direction.
7. The cloud deployment documentation/examples from Requirement 12 SHALL be
   updated to show the root fingerprint distributed alongside the bearer
   token — e.g. a second `EnvironmentFile` entry, `CA_SERVICE_ROOT_FINGERPRINT`,
   next to `CA_SERVICE_AUTH_TOKEN`.

### Requirement 20: Certificate issuance across LAN/mDNS, IP-only, and DNS-integrated node configurations

**User Story:** As an operator deploying nodes into different network
environments — a LAN where mDNS is available, an environment where the node
has no discoverable name at all (only an IP), or an environment where the
node already controls a DNS record — I want certificate issuance, including
via ACME, to work correctly for whichever naming a node actually has, without
requiring a DNS name it doesn't control.

#### Acceptance Criteria

1. `certificate_authority::issue()`/`sign_csr()`, `ca_test_fixture::bootstrap_client()`,
   and `ca_service`/`ca_cluster_node`'s bearer-token-authenticated
   `/v1/certificates` route already impose no requirement that a SAN be a
   real, externally-resolvable DNS name — Requirement 2.5 only requires at
   least one of `dns_names`/`ip_addresses` to be non-empty, and none of these
   paths validate domain or name control at all (trust comes from the bearer
   token, the shared volume, or the in-process call, per Requirements 7/11).
   This requirement makes explicit that mDNS names (e.g. `node1.local`), bare
   IP addresses, and ordinary DNS names SHALL all be equally valid inputs to
   every one of these paths with no special-casing — they already satisfy
   Cases 1–3 without modification.
2. **LAN/mDNS (Case 1):** `acme_test_server`'s challenge validation
   (Requirement 18.7–18.8) SHALL resolve an identifier ending in `.local` the
   same way any other hostname is resolved on the validating host —
   `getaddrinfo()` — relying on the host's own mDNS-capable resolver
   configuration (e.g. `nss-mdns`/Avahi on Linux, native `mDNSResponder` on
   macOS) rather than any custom mDNS client code. WHEN the validating host
   has no mDNS-capable resolver configured, `.local` resolution SHALL fail
   with an error distinguishable from a normal DNS failure (Requirement
   20.6), not be silently retried against public DNS. mDNS is link-local:
   this SHALL only be expected to succeed when `acme_test_server` and the
   node being validated share a local network segment. Real public ACME CAs
   SHALL NOT be expected to support `.local` identifiers at all — this
   capability is specific to `acme_test_server` (or any private ACME CA an
   operator runs on the same LAN), and `acme_certificate_provider`'s
   documentation SHALL say so plainly.
3. **IP-only, no mDNS, no controlled DNS record (Case 2):**
   `acme_certificate_provider` SHALL support RFC 8738 IP-identifier
   issuance. WHEN an identifier being requested is an IP address rather than
   a DNS name, the ACME `newOrder` request's `identifier.type` SHALL be
   `"ip"` (not `"dns"`), and that identifier's challenge SHALL always be
   `http-01`, regardless of `acme_certificate_provider_config::challenge_type` —
   `dns-01` SHALL NOT be attempted for an IP identifier, since RFC 8738 does
   not define it for that identifier type. `challenge_type` therefore governs
   DNS-identifier validation only. `acme_test_server` SHALL accept and
   correctly validate `"ip"`-typed orders the same way a real RFC
   8738-supporting CA would. (`tls-alpn-01`, RFC 8738's other IP-compatible
   challenge type, is out of scope for this spec.)
4. **DNS integration (Case 3):** WHEN a node already uses one of the
   existing DNS-based `peer_discovery` implementations
   (`rfc2136_ldns_discovery`, `rfc6763_ldns_peer_discovery`) and therefore
   already controls a DNS zone and TSIG credential, that same configuration
   SHALL be directly usable as `acme_certificate_provider_config::dns01_update_config`
   (Requirement 18.2) without separate setup — both features already share
   the identical RFC 2136 UPDATE mechanism (Requirement 18.4), so a node's
   existing DNS-registration credentials for peer discovery are sufficient
   for certificate issuance too.
5. A single `sign_csr()` call whose `csr_signing_options` mixes DNS names and
   IP addresses (e.g. a node with both an mDNS name and a bare IP as SANs)
   SHALL validate each identifier using the challenge type appropriate to
   *that identifier's own type* (per Requirements 20.2–20.3), not one
   challenge type for the whole request — a single certificate's SANs MAY be
   validated by different challenge mechanisms.
6. mDNS resolution failure (Requirement 20.2) and RFC 2136 DNS UPDATE failure
   (Requirement 18.4/20.4) SHALL surface as distinguishable error conditions
   — an operator debugging a failed LAN deployment needs to tell "no
   mDNS resolver on this host" apart from "the DNS zone update was rejected,"
   since the fixes are unrelated.
