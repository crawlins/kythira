# Design Document

## Overview

This document describes the design of the Certificate Authority (CA) testing
framework: an in-process `certificate_authority` library, an RAII
`temp_cert_files` helper for handing PEM material to file-path-based transport
configs, and a standalone `ca_service` executable that provisions matching
certificate material across multiple containers in Docker/Podman scenarios. All
cryptographic work is done with OpenSSL's EVP/X509 APIs (OpenSSL is already an
optional project dependency, detected via `find_package(OpenSSL QUIET)` and
consumed elsewhere via `OpenSSL::SSL` / `OpenSSL::Crypto`).

The framework exists to replace hand-authored placeholder PEM strings currently
embedded in `tests/http_ssl_*` and `tests/coap_*` test files — those strings are
shaped like PEM but decode to filler bytes, not a valid key pair, so they cannot
complete a real handshake. Real, generated material lets certificate-consuming
tests exercise actual TLS/mTLS/DTLS handshakes, including precise negative
scenarios (expired, not-yet-valid, wrong host, untrusted issuer, revoked).

## Architecture

```
certificate_authority                         (include/raft/certificate_authority.hpp)
  │
  ├── ca_options ──────────────► ctor generates a self-signed root CA
  │                               (EVP_PKEY keypair + X509 cert, CA:TRUE)
  │
  ├── issue(leaf_certificate_options) ─► pem_material
  │     - generates a fresh EVP_PKEY for the leaf
  │     - builds an X509 signed by the CA's key, with SAN/EKU/keyUsage set
  │       from leaf_certificate_options
  │
  ├── issue_expired(...) / issue_not_yet_valid(...)
  │     - same as issue(), with notBefore/notAfter shifted into the past
  │       or future before signing
  │
  ├── revoke(pem_material) / crl_pem()
  │     - tracks revoked serials in an internal X509_CRL, re-signed on read
  │
  └── root_certificate_pem()
        - returns the root cert only; the CA private key never leaves the class

temp_cert_files                               (include/raft/certificate_authority.hpp)
  - RAII: pem_material -> files under a unique tmp dir (key file mode 0600)
  - used to populate:
      cpp_httplib_server_config::ssl_cert_path / ssl_key_path / ca_cert_path
      cpp_httplib_client_config::client_cert_path / client_key_path / ca_cert_path
      coap_server_config / coap_client_config :: cert_file / key_file / ca_file

ca_service                                    (cmd/ca_service/main.cpp)
  - CLI over certificate_authority + temp_cert_files-like fixed output dir
  - issues one root CA + one leaf cert per --service argument
  - runs once, exits 0, writes to a directory (typically a shared compose volume)
  - consumed by docker/ca-provisioning-compose.yml as an init container
```

`certificate_authority` never touches the filesystem or network; `temp_cert_files`
is the only piece that writes files, and it owns their lifetime. `ca_service` is a
thin CLI wrapper: it does not introduce new certificate-generation logic, only
argument parsing, `getaddrinfo` resolution, and directory layout.

## Components and Interfaces

### 1. `include/raft/certificate_authority.hpp`

Public types and the `certificate_authority` / `temp_cert_files` class
declarations. No OpenSSL types appear in this header's public interface — all
OpenSSL handles are private implementation detail (`impl` pointer or private
members forward-declared against OpenSSL opaque types), keeping consumers free
of a hard `#include <openssl/...>` dependency merely to hold a
`certificate_authority`.

```cpp
namespace raft::testing {

enum class key_algorithm { rsa_2048, rsa_4096, ecdsa_p256, ecdsa_p384 };

struct distinguished_name {
    std::string common_name;
    std::string organization{"Kythira Test CA"};
    std::string country{"US"};
};

struct ca_options {
    distinguished_name subject{.common_name = "Kythira Test Root CA"};
    key_algorithm       algorithm{key_algorithm::ecdsa_p256};
    std::chrono::seconds validity{std::chrono::hours(24 * 365)};
};

struct leaf_certificate_options {
    distinguished_name              subject;
    key_algorithm                   algorithm{key_algorithm::ecdsa_p256};
    std::vector<std::string>        dns_names;
    std::vector<std::string>        ip_addresses;
    bool                             server_auth{true};
    bool                             client_auth{false};
    std::chrono::system_clock::time_point not_before{std::chrono::system_clock::now()};
    std::chrono::seconds            validity{std::chrono::hours(24 * 30)};
};

struct pem_material {
    std::string certificate_pem;
    std::string private_key_pem;
    std::string chain_pem;   // leaf + root, concatenated; empty for the root itself
    std::uint64_t serial{0}; // exposed so revoke() can match without reparsing PEM
};

class certificate_authority {
public:
    explicit certificate_authority(ca_options options = {});
    ~certificate_authority();

    certificate_authority(const certificate_authority&) = delete;
    certificate_authority& operator=(const certificate_authority&) = delete;

    [[nodiscard]] auto root_certificate_pem() const -> const std::string&;

    [[nodiscard]] auto issue(leaf_certificate_options options) -> pem_material;
    [[nodiscard]] auto issue_expired(leaf_certificate_options options) -> pem_material;
    [[nodiscard]] auto issue_not_yet_valid(leaf_certificate_options options) -> pem_material;

    auto revoke(const pem_material& cert) -> void;
    [[nodiscard]] auto crl_pem() const -> std::string;

private:
    struct impl;
    std::unique_ptr<impl> _impl;
};

class temp_cert_files {
public:
    explicit temp_cert_files(const pem_material& material);
    ~temp_cert_files();

    temp_cert_files(const temp_cert_files&) = delete;
    temp_cert_files& operator=(const temp_cert_files&) = delete;

    [[nodiscard]] auto cert_path() const -> const std::string&;
    [[nodiscard]] auto key_path() const -> const std::string&;
    [[nodiscard]] auto chain_path() const -> const std::string&;   // "" if no chain

private:
    std::filesystem::path _dir;
    std::string _cert_path;
    std::string _key_path;
    std::string _chain_path;
};

}  // namespace raft::testing
```

`certificate_authority` uses the pimpl pattern (`struct impl`) so
`certificate_authority.hpp` itself needs no OpenSSL includes; only
`certificate_authority_impl.hpp` / the corresponding `.cpp` do. This mirrors the
existing header/impl split used by `http_transport.hpp` /
`http_transport_impl.hpp`.

### 2. `include/raft/certificate_authority_impl.hpp` + `src/certificate_authority.cpp`

Holds `certificate_authority::impl` and all OpenSSL calls, guarded by
`#ifdef KYTHIRA_HAS_OPENSSL` at the translation-unit level (the target itself is
only compiled when `OpenSSL::SSL` exists, so this is primarily documentation of
the dependency at the top of the file).

Key generation dispatches on `key_algorithm`:

```cpp
auto generate_key(key_algorithm algorithm) -> EvpKeyPtr {  // EvpKeyPtr = unique_ptr<EVP_PKEY, EVP_PKEY_free>
    switch (algorithm) {
        case key_algorithm::rsa_2048: return generate_rsa_key(2048);
        case key_algorithm::rsa_4096: return generate_rsa_key(4096);
        case key_algorithm::ecdsa_p256: return generate_ec_key(NID_X9_62_prime256v1);
        case key_algorithm::ecdsa_p384: return generate_ec_key(NID_secp384r1);
    }
}
```

Both `generate_rsa_key` and `generate_ec_key` use `EVP_PKEY_CTX` (`EVP_PKEY_CTX_new_id`,
`EVP_PKEY_keygen_init`, `EVP_PKEY_keygen`) — the OpenSSL 3.x-preferred API, not the
deprecated `RSA_generate_key`/`EC_KEY_new` low-level APIs.

Root CA construction (`certificate_authority::impl::impl(ca_options)`):
1. `_ca_key = generate_key(options.algorithm)`.
2. Build an `X509` with subject = issuer (self-signed), serial from
   `next_serial()`, validity from `options.validity`.
3. Set extensions via `X509V3_EXT_conf_nid`: `basicConstraints = critical,CA:TRUE`,
   `keyUsage = critical,keyCertSign,cRLSign`, `subjectKeyIdentifier = hash`.
4. Sign with `X509_sign(cert, _ca_key, EVP_sha256())`.
5. Serialize to PEM immediately into `_root_pem` (`PEM_write_bio_X509`) — the
   in-memory `X509*` is retained for signing leaves, but the class never needs to
   re-serialize it.

Leaf issuance (`issue()` / `issue_expired()` / `issue_not_yet_valid()` all funnel
through one private `issue_with_window(options, not_before, not_after)`):
1. Validate `options.dns_names` and `options.ip_addresses` are not both empty
   (throw `std::invalid_argument` otherwise — Requirement 2.5).
2. `auto leaf_key = generate_key(options.algorithm)`.
3. Build an `X509` with subject = `options.subject`, issuer = the CA's subject,
   serial = `next_serial()`, `notBefore`/`notAfter` from the given window.
4. Set `basicConstraints = critical,CA:FALSE`.
5. Build the `subjectAltName` extension from `dns_names` (`DNS:` entries) and
   `ip_addresses` (`IP:` entries) via `X509V3_EXT_conf_nid(NID_subject_alt_name, ...)`.
6. Build `keyUsage` / `extendedKeyUsage` per `server_auth` / `client_auth`
   (Requirement 2.3–2.4).
7. Sign with the CA's private key (`X509_sign(leaf, _ca_key, EVP_sha256())`) —
   this is what makes the leaf chain-verifiable against `root_certificate_pem()`.
8. Serialize leaf + key to PEM (`PEM_write_bio_X509`, `PEM_write_bio_PrivateKey`);
   build `chain_pem` by concatenating leaf PEM and `_root_pem`.
9. Return `pem_material{cert_pem, key_pem, chain_pem, serial}`.

`issue_expired()` passes `not_before = now - 2 days`, `not_after = now - 1 day`.
`issue_not_yet_valid()` passes `not_before = now + 1 day`,
`not_after = not_before + options.validity`.

Revocation / CRL (`revoke()` / `crl_pem()`):
- `_revoked` is a `std::vector<std::pair<std::uint64_t /*serial*/, time_t /*revoked_at*/>>`
  guarded by the same mutex as issuance.
- `revoke()` looks up `cert.serial` in previously issued serials (`_issued_serials`
  set, populated by every successful `issue*()` call); throws
  `std::invalid_argument` if not found (Requirement 4.3).
- `crl_pem()` builds a fresh `X509_CRL` from `_revoked` on every call
  (`X509_CRL_new`, `X509_CRL_set_issuer_name`, one `X509_REVOKED` per entry via
  `X509_CRL_add0_revoked`, `X509_CRL_set_lastUpdate`/`set_nextUpdate`,
  `X509_CRL_sign`), then serializes to PEM. CRLs are cheap to regenerate given the
  expected test-scale revocation counts (single digits), so no incremental-update
  optimization is needed.

Serial numbers (Requirement 1.4): `next_serial()` returns
`(instance_seed << 32) | ++_serial_counter`, where `instance_seed` is computed
once in the constructor from
`std::chrono::high_resolution_clock::now().time_since_epoch().count()` XORed with
`reinterpret_cast<std::uintptr_t>(this)`, truncated to 32 bits. This keeps
serials unique across instances without any shared/global state, which matters
because `ctest -j$(nproc)` runs multiple test binaries — and multiple
`certificate_authority` instances within one binary — concurrently.

### 3. `temp_cert_files` implementation

```cpp
temp_cert_files::temp_cert_files(const pem_material& material) {
    auto unique = std::to_string(material.serial) + "_" +
        std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count());
    _dir = std::filesystem::temp_directory_path() / ("kythira_ca_" + unique);
    std::filesystem::create_directories(_dir);

    _cert_path = write_file(_dir / "cert.pem", material.certificate_pem);
    _key_path  = write_file(_dir / "key.pem", material.private_key_pem, /*mode=*/0600);
    if (!material.chain_pem.empty()) {
        _chain_path = write_file(_dir / "chain.pem", material.chain_pem);
    }
}

temp_cert_files::~temp_cert_files() {
    std::error_code ec;
    std::filesystem::remove_all(_dir, ec);  // best-effort; ec deliberately ignored
}
```

### 4. `cmd/ca_service/main.cpp`

Follows the existing `cmd/<name>/{CMakeLists.txt,main.cpp}` layout (see
`cmd/dns_discovery_node`). Argument parsing is hand-rolled (consistent with the
other `cmd/*` executables — none of them pull in a CLI-parsing library):

```cpp
struct service_spec {
    std::string name;
    std::vector<std::string> alt_names;
};

struct cli_options {
    std::vector<service_spec> services;
    std::string out_dir;
    std::string domain;              // optional suffix, e.g. "cluster.example.com"
    int validity_days{30};
    bool resolve_ips{false};
};
```

`main()`:
1. Parse argv into `cli_options`; print usage and return non-zero on malformed
   input (missing `--out-dir`, zero `--service` entries, unparsable
   `--validity-days`).
2. Construct one `raft::testing::certificate_authority` with default `ca_options`.
3. Write `root_ca.pem` to `<out-dir>/root_ca.pem`.
4. For each `service_spec`:
   a. Build `dns_names = [name + (domain.empty() ? "" : "." + domain)] + alt_names`.
   b. WHEN `resolve_ips`, call `getaddrinfo()` on each DNS name and collect
      resulting addresses into `ip_addresses` (skip names that fail to resolve —
      at provisioning time the service's own container may not have started yet
      for peer names, only self-resolution is expected to succeed reliably; this
      matches the project's existing pattern of resolving compose service names
      to IPs only where the consuming binary needs a literal IP).
   c. `auto material = ca.issue({.subject = {.common_name = name}, .dns_names = dns_names, .ip_addresses = ip_addresses, .server_auth = true, .client_auth = true, .validity = std::chrono::hours(24 * validity_days)});`
   d. Write `<out-dir>/<name>/cert.pem`, `key.pem` (mode 0600), `chain.pem`.
5. Return 0. Any exception thrown by `certificate_authority` or filesystem calls
   is caught in `main()`, printed to `stderr`, and converted to a non-zero exit
   code — no exception ever escapes `main()`.

### 5. `docker/ca_service/Dockerfile`

Two-stage build mirroring `docker/dns_discovery_node/Dockerfile`:

```dockerfile
FROM ubuntu:24.04 AS builder
RUN apt-get update && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y g++ cmake ninja-build pkg-config libssl-dev && \
    rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
RUN cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_PREFIX_PATH=/src/vcpkg_installed/x64-linux && \
    cmake --build build --target ca_service

FROM ubuntu:24.04
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y libssl3 && \
    rm -rf /var/lib/apt/lists/*
COPY --from=builder /src/build/ca_service /usr/local/bin/ca_service
ENTRYPOINT ["/usr/local/bin/ca_service"]
```

No `HEALTHCHECK` is defined — unlike the long-lived node containers,
`ca_service` is an init container; compose gates on process exit status
(`service_completed_successfully`), not a health probe.

### 6. `docker/ca-provisioning-compose.yml`

Demonstrates the intended usage pattern for future mTLS/DTLS docker-chaos
scenarios:

```yaml
networks:
  ca-provisioning-net:
    driver: bridge

volumes:
  ca-material:

services:
  ca-service:
    image: kythira-ca-service:dev
    networks:
      - ca-provisioning-net
    volumes:
      - ca-material:/ca
    command:
      - "--out-dir=/ca"
      - "--service=mtls-node1"
      - "--service=mtls-node2"
      - "--domain=cluster.example.local"

  mtls-node1:
    image: kythira-mtls-node:dev
    hostname: mtls-node1
    networks:
      ca-provisioning-net:
    volumes:
      - ca-material:/ca:ro
    depends_on:
      ca-service:
        condition: service_completed_successfully

  mtls-node2:
    image: kythira-mtls-node:dev
    hostname: mtls-node2
    networks:
      ca-provisioning-net:
    volumes:
      - ca-material:/ca:ro
    depends_on:
      ca-service:
        condition: service_completed_successfully
```

No static IPs appear anywhere in this file; nodes address each other by compose
service name (`mtls-node1`, `mtls-node2`), consistent with the project's
container runtime compatibility rules for both Docker and rootless Podman. The
`mtls-node1` / `mtls-node2` images are illustrative stubs for this spec — wiring
up an actual mTLS chaos test node is future scope, tracked separately from this
framework.

## Architecture — cloud and network-servable extensions

```
certificate_provider (concept, include/raft/certificate_provider.hpp)
  │
  ├── local_certificate_provider     wraps certificate_authority& (sync -> immediately-resolved Future)
  │
  └── aws_acm_pca_provider           wraps Aws::ACMPCA::ACMPCAClient
        root_certificate_pem() → GetCertificateAuthorityCertificate (cached)
        sign_csr()             → IssueCertificate, then poll GetCertificate (bounded by api_timeout)

certificate_authority (extended)
  └── sign_csr(csr_pem, csr_signing_options) -> pem_material   (private_key_pem left empty)

generate_key_and_csr(leaf_certificate_options) -> csr_material   (free function; no CA involved)

ca_service (extended)
  ├── oneshot mode   --out-dir + --service*   (Requirement 7 — local filesystem/compose-volume delivery)
  └── serve mode     --serve <host:port> --provider {local|aws-acm-pca}
        httplib::Server, Bearer-token auth required on every route
          GET  /healthz
          GET  /v1/root-ca
          POST /v1/certificates          (CSR in → cert+chain PEM out; no private key crosses the wire)
          POST /v1/certificates/revoke   (local provider only; 501 otherwise)
          GET  /v1/crl                   (local provider only; 501 otherwise)
        Deployable as:
          - long-running docker-compose service   (docker/ca-service-server-compose.yml)
          - systemd unit on an EC2 instance        (docker/ca_service/ca_service.service)
          - ECS task                                (docker/ca_service/ecs-task-definition.json)

ca_test_fixture                               (tests/ca_test_fixture.hpp)
  - the ergonomic front door test authors actually call:
      construct(options)   → sets up a CA (in-process, or a real ca_service --serve child process)
      bootstrap_client(id, dns_names, ...) → temp_cert_files&, ready for a transport config
  - hides whether the CA is in-process or a running network service behind one interface
```

The key design move is putting the CSR at the boundary between "local" and
"cloud" issuance: `sign_csr()` on `certificate_authority`, `IssueCertificate` on
ACM Private CA, and the `/v1/certificates` HTTP route all take a CSR in and hand
back a certificate — never a private key. `local_certificate_provider` and
`aws_acm_pca_provider` are the only two places that know which backend is in
use; everything above the `certificate_provider` concept (including the HTTP
route handlers in `ca_service`) is backend-agnostic, the same way `find_peers()`
callers in the node-bootstrap flow don't know which `peer_discovery`
implementation they're talking to.

## Components and Interfaces (continued)

### 7. `include/raft/certificate_provider.hpp`

```cpp
namespace raft::testing {

struct csr_signing_options {
    std::vector<std::string> dns_names;
    std::vector<std::string> ip_addresses;
    bool server_auth{true};
    bool client_auth{false};
    std::chrono::seconds validity{std::chrono::hours(24 * 30)};
};

struct csr_material {
    std::string private_key_pem;
    std::string csr_pem;
};

[[nodiscard]] auto generate_key_and_csr(leaf_certificate_options options) -> csr_material;

template<typename P>
concept certificate_provider = requires(P& p, std::string csr_pem, csr_signing_options options) {
    { p.root_certificate_pem() } -> std::same_as<kythira::Future<std::string>>;
    { p.sign_csr(csr_pem, options) } -> std::same_as<kythira::Future<pem_material>>;
};

}  // namespace raft::testing
```

`generate_key_and_csr()` builds the key via the same `generate_key(key_algorithm)`
helper used internally by `certificate_authority`, then an `X509_REQ` carrying
`options.subject` and a `subjectAltName` extension via
`X509_REQ_add_extensions`, self-signed with the CSR's own key
(`X509_REQ_sign`) as CSRs require. It has no dependency on any
`certificate_authority` instance — a caller can generate a CSR and hand it to
either a local or cloud provider.

### 8. `certificate_authority::sign_csr()`

Added to `certificate_authority::impl` alongside `issue_with_window()`:

```cpp
auto certificate_authority::sign_csr(std::string csr_pem, csr_signing_options options) -> pem_material {
    auto req = load_csr(csr_pem);                    // PEM_read_bio_X509_REQ
    verify_csr_signature(req.get());                  // X509_REQ_verify — reject a forged CSR
    auto pubkey = X509_REQ_get_pubkey(req.get());     // caller's public key; CA never sees the private key

    auto cert = build_leaf_cert(X509_REQ_get_subject_name(req.get()), pubkey,
                                 options.dns_names, options.ip_addresses,
                                 options.server_auth, options.client_auth,
                                 now(), now() + options.validity);
    X509_sign(cert.get(), _ca_key.get(), EVP_sha256());

    return pem_material{serialize_cert(cert.get()), /*private_key_pem=*/"",
                         serialize_cert(cert.get()) + _root_pem, extract_serial(cert.get())};
}
```

`build_leaf_cert()` is the same helper `issue_with_window()` already uses
internally to set `basicConstraints`/`keyUsage`/`extendedKeyUsage`/SAN — the two
entry points differ only in where the key pair (and thus the public key placed
in the certificate) comes from: freshly generated inside `issue()`, or read out
of the caller-supplied CSR here.

### 9. `local_certificate_provider`

```cpp
class local_certificate_provider {
public:
    explicit local_certificate_provider(certificate_authority& ca) : _ca(ca) {}

    auto root_certificate_pem() -> kythira::Future<std::string> {
        return kythira::make_ready_future(std::string(_ca.root_certificate_pem()));
    }

    auto sign_csr(std::string csr_pem, csr_signing_options options) -> kythira::Future<pem_material> {
        return kythira::make_ready_future(_ca.sign_csr(std::move(csr_pem), std::move(options)));
    }

private:
    certificate_authority& _ca;   // non-owning; caller manages lifetime
};

static_assert(certificate_provider<local_certificate_provider>);
```

### 10. `include/raft/aws_acm_pca_provider.hpp` / `_impl.hpp`

Follows the same structure as `aws_ec2_quorum_manager`: a public header with the
config struct and class declaration, an `_impl.hpp` with the AWS SDK calls,
guarded by `#ifdef KYTHIRA_HAS_AWS_ACM_PCA`.

```cpp
struct aws_acm_pca_provider_config {
    aws_client_config aws;
    std::string certificate_authority_arn;
    std::string template_arn{"arn:aws:acm-pca:::template/EndEntityCertificate/V1"};
    std::string signing_algorithm{"SHA256WITHRSA"};
    std::chrono::seconds validity{std::chrono::hours(24 * 30)};
};

class aws_acm_pca_provider {
public:
    explicit aws_acm_pca_provider(aws_acm_pca_provider_config config);

    auto root_certificate_pem() -> kythira::Future<std::string>;
    auto sign_csr(std::string csr_pem, csr_signing_options options) -> kythira::Future<pem_material>;
    auto revoke(const std::string& serial) -> kythira::Future<void>;   // Requirement 10.7

private:
    Aws::ACMPCA::ACMPCAClient _client;
    aws_acm_pca_provider_config _config;
    std::optional<std::string> _cached_root_pem;
    std::mutex _mutex;
};

static_assert(certificate_provider<aws_acm_pca_provider>);
```

`sign_csr()` sequence:
1. `fiu_do_on("raft/aws/acm_pca/issue_certificate", ...)` fault point, then
   `Aws::ACMPCA::Model::IssueCertificateRequest` with `Csr` (raw CSR bytes),
   `CertificateAuthorityArn`, `TemplateArn`, `SigningAlgorithm`, and
   `Validity` (converted from `options.validity`); on failure, reject the
   future with the AWS error message.
2. Poll `GetCertificateRequest` for the returned `CertificateArn`, with
   exponential backoff, until it succeeds or `_config.aws.api_timeout` elapses
   — ACM Private CA issuance is asynchronous and typically completes in low
   single-digit seconds, but the API contract does not guarantee a bound.
   `fiu_do_on("raft/aws/acm_pca/get_certificate", ...)` wraps each poll.
3. On success, return
   `pem_material{response.GetCertificate(), "", response.GetCertificate() + response.GetCertificateChain(), 0}`
   (`serial` is left `0` — ACM Private CA identifies certificates by ARN, not
   the local monotonic-counter scheme used by `certificate_authority`; `revoke()`
   here takes the ARN string directly rather than a `pem_material`, unlike the
   local `certificate_authority::revoke()`).

`root_certificate_pem()` calls
`GetCertificateAuthorityCertificateRequest{CertificateAuthorityArn}`
(`fiu_do_on("raft/aws/acm_pca/get_certificate_authority_certificate", ...)`),
caches the PEM in `_cached_root_pem` under `_mutex`, and returns it on
subsequent calls without another API call.

### 11. `tests/ca_test_fixture.hpp` — CA setup and client bootstrap primitives

The two primitives above (`certificate_authority`/`temp_cert_files` for
in-process use, `ca_service --serve` for network use) are correct but
low-level: a test author wiring up mTLS by hand needs to construct a CA,
call `issue()` or generate-a-CSR-and-POST-it, and materialize the result to
files, in the right order, every time. `ca_test_fixture` collapses that into
two calls — "set up a CA" (constructor) and "get a client bootstrapped"
(`bootstrap_client()`) — following the same shape as `LocalNetworkFixture`
(construct once, then call a method per simulated participant).

```cpp
namespace raft::testing {

struct ca_test_fixture_options {
    ca_options ca{};
    bool start_network_service{false};
    std::string serve_bind_address{"127.0.0.1:0"};   // :0 = OS-assigned ephemeral port
    std::chrono::seconds startup_timeout{10};
};

class ca_test_fixture {
public:
    explicit ca_test_fixture(ca_test_fixture_options options = {});
    ~ca_test_fixture();   // terminates the child process, if one was started

    ca_test_fixture(const ca_test_fixture&) = delete;
    ca_test_fixture& operator=(const ca_test_fixture&) = delete;

    [[nodiscard]] auto root_certificate_pem() const -> const std::string&;

    [[nodiscard]] auto bootstrap_client(std::string client_id,
                                         std::vector<std::string> dns_names,
                                         std::vector<std::string> ip_addresses = {},
                                         bool server_auth = true,
                                         bool client_auth = true,
                                         std::chrono::seconds validity = std::chrono::hours(24 * 30))
        -> const temp_cert_files&;

private:
    certificate_authority _ca;
    std::optional<std::string> _service_root_pem;      // cached when start_network_service
    std::optional<int> _service_pid;                    // child process, when started
    std::string _service_base_url;                      // e.g. "http://127.0.0.1:41234"
    std::vector<std::unique_ptr<temp_cert_files>> _issued;   // fixture owns every result it hands out
};

}  // namespace raft::testing
```

Constructor (`start_network_service = false`, the default): construct `_ca`
only; no process, no network — the common case for single-process unit and
property tests.

Constructor (`start_network_service = true`): `fork()`/`posix_spawn()` (or
`std::system`-free process launch consistent with how other test harnesses in
this repo shell out, e.g. `docker_chaos::os::real_exec`) invoking
`ca_service --serve <serve_bind_address> --provider local --auth-token
<generated-per-fixture-token>`; poll `GET /healthz` on the resulting base URL
with backoff until `200` or `startup_timeout` elapses (throw
`std::runtime_error` on timeout, killing the child first); fetch and cache
`GET /v1/root-ca` into `_service_root_pem`.

`bootstrap_client(...)`:
- In-process mode: call `_ca.issue({...})` directly, wrap in
  `std::make_unique<temp_cert_files>(material)`, push to `_issued`, return
  `*_issued.back()`.
- Service mode: call `generate_key_and_csr({...})` locally to get
  `{private_key_pem, csr_pem}` (the private key is generated here and never
  transmitted); `POST` `csr_pem` plus the SAN/EKU/validity metadata as JSON to
  `<service_base_url>/v1/certificates` with the fixture's bearer token; on
  `200`, build `pem_material{response.certificate_pem, private_key_pem,
  response.chain_pem}` and materialize it the same way as the in-process path.

`root_certificate_pem()` returns `_ca.root_certificate_pem()` in in-process
mode or `*_service_root_pem` in service mode — callers don't need to branch on
which mode is active.

Because `_issued` holds `unique_ptr<temp_cert_files>` (not `temp_cert_files`
directly), returning `const temp_cert_files&` from `bootstrap_client()` stays
valid even though `std::vector` may reallocate and move its elements as more
clients are bootstrapped — only the pointee, never the pointer target, would
need to move, and `unique_ptr` guarantees it doesn't.

### 12. `cmd/ca_service/main.cpp` — serve mode

Extends the `cli_options` from Requirement 7 with:

```cpp
struct serve_options {
    std::string bind_address_and_port;
    std::string provider{"local"};              // "local" | "aws-acm-pca"
    std::string acm_pca_arn;
    std::string aws_region;
    std::string aws_endpoint_override;
    std::string auth_token;                     // required; from --auth-token or $CA_SERVICE_AUTH_TOKEN
    std::string tls_cert_path;                  // optional
    std::string tls_key_path;                   // optional
};
```

`main()` dispatches on whether `--serve` was given: oneshot path unchanged from
Requirement 7; serve path constructs either a `certificate_authority` +
`local_certificate_provider`, or an `aws_acm_pca_provider`, wires up an
`httplib::Server` (or `httplib::SSLServer` when `--tls-cert`/`--tls-key` are
given), and registers routes:

```cpp
server.set_pre_routing_handler([&](const httplib::Request& req, httplib::Response& res) {
    auto auth = req.get_header_value("Authorization");
    if (auth != "Bearer " + auth_token) {
        res.status = 401;
        return httplib::Server::HandlerResponse::Handled;
    }
    return httplib::Server::HandlerResponse::Unhandled;
});

server.Get("/healthz", [](const auto&, auto& res) { res.status = 200; });

server.Get("/v1/root-ca", [&](const auto&, auto& res) {
    // .get() on the provider's future — request handling here is synchronous
    // from the HTTP server's point of view, matching cpp-httplib's threading model
    res.set_content(provider_root_certificate_pem_sync(), "application/x-pem-file");
});

server.Post("/v1/certificates", [&](const httplib::Request& req, auto& res) {
    auto [csr_pem, options] = parse_certificate_request(req.body);  // JSON -> csr_signing_options
    auto material = provider_sign_csr_sync(csr_pem, options);
    res.set_content(to_json(material), "application/json");         // certificate_pem + chain_pem only
});
```

`provider_*_sync()` helpers call `.get()`/`.wait()` on the `kythira::Future`
returned by whichever `certificate_provider` was selected — cpp-httplib handles
each request on its own worker thread already, so blocking briefly on an AWS
API call inside one handler does not stall the others.

`SIGINT`/`SIGTERM` are handled by installing a signal handler that calls
`server.stop()`; `main()` calls `server.listen(...)` on the main thread and
returns once `listen()` unblocks.

## Architecture — TLS termination fix, renewal, and hot-reload

Building hot-reload surfaced a real gap: `cpp_httplib_server` never actually
terminates TLS today. `configure_ssl_server()` builds a scratch `SSL_CTX*`
purely to validate that the configured cert/key/CA files load and match, then
unconditionally throws `ssl_configuration_error("...not fully
implemented...")` — the running listener is a plain `httplib::Server`
regardless of `enable_ssl`. `coap_transport`, by contrast, already wires DTLS
for real via a live `coap_context_t*` configured with `coap_context_set_pki()`.
Requirement 14 fixes the HTTP side first, because hot-reload has nothing to
reload into otherwise; Requirement 15 adds the renewal *source*; Requirement 16
adds the reload mechanism that consumes it, on both transports.

```
cpp_httplib_server (fixed)
  enable_ssl=true → httplib::SSLServer, not httplib::Server
  configure_ssl_server() now configures the SSLServer's real SSL_CTX*
    (obtained via ssl_context()), not a throwaway validation context

renewal + reload loop
  ca_test_fixture::renew(client_id)
      re-issue same identity/options → atomic rename over the existing
      cert.pem/key.pem/chain.pem at the SAME paths bootstrap_client() returned
                │
                ▼ (files change on disk; no signal sent to the transport)
  {cpp_httplib_server,cpp_httplib_client,coap_server,coap_client}
      .enable_auto_reload(poll_interval)
          background thread: stat(cert_path).mtime changed? → reload_tls_material()
              validate into a scratch SSL_CTX / coap_dtls_pki_t first
              on success: swap into the LIVE context (new handshakes only)
              on failure: log via metrics_type/logger; keep serving old material
      .reload_tls_material()   ← also callable directly, no polling required

ca_service --serve
  POST /v1/certificates/renew        (EST/EST-coaps-inspired)
      auth: caller's own current client cert over mTLS (not the bearer token)
      same subject/SAN as the presented cert; fresh validity window
      rejects (401) a cert that doesn't chain to this provider's own root
```

The renewal source (`ca_test_fixture::renew()`, or an operator's own rotation
process writing to the same path) and the reload consumer
(`reload_tls_material()` / `enable_auto_reload()`) are deliberately decoupled —
neither knows about the other. Anything that atomically replaces the files at
the configured paths triggers a pickup on the next poll (or immediately, via a
direct `reload_tls_material()` call), the same way file-mounted secret
rotation works in most production cert-management setups (e.g. a
Kubernetes-mounted `Secret` updated by cert-manager).

## Components and Interfaces (continued further)

### 13. `cpp_httplib_server` — real TLS termination

```cpp
// Member changes: _http_server becomes conditional on OpenSSL support.
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
std::unique_ptr<httplib::Server> _http_server;   // used when !enable_ssl
std::unique_ptr<httplib::SSLServer> _ssl_server;  // used when enable_ssl
#else
std::unique_ptr<httplib::Server> _http_server;
#endif
```

`configure_ssl_server()` is rewritten to build the *real* server:

```cpp
auto cpp_httplib_server<Types>::configure_ssl_server() -> void {
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
    if (!_config.enable_ssl) return;

    validate_certificate_key_pair(_config.ssl_cert_path, _config.ssl_key_path);
    if (_config.require_client_cert) {
        if (_config.ca_cert_path.empty()) {
            throw kythira::ssl_configuration_error(
                "Client certificate authentication requires CA certificate path");
        }
        validate_certificate_file(_config.ca_cert_path);
    }

    _ssl_server = std::make_unique<httplib::SSLServer>(
        _config.ssl_cert_path.c_str(), _config.ssl_key_path.c_str());
    if (!_ssl_server->is_valid()) {
        throw kythira::ssl_configuration_error(
            std::format("Failed to initialize SSL server with cert {} / key {}",
                        _config.ssl_cert_path, _config.ssl_key_path));
    }

    auto* ctx = _ssl_server->ssl_context();   // the LIVE context, not a scratch one
    configure_ssl_context(ctx, _config.cipher_suites, _config.min_tls_version,
                          _config.max_tls_version);

    if (_config.require_client_cert) {
        if (SSL_CTX_load_verify_locations(ctx, _config.ca_cert_path.c_str(), nullptr) != 1) {
            throw kythira::ssl_configuration_error(
                std::format("Failed to load CA certificate: {}", _config.ca_cert_path));
        }
        SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, nullptr);
    }
#else
    throw kythira::ssl_configuration_error("SSL support not available (OpenSSL not enabled)");
#endif
}
```

`setup_endpoints()`, `start()`, and `stop()` are updated to operate on
whichever of `_http_server`/`_ssl_server` is active (a small private
`active_server()` accessor returning `httplib::Server*` — `SSLServer` derives
from `Server`, so route registration code is unchanged either way).

Exact `httplib::SSLServer` constructor overloads and whether
`ssl_context()`/`is_valid()` are named precisely this way SHALL be confirmed
against the vendored cpp-httplib version during implementation; the shape
above is standard cpp-httplib usage as of the versions this project already
depends on for its unified `Client`.

### 14. `reload_tls_material()` for `http_transport`

```cpp
auto cpp_httplib_server<Types>::reload_tls_material() -> void {
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
    if (!_ssl_server) {
        throw std::logic_error("reload_tls_material() requires enable_ssl");
    }
    validate_certificate_key_pair(_config.ssl_cert_path, _config.ssl_key_path);  // fail before touching the live ctx

    auto* ctx = _ssl_server->ssl_context();
    if (SSL_CTX_use_certificate_chain_file(ctx, _config.ssl_cert_path.c_str()) != 1 ||
        SSL_CTX_use_PrivateKey_file(ctx, _config.ssl_key_path.c_str(), SSL_FILETYPE_PEM) != 1 ||
        SSL_CTX_check_private_key(ctx) != 1) {
        throw kythira::ssl_configuration_error("reload_tls_material: new cert/key rejected");
    }
    if (_config.require_client_cert &&
        SSL_CTX_load_verify_locations(ctx, _config.ca_cert_path.c_str(), nullptr) != 1) {
        throw kythira::ssl_configuration_error("reload_tls_material: new CA cert rejected");
    }
#endif
}
```

`validate_certificate_key_pair()` (an existing helper, run against a scratch
`SSL_CTX` it owns internally) is called *first*, so a malformed replacement
never touches the live context — satisfying Requirement 16.3's
validate-then-swap ordering. Because OpenSSL's `SSL_CTX_use_*` calls only
affect the *default* certificate used by handshakes that start after the call,
already-established `SSL*` connection objects (created from the old default
before the reload) are unaffected — this is the same mechanism reverse proxies
like nginx/HAProxy rely on for zero-downtime cert rotation.

`enable_auto_reload(poll_interval)` starts a `std::jthread` that loops
`std::filesystem::last_write_time(_config.ssl_cert_path)` on
`poll_interval`, calling `reload_tls_material()` (wrapped in try/catch,
logging failures via `_metrics`/the transport's logger, per
Requirement 16.7) whenever the mtime advances past the last successfully
reloaded value. `disable_auto_reload()` requests stop and joins the
`jthread`. The same pattern applies to `cpp_httplib_client` for its
`client_cert_path`/`client_key_path`.

### 15. `reload_tls_material()` for `coap_transport`

```cpp
auto coap_server<...>::reload_tls_material() -> void {
    coap_dtls_pki_t pki_config = build_pki_config(_config);   // same helper start() already uses
    // Validate by attempting to load into a throwaway context first is not
    // straightforward with libcoap's API surface; instead, build_pki_config()
    // performs its own file-readability/parseability checks (already present
    // in the existing start()-time validation path) before this call proceeds.
    if (!coap_context_set_pki(_coap_context, &pki_config)) {
        throw kythira::certificate_validation_error("reload_tls_material: PKI setup rejected");
    }
}
```

`build_pki_config()` is the existing helper already used at
[coap_transport_impl.hpp:676](include/raft/coap_transport_impl.hpp#L676) /
[coap_transport_impl.hpp:2238](include/raft/coap_transport_impl.hpp#L2238) to
construct the `coap_dtls_pki_t` passed to `coap_context_set_pki()` at startup —
reload reuses it unchanged with the (possibly updated) `_config.cert_file`
etc. Whether the vendored libcoap version safely re-applies PKI configuration
to a context with active DTLS sessions (rather than only being safe before any
session exists) SHALL be verified experimentally during implementation; if it
is not safe, the fallback is to open a fresh `coap_context_t*` with the new
PKI config and migrate the listening endpoint to it, draining the old
context's sessions naturally as they close — this fallback is noted here as a
risk, not designed in full, pending that verification.

`enable_auto_reload()`/`disable_auto_reload()` follow the same
`std::jthread` + mtime-polling shape as the HTTP side.

### 16. `ca_test_fixture::renew()`

```cpp
auto ca_test_fixture::renew(const std::string& client_id) -> const temp_cert_files& {
    auto it = _bootstrapped.find(client_id);   // client_id -> {options, index into _issued}
    if (it == _bootstrapped.end()) {
        throw std::invalid_argument("renew(): unknown client_id: " + client_id);
    }
    auto material = /* re-issue with it->second.options, in-process or via service mode */;
    _issued[it->second.index]->replace_atomically(material);   // new method on temp_cert_files
    return *_issued[it->second.index];
}
```

`temp_cert_files` gains a private `replace_atomically(const pem_material&)`
used only by `renew()`: write `cert.pem.tmp`/`key.pem.tmp`/`chain.pem.tmp` in
the same directory, then `std::filesystem::rename()` each over its
non-`.tmp` counterpart — `rename()` within the same filesystem is atomic per
POSIX, so a background `reload_tls_material()` poller never observes a
half-written file.

### 17. `ca_service` — `POST /v1/certificates/renew`

Unlike every other `ca_service --serve` route, this one authenticates via the
caller's presented TLS client certificate rather than the bearer token.
cpp-httplib doesn't offer native per-route mTLS enforcement (client-certificate
verification is a connection-level `SSL_CTX` setting) — the design sets the
server's verify mode to *request* a client certificate without *requiring* one
(`SSL_VERIFY_PEER` without `SSL_VERIFY_FAIL_IF_NO_PEER_CERT`), so the
bearer-token routes keep working for callers presenting no certificate, and
the renewal handler explicitly:

1. Retrieves the peer certificate for the current connection (the exact
   cpp-httplib accessor for this — likely via the underlying socket's `SSL*` —
   SHALL be confirmed against the vendored version during implementation).
2. Returns `401` if none was presented, or if it doesn't chain-verify against
   the active provider's `root_certificate_pem()` (Requirement 15.4).
3. Extracts the presented certificate's subject and SAN entries and issues a
   fresh certificate with identical subject/SAN and a new validity window,
   via the same `certificate_provider::sign_csr()` path `/v1/certificates`
   already uses (the caller still supplies a fresh CSR in the request body —
   renewal changes *validity dates*, not the identity or the requirement that
   the private key stays local).

## Architecture — CA state as a Kythira Raft cluster

`certificate_authority`'s state (root key/cert, issuance ledger, revocation
list) is entirely in-memory, by design, for the single-process use cases
described so far. Running it as a durable, fault-tolerant service means
replicating that state the same way any other Kythira application would —
using the library's own `state_machine` concept, `node<Types>::submit_command()`,
and `file_persistence` — rather than inventing a separate storage layer. This
also fixes the restart-loses-everything gap noted earlier: a `ca_cluster_node`
that crashes and restarts recovers its state from disk (`file_persistence`)
and, if the disk is gone too, from the other nodes in the cluster via
Kythira's existing snapshot-installation path.

```
ca_cluster_node                                (cmd/ca_cluster_node/main.cpp)
  node<Types>                 — Raft consensus (election, replication, snapshotting)
  ca_state_machine            — apply() records facts only; never generates key material
  file_persistence            — on-disk log + snapshot storage (same engine cmd/chaos_node uses)
  client-facing HTTP API      — /healthz, /v1/root-ca, /v1/certificates(+/renew), /v1/crl
                                 served only when is_leader(); otherwise:
                                   known leader    → 308 + Location: <leader http addr><path>
                                   no known leader → 503 (nowhere to redirect to)

Bootstrap (once, cluster lifetime):
  operator starts one node with --bootstrap-ca
      certificate_authority (fresh) generated locally, in memory
      CA private key encrypted (AES-256-GCM, PBKDF2 from --unseal-key-file)
      submit_command(bootstrap_ca{cert_pem, encrypted_key_pem})
              │
              ▼ replicated + applied on every node (apply() just stores the given bytes)
      every node's ca_state_machine now holds the same encrypted CA material

Issuance (steady state, any time, survives leader failover):
  client → leader's POST /v1/certificates
      leader decrypts bootstrap material once (unseal key from its own config)
          → certificate_authority::from_existing(cert_pem, decrypted_key_pem)
      leader.sign_csr(csr) computed LOCALLY (non-deterministic OpenSSL signing
          happens here, outside apply())
      submit_command(record_issuance{serial, subject, SAN, validity, cert_pem})
              │
              ▼ replicated to a majority, applied on every node (deterministic: just stores cert_pem)
      HTTP response sent only after submit_command()'s future resolves
```

The key design move — the same one that makes a Raft-replicated CA possible at
all — is keeping every non-deterministic operation (key generation, CSR
signing) *outside* `apply()`. The leader computes the result once, using its
own in-memory `certificate_authority`, and the state machine only ever commits
and replicates the finished, already-signed bytes. This is the same pattern
HashiCorp Vault's integrated-storage (Raft) backend uses for its PKI secrets
engine: the root key material and issuance ledger are replicated as opaque,
already-computed values, and only the active (leader) node ever touches the
unwrapped key.

## Components and Interfaces (continued once more)

### 18. `include/raft/ca_state_machine.hpp`

```cpp
namespace raft::testing {

enum class ca_command_type : std::uint8_t { bootstrap_ca = 0, record_issuance = 1, record_revocation = 2 };

struct ca_ledger_entry {
    std::uint64_t serial;
    std::string subject;
    std::vector<std::string> dns_names;
    std::vector<std::string> ip_addresses;
    std::string certificate_pem;             // never a private key
    std::chrono::system_clock::time_point not_before;
    std::chrono::system_clock::time_point not_after;
    std::optional<std::chrono::system_clock::time_point> revoked_at;
};

class ca_state_machine {
public:
    using log_index_t = std::uint64_t;

    auto apply(const std::vector<std::byte>& command, log_index_t index) -> std::vector<std::byte>;
    [[nodiscard]] auto get_state() const -> std::vector<std::byte>;
    auto restore_from_snapshot(const std::vector<std::byte>& snapshot, log_index_t index) -> void;

    // Not part of the state_machine concept; used by ca_cluster_node's HTTP layer.
    [[nodiscard]] auto has_root_material() const -> bool;
    [[nodiscard]] auto encrypted_bootstrap_material() const -> const std::string&;  // "" until bootstrapped
    [[nodiscard]] auto ledger() const -> const std::vector<ca_ledger_entry>&;

private:
    bool _bootstrapped{false};
    std::string _encrypted_ca_key_pem;   // AES-256-GCM ciphertext + nonce + tag, base64
    std::string _root_cert_pem;          // plaintext — the CA cert itself is public
    std::vector<ca_ledger_entry> _ledger;
    log_index_t _last_applied_index{0};
};

}  // namespace raft::testing
```

`apply()` dispatches on `ca_command_type`:
- `bootstrap_ca`: WHEN `_bootstrapped` is already `true`, return an error
  result (boost::json `{"error": "already_bootstrapped"}`) without modifying
  state — deterministic on every replica, since `_bootstrapped` is itself
  part of the replicated state built up by prior `apply()` calls. Otherwise
  store `_root_cert_pem`/`_encrypted_ca_key_pem` from the command and set
  `_bootstrapped = true`.
- `record_issuance`: append a `ca_ledger_entry` built directly from the
  command's fields. No computation, no validation beyond structural parsing —
  the leader already validated everything before proposing this command.
- `record_revocation`: find the ledger entry by serial and set `revoked_at`;
  return an error result if the serial isn't found (can legitimately happen if
  a revoke and a concurrent snapshot-install race — the caller retries).

`get_state()`/`restore_from_snapshot()` serialize/deserialize
`{_bootstrapped, _encrypted_ca_key_pem, _root_cert_pem, _ledger}` as a single
boost::json document — the same library `file_persistence.hpp` already uses,
so no new serialization dependency is introduced.

### 19. `cmd/ca_cluster_node/main.cpp`

Configuration mirrors `cmd/chaos_node/config.hpp`'s `node_config` shape:

```cpp
struct ca_cluster_peer_info {
    std::uint64_t node_id;
    std::string rpc_host;
    std::uint16_t rpc_port;
    std::string http_address;    // e.g. "https://ca-node-2.internal:8443" — for redirects
};

struct ca_cluster_node_config {
    std::uint64_t node_id{0};
    std::string rpc_address{"0.0.0.0"};
    std::uint16_t rpc_port{7000};       // Raft-internal (AppendEntries/RequestVote/InstallSnapshot)
    std::uint16_t http_port{8443};      // client-facing /v1/* API
    std::string data_dir{"/var/lib/ca_cluster_node"};   // file_persistence root
    std::vector<ca_cluster_peer_info> peers;             // RPC address AND http_address per peer
    std::string unseal_key_file;         // passphrase source; same value on every node
    bool bootstrap_ca{false};            // --bootstrap-ca; at most one node per cluster lifetime
    std::chrono::milliseconds election_timeout_min{150};
    std::chrono::milliseconds election_timeout_max{300};
    std::chrono::milliseconds heartbeat_interval{50};
};
```

`ca_cluster_peer_info` extends `cmd/chaos_node`'s plain `peer_info` (which only
carries `host`/`port` for Raft RPC) with `http_address`, because the two
addresses are not derivable from each other — a node building a redirect
target for Requirement 17.7 needs to look up the *client-facing* address of
whichever `node_id` `_known_leader` names, not its RPC address.

`main()`:
1. Parse config (flags + env, following `node_config::from_env()`'s pattern).
2. Construct `file_persistence` rooted at `data_dir`, `ca_state_machine`, and
   `node<Types>` (Raft) wired to both, plus the RPC transport listening on
   `rpc_port`.
3. Start an HTTP server (same `httplib::SSLServer` pattern as
   Requirement 14/component 13) on `http_port`, registering the `/v1/*` routes
   from Requirement 11, each beginning with:
   ```cpp
   if (!raft_node.is_leader()) {
       auto leader_id = raft_node.known_leader();     // std::optional<node_id_type>
       if (leader_id.has_value()) {
           auto leader_http_addr = http_address_for(*leader_id, _config.peers);  // ca_cluster_peer_info lookup
           res.status = 308;
           res.set_header("Location", leader_http_addr + req.path);
       } else {
           res.status = 503;
           res.set_content(to_json({{"error", "no_known_leader"}}), "application/json");
       }
       return;
   }
   ```
   `308 Permanent Redirect` (not `301`/`302`) preserves the request method and
   body on redirect, so `POST /v1/certificates` with a CSR body redirects to
   the same method and body at the leader — an `httplib::Client` configured to
   follow redirects (`set_follow_location(true)`) completes the whole
   round-trip against whichever node it was pointed at, without the caller
   needing its own leader-discovery logic.
4. WHEN `bootstrap_ca` is set and `!state_machine.has_root_material()` (checked
   after the node has caught up on replication — i.e. only once this node
   knows the cluster's true state, not merely its own fresh-disk state):
   generate a `certificate_authority` locally, encrypt its private key with
   the unseal passphrase, and `submit_command(bootstrap_ca{...})`.
5. On becoming leader (or on startup, if already leader from a prior run
   whose in-memory signer was lost to a restart): decrypt
   `state_machine.encrypted_bootstrap_material()` with the configured unseal
   passphrase and construct a `certificate_authority` via
   `certificate_authority::from_existing(root_cert_pem, decrypted_key_pem)`,
   held for as long as this node remains leader.
6. `/v1/certificates` handler (leader-only, per step 3): validate the CSR,
   call the locally-held `certificate_authority::sign_csr()`, then
   `submit_command(record_issuance{...})` and wait on its future before
   responding — per Requirement 17.8.

### 20. Default topology and AWS AZ placement

`ca_cluster_node_config.peers` defaults to describing a 3-node cluster — the
standard minimal odd-sized Raft cluster that keeps a majority (2 of 3)
reachable through any single node failure. All of Requirement 17's example
configuration and the multi-node test in task 26 target this default; nothing
in `ca_state_machine`/`ca_cluster_node` hardcodes the number 3 structurally
(a 5-node config works the same way), but 3 is what ships and what's tested.

On AWS, the 3 nodes SHOULD each live in a different Availability Zone, so
losing one AZ doesn't cost the cluster its quorum. Two ways to achieve this,
both using infrastructure this project already has rather than adding a new
provisioner:

**Manual (matches the systemd/ECS artifacts from Requirement 12's task 14):**
three subnets, one per AZ, each running one `ca_cluster_node` instance (EC2 via
the systemd unit, or an ECS task); each node's `peers` config lists all three
nodes' `rpc_host:rpc_port` and `http_address` values, spanning the three AZs.
No new artifact type is needed — this is the same systemd unit / ECS task
definition from task 14, replicated three times with different `--peers`
values and one distinct AZ/subnet each.

**Automated (opt-in, reuses `aws_ec2_quorum_manager`):** an operator who wants
Kythira to detect and replace a failed CA-cluster node's EC2 instance
automatically can point `aws_ec2_quorum_manager_config.topology` at three
placement groups named by AZ, one node each:

```cpp
kythira::aws_ec2_quorum_manager_config cfg;
cfg.cluster_name = "ca-cluster";
cfg.image_id = "ami-...";                 // AMI running ca_cluster_node
cfg.node_port = 7000;                     // Raft RPC port
cfg.topology.groups = {
    {.group_id = "us-east-1a", .target_count = 1},
    {.group_id = "us-east-1b", .target_count = 1},
    {.group_id = "us-east-1c", .target_count = 1},
};
cfg.subnet_by_group = {
    {"us-east-1a", "subnet-aaa..."},
    {"us-east-1b", "subnet-bbb..."},
    {"us-east-1c", "subnet-ccc..."},
};
```

This is exactly the `group_id` = AZ-name / `subnet_by_group` pattern already
exercised by `tests/aws_quorum_manager_unit_test.cpp`'s
`ec2_construction` suite — `aws_ec2_quorum_manager` doesn't need any
CA-specific change to provision a 3-AZ-spread `ca_cluster_node` fleet; it was
already general-purpose. This spec does not add new code for option (b), only
documents the configuration.

### 21. `certificate_authority::from_existing()`

```cpp
[[nodiscard]] static auto certificate_authority::from_existing(
    std::string ca_cert_pem, std::string ca_key_pem) -> certificate_authority;
```

Parses the supplied cert/key PEM (`PEM_read_bio_X509`, `PEM_read_bio_PrivateKey`),
validates the key matches the cert's public key
(`X509_check_private_key`-equivalent), and constructs a `certificate_authority`
whose `_ca_key`/`_ca_cert`/`_root_pem` are populated from the supplied material
rather than freshly generated. `_serial_counter`/`instance_seed` are still
computed fresh at construction (per Requirement 1.4) — two different
`ca_cluster_node` processes reconstructing the same CA identity via
`from_existing()` after a leader failover do not need to agree on a serial
counter starting point, because serials for facts already committed live in
the replicated ledger (`ca_ledger_entry.serial`), and `next_serial()` for
newly-leader-issued certificates only needs to avoid colliding with *this*
process's own future issuances, not replay history — a fresh `instance_seed`
per process already guarantees that (Requirement 1.4).

## Architecture — ACME provider and test server

`acme_certificate_provider` is a third `certificate_provider` implementation,
alongside `local_certificate_provider` and `aws_acm_pca_provider` — same
`root_certificate_pem()`/`sign_csr()` shape, different backend. Unlike the
other two, ACME (RFC 8555) requires the *client* to actively prove control of
each identifier being certified — there's no ARN or in-process key to just
sign against. `acme_test_server` exists to make that provable without a real
domain, real public DNS, or a real CA's rate limits.

```
acme_certificate_provider                    (include/raft/acme_certificate_provider.hpp)
  account key (ECDSA P-256, generated once, held in memory)
  root_certificate_pem() → top of the most recently downloaded chain
  sign_csr(csr, options):
    directory → new-nonce → new-account (or reuse) → new-order(identifiers)
    for each authorization:
        http_01 → serve key-authorization at /.well-known/acme-challenge/<token>
                    (short-lived httplib::Server on the challenged identifier)
        dns_01  → publish TXT _acme-challenge.<id>. via RFC 2136 UPDATE
                    (reuses rfc2136_ldns_discovery's update/sign/send helpers)
        POST challenge "ready" → poll authorization until valid/invalid
        tear down responder / DNS record either way
    finalize(order, csr) → poll order until valid → GET certificate → chain PEM

acme_test_server                              (tests/acme_test_server.hpp)
  RFC 8555 wire protocol only — no signing logic of its own
  backed internally by ONE certificate_authority instance (its root = the
  chain root acme_certificate_provider sees when talking to this server)
  validates challenges for real (HTTP GET / DNS query) by default;
  fiu_do_on("raft/acme/test_server/skip_challenge_validation", ...) to bypass
  for tests that only care about the ACME state machine, not real challenge
  infrastructure
```

The `dns_01` path is the more interesting reuse: Kythira already has a
complete RFC 2136 DNS UPDATE implementation
(`include/raft/rfc2136_ldns_discovery.hpp`) for a different purpose (dynamic
peer registration). `acme_certificate_provider`'s `dns_01` challenge responder
is the same UPDATE/sign/send machinery pointed at a different record
(`_acme-challenge.<identifier>. TXT "<digest>"` instead of
`<shared_name>. A <ip>`) — not a second DNS client implementation.

## Components and Interfaces (continued yet again)

### 22. `include/raft/acme_certificate_provider.hpp` / `_impl.hpp`

```cpp
struct acme_certificate_provider_config {
    std::string directory_url;                 // e.g. "https://acme.example.com/directory"
    std::optional<std::string> account_key_pem; // reuse an existing account; generate if empty
    std::vector<std::string> contact;            // e.g. {"mailto:ops@example.com"}, optional

    enum class challenge_type { http_01, dns_01 } challenge{challenge_type::http_01};
    std::string http01_bind_address{"0.0.0.0:80"};      // used when challenge == http_01
    rfc2136_ldns_discovery::config dns01_update_config;  // reused verbatim when challenge == dns_01

    std::chrono::seconds poll_timeout{120};
    std::chrono::milliseconds poll_interval{2000};
};

class acme_certificate_provider {
public:
    explicit acme_certificate_provider(acme_certificate_provider_config config);

    auto root_certificate_pem() -> kythira::Future<std::string>;
    auto sign_csr(std::string csr_pem, csr_signing_options options) -> kythira::Future<pem_material>;

private:
    EvpKeyPtr _account_key;
    std::optional<std::string> _account_url;   // populated after the first new-account call
    std::string _last_chain_pem;
    acme_certificate_provider_config _config;
};

static_assert(certificate_provider<acme_certificate_provider>);
```

`sign_csr()` sequence: `GET directory_url` → `HEAD`/`GET` the directory's
`newNonce` URL for a `Replay-Nonce` → JWS-signed `POST` to `newAccount`
(skipped if `_account_url` is already set) → JWS-signed `POST` to `newOrder`
with `options.dns_names`/`ip_addresses` as identifiers → for each
authorization URL in the response, `GET` it, select the `http-01` or `dns-01`
challenge matching `_config.challenge`, stand up the responder (component 23),
`POST` the challenge URL to signal readiness, poll the authorization until
`valid`/`invalid`, tear the responder down → JWS-signed `POST` to the order's
`finalize` URL with the DER-encoded, base64url CSR → poll the order until
`valid` → `GET` the `certificate` URL → split the returned PEM bundle into
leaf + chain, cache the topmost cert for `root_certificate_pem()`, return
`pem_material{leaf_pem, "", full_chain_pem, 0}` (no private key — the CSR's
key was generated by the caller, same as every other `sign_csr()` path).

### 23. JWS/JWK signing and challenge responders

A small internal helper (`detail::jws_sign(payload_json, protected_header_json,
EVP_PKEY*) -> std::string`) builds the RFC 7515 compact serialization
(`base64url(header) + "." + base64url(payload) + "." +
base64url(signature)`) using `EVP_DigestSign`/`EVP_DigestVerify` for ES256 and
a small base64url encode/decode function (standard base64 via OpenSSL's
`EVP_EncodeBlock`, then `+`/`/` → `-`/`_` and padding stripped). The account's
JWK thumbprint (RFC 7638, SHA-256 over a canonical JSON encoding of the public
key) is computed once and reused for every key-authorization value
(`token + "." + thumbprint`, itself base64url-SHA-256'd for `dns-01`'s TXT
value per RFC 8555 §8.4).

`http01_responder`: a short-lived `httplib::Server` (no TLS — the ACME spec
requires the *challenge* to be served over plain HTTP on port 80) registering
exactly one route, `GET /.well-known/acme-challenge/<token>`, returning the
key authorization as the body; started just before signaling challenge
readiness, stopped once the authorization reaches a terminal state.

`dns01_responder`: reuses `rfc2136_ldns_discovery`'s internal UPDATE-building
and TCP-send helpers (refactored into a small shared, non-public
`detail::send_rfc2136_update()` if not already factored that way) to add a
`TXT` record at `_acme-challenge.<identifier>.` with the key-authorization
digest, and to delete it afterward.

### 24. `tests/acme_test_server.hpp`

```cpp
class acme_test_server {
public:
    struct options {
        bool validate_challenges{true};   // false only via the fault-injection bypass, for fast tests
    };

    explicit acme_test_server(options opts = {});
    ~acme_test_server();

    [[nodiscard]] auto directory_url() const -> std::string;   // for acme_certificate_provider_config
    [[nodiscard]] auto root_certificate_pem() const -> const std::string&;

private:
    certificate_authority _ca;             // the ONLY place signing actually happens
    httplib::Server _server;               // implements the RFC 8555 endpoints below
    std::jthread _server_thread;
    // order/authorization/challenge state, keyed by opaque IDs, guarded by a mutex
};
```

Routes registered on construction: `GET /directory`, `HEAD /new-nonce`,
`POST /new-account`, `POST /new-order`, `GET /authz/{id}`,
`POST /challenge/{id}` (triggers validation — an outbound HTTP GET or DNS
query against the identifier being validated, unless
`fiu_do_on("raft/acme/test_server/skip_challenge_validation", ...)` is active),
`POST /finalize/{order-id}` (parses the CSR, calls `_ca.sign_csr()`,
transitions the order to `valid`), `GET /cert/{id}` (returns
`leaf_pem + _ca.root_certificate_pem()`). Every JWS-signed request's
signature is verified against the calling account's registered public key
before any state change — a request with a tampered signature or a stale/
reused nonce is rejected with the RFC 8555 `badSignature`/`badNonce` problem
document, exercising `acme_certificate_provider`'s own error handling.

## Architecture — first-contact bootstrap trust

Requirement 11's bearer token secures one direction of the bootstrap
handshake — the server trusting the client's request. It never addressed the
other direction: when TLS is enabled on `--serve`, a brand-new instance has no
certificate from this CA yet, so it has nothing to chain-verify the server's
own presented certificate against. Requirement 19 closes that gap the same
way `kubeadm join`'s `--discovery-token-ca-cert-hash` does — pin the *root*
certificate's fingerprint (not the leaf, which rotates under Requirement 16's
hot-reload) and distribute it through the same out-of-band channel as the
token:

```
Operator provisioning step (once, out-of-band):
  ca_service --print-root-fingerprint  →  sha256:AB:CD:...
  Distribute alongside the bearer token: same EnvironmentFile / user_data /
  secrets-manager entry that already carries CA_SERVICE_AUTH_TOKEN.

New instance, first contact:
  ca_bootstrap_client::fetch_trusted_root(base_url, expected_fingerprint)
      TLS-connect, DO NOT chain-verify (nothing to verify against yet)
      compute sha256 of the presented chain's root cert
      match? → fetch GET /v1/root-ca over this connection, cache PEM, return
      mismatch → abort, no fallback
  ↓ (from here on, normal chain verification against the cached root)
  POST /v1/certificates  (Authorization: Bearer <token>, verified against
                           the now-cached root — no more pinning needed)
```

Pinning is deliberately a *one-time* bootstrap step, not the ongoing trust
model — once the root PEM is fetched and cached, every subsequent connection
(including the very request that gets the first certificate) uses ordinary
X.509 chain verification. This is what decouples the pin from leaf rotation:
the root the fingerprint identifies never changes across a `reload_tls_material()`
call, only the leaf being served does.

## Components and Interfaces (one more time)

### 25. Server-side root-fingerprint printing

`ca_service` / `ca_cluster_node`, when TLS is enabled, compute the SHA-256
fingerprint of the root certificate backing their listener
(`certificate_authority::root_certificate_pem()` for the `local` provider; the
external issuer's root when an operator-supplied `--tls-cert` comes from
elsewhere) via `X509_digest(cert, EVP_sha256(), ...)`, and:
- Log it once at startup (`Root certificate fingerprint (sha256): AB:CD:...`).
- Support `--print-root-fingerprint`: load just enough config to construct
  the same root (or read an existing `--tls-cert` file directly), print the
  fingerprint, and exit 0 without binding any port — a read-only,
  side-effect-free mode safe to run repeatedly in a provisioning script.

### 26. `include/raft/ca_bootstrap_client.hpp`

```cpp
namespace raft::testing {

struct ca_bootstrap_result {
    std::string root_certificate_pem;
};

// Connects to `base_url` over TLS WITHOUT chain verification, checks the
// presented chain's root against `expected_root_fingerprint_sha256` (hex,
// colon-separated or bare — both accepted), and on an exact match fetches
// GET /v1/root-ca over that same connection. Throws on any mismatch; never
// falls back to unpinned verification.
[[nodiscard]] auto fetch_trusted_root(std::string base_url,
                                       std::string expected_root_fingerprint_sha256)
    -> ca_bootstrap_result;

}  // namespace raft::testing
```

Implementation: an `httplib::Client` configured with
`enable_server_certificate_verification(false)` (disabling httplib's normal
chain check, since there's nothing to chain-verify against yet) plus a
certificate callback — the exact cpp-httplib hook name/signature for
inspecting the peer certificate mid-handshake SHALL be confirmed against the
vendored version during implementation, following the same
verify-before-committing-to-an-API-shape approach already used for the
`ca_service` renewal route's peer-certificate lookup (component 17). The
callback computes `X509_digest(peer_root, EVP_sha256(), ...)`, hex-encodes it,
and compares case-insensitively against `expected_root_fingerprint_sha256`,
raising `std::runtime_error` naming both values on mismatch. On success, the
same connection issues `GET /v1/root-ca` and returns its body as
`ca_bootstrap_result::root_certificate_pem`.

Callers (a real node's provisioning script, or a future non-test-only
equivalent of `ca_test_fixture` for production use) are expected to use this
function *exactly once* per instance lifetime — to obtain the root PEM before
making any further requests — and then construct their normal
`cpp_httplib_client_config`/`coap_client_config` with `ca_cert_path` pointing
at that fetched root for every subsequent connection, including the bootstrap
`POST /v1/certificates` call itself.

## Architecture — LAN/mDNS, IP-only, and DNS-integrated node configurations

The non-ACME issuance paths (`certificate_authority::issue()`, `ca_test_fixture::bootstrap_client()`,
`ca_service`'s bearer-token-authenticated `/v1/certificates`) already impose
no constraint on what a SAN looks like — they never validate domain control
in the first place, so mDNS names, bare IPs, and real DNS names are already
interchangeable inputs. ACME is the one path where identifier *type* actually
changes behavior, because each type has a different, protocol-defined way to
prove control of it:

```
sign_csr() per-identifier dispatch (acme_certificate_provider):

  identifier          ACME type   allowed challenge     validation mechanism
  ─────────────────   ─────────   ────────────────────  ──────────────────────────────
  "node1.example.com" "dns"       http-01 or dns-01      http-01: short-lived httplib
                                   (per config)             server on the identifier
                                                          dns-01: RFC 2136 UPDATE of
                                                             _acme-challenge.<id>. TXT
                                                             (same helper rfc2136_ldns_
                                                             discovery already uses)

  "node1.local"        "dns"      http-01 ONLY            http-01, but the *validator*
                        (mDNS is                          (acme_test_server) resolves
                         not a                             "node1.local" via plain
                         separate                          getaddrinfo() — mDNS-capable
                         ACME id                            only if the OS resolver is
                         type)                              configured for it (nss-mdns/
                                                             Avahi, mDNSResponder); no
                                                             custom mDNS client code

  "10.0.0.5"           "ip"       http-01 ONLY            http-01, direct IP connection;
                        (RFC 8738)                          dns-01 is invalid for "ip"
                                                             identifiers per RFC 8738 —
                                                             never attempted
```

A single order's identifiers can span more than one row of this table — a
node bootstrapping with both an mDNS name and a bare IP as SANs gets each one
validated by whatever that identifier's own type requires, not one fixed
mode for the whole request. `.local` names are *not* a distinct ACME
identifier type in RFC 8555/8738 — they're ordinary `"dns"` identifiers whose
only viable challenge happens to be `http-01`, because `dns-01` would need a
real DNS zone to publish a TXT record in, which `.local` doesn't have.

## Components and Interfaces (extending `acme_certificate_provider`/`acme_test_server`)

### 27. Per-identifier challenge-type dispatch in `acme_certificate_provider`

`sign_csr()`'s per-authorization loop (component 22) is extended with a
dispatch step before selecting a challenge:

```cpp
enum class acme_identifier_type { dns, ip };

auto classify(const std::string& identifier) -> acme_identifier_type {
    // Parse as an IPv4/IPv6 literal (e.g. via inet_pton); anything else is "dns",
    // including .local names — mDNS is not a separate identifier type.
    return is_ip_literal(identifier) ? acme_identifier_type::ip : acme_identifier_type::dns;
}

auto challenge_for(const std::string& identifier, acme_certificate_provider_config::challenge_type configured)
    -> std::string /* "http-01" | "dns-01" */ {
    if (classify(identifier) == acme_identifier_type::ip) {
        return "http-01";   // RFC 8738: dns-01 is not defined for "ip" identifiers
    }
    return configured == acme_certificate_provider_config::challenge_type::dns_01
               ? "dns-01" : "http-01";
}
```

The `newOrder` request's per-identifier JSON object carries `type: "ip"` or
`type: "dns"` per RFC 8555 §7.1.3/RFC 8738 §3, set from `classify()` — this
is what tells the ACME server which validation methods are even legal for
that identifier, independent of which challenge `acme_certificate_provider`
then selects from the ones the server offers.

### 28. `.local` resolution in `acme_test_server`

`acme_test_server`'s `POST /challenge/{id}` handler (component 24), when
performing real `http-01` validation, resolves the target host via ordinary
`getaddrinfo()` — the same call used for any other hostname. No mDNS-specific
code is added; `.local` resolution works precisely when the host running
`acme_test_server` has an mDNS-capable resolver configured (`nss-mdns` +
Avahi on Linux, native on macOS) and fails with a distinguishable
`mdns_resolver_unavailable`-style error (Requirement 20.6) otherwise —
detected by checking `errno`/`EAI_NONAME` against a preflight capability
probe (e.g. attempting to resolve a well-known `.local` sentinel, or
checking for `libnss_mdns` in `/etc/nsswitch.conf`'s `hosts:` line) rather
than letting a generic resolution failure masquerade as "challenge failed."

## Data Models

### `pem_material` example (leaf, server+client dual-purpose)

```
certificate_pem:  -----BEGIN CERTIFICATE-----\n...\n-----END CERTIFICATE-----\n
private_key_pem:  -----BEGIN PRIVATE KEY-----\n...\n-----END PRIVATE KEY-----\n
chain_pem:        <certificate_pem><root_certificate_pem>
serial:           <uint64>
```

Decoded extensions on a leaf produced by `issue({.dns_names={"node1"}, .server_auth=true, .client_auth=true})`:

```
X509v3 Basic Constraints: critical
    CA:FALSE
X509v3 Key Usage: critical
    Digital Signature, Key Encipherment
X509v3 Extended Key Usage:
    TLS Web Server Authentication, TLS Web Client Authentication
X509v3 Subject Alternative Name:
    DNS:node1
```

### `ca_service` output layout

```
<out-dir>/
  root_ca.pem
  mtls-node1/
    cert.pem
    key.pem       (mode 0600)
    chain.pem
  mtls-node2/
    cert.pem
    key.pem       (mode 0600)
    chain.pem
```

## Correctness Properties

### Property 1: Issued leaves chain-verify against their own root
**Validates: Requirements 1, 2**

For any `pem_material m = ca.issue(opts)`, loading `m.certificate_pem` and
`ca.root_certificate_pem()` into an OpenSSL `X509_STORE` and calling
`X509_verify_cert()` SHALL succeed (return 1), because the leaf was signed with
the same CA private key whose public counterpart is embedded in the root
certificate.

### Property 2: Time-window presets fail exactly on time
**Validates: Requirements 3.1, 3.2**

A certificate from `issue_expired()` fails `X509_verify_cert()` at the current
time with `X509_V_ERR_CERT_HAS_EXPIRED`, but the *same* certificate would pass
signature/chain checks if the verifier's clock were set to a time between its
(backdated) `notBefore` and `notAfter` — i.e. the failure is purely
time-window-driven, not a malformed signature. Symmetrically for
`issue_not_yet_valid()` and `X509_V_ERR_CERT_NOT_YET_VALID`.

### Property 3: Cross-CA leaves fail verification
**Validates: Requirements 3.3**

For two instances `ca_a`, `ca_b` and `m = ca_a.issue(opts)`, verifying
`m.certificate_pem` against a trust store containing only
`ca_b.root_certificate_pem()` fails with
`X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT_LOCALLY` (or equivalent) because `ca_b`'s
key never signed `m`.

### Property 4: Revocation is enforced when CRL checking is enabled
**Validates: Requirements 4.1, 4.2**

After `ca.revoke(m)`, verifying `m.certificate_pem` with
`X509_V_FLAG_CRL_CHECK` set and `ca.crl_pem()` loaded into the store fails with
`X509_V_ERR_CERT_REVOKED`. Without CRL checking enabled (the default for most
existing transport tests), the same certificate still verifies — revocation is
opt-in enforcement, matching how `coap_server_config::verify_peer_cert` and
`cpp_httplib_server_config::require_client_cert` are themselves opt-in.

### Property 5: `temp_cert_files` cleanup is unconditional
**Validates: Requirements 5.3**

Because cleanup happens in the destructor via `std::filesystem::remove_all` with
an `std::error_code` overload (never the throwing overload), stack unwinding
during a test failure (e.g. a `BOOST_CHECK` failure inside the test body, which
does not throw, or an unrelated exception) still runs the destructor and removes
the directory. No test run leaves key material behind in `/tmp`.

### Property 6: `ca_service` never bakes in a static IP unless asked
**Validates: Requirements 7.3**

Running `ca_service` without `--resolve-ips` produces leaf certificates whose
SAN contains only DNS entries; grepping the output PEM's decoded SAN extension
for an IP literal SHALL find none. This directly satisfies the project's
container-runtime-compatibility rule against embedding static IPs in test
infrastructure — the compose file in Requirement 7.7 relies on DNS-name SANs
resolved dynamically by whichever DNS mechanism the consuming scenario already
uses (compose's embedded DNS, or one of the `peer_discovery` implementations).

### Property 7: CSR-signed certificates chain-verify identically to key-generated ones
**Validates: Requirements 9.1, 9.4**

For `m = ca.sign_csr(csr_pem, opts)` where `csr_pem` came from
`generate_key_and_csr(...)`, `m.certificate_pem` verifies against
`ca.root_certificate_pem()` via `X509_verify_cert()` exactly as in Property 1 —
`sign_csr()` and `issue()` share the same leaf-construction and signing code
path, differing only in where the public key comes from.

### Property 8: Private key material never crosses the CA boundary
**Validates: Requirements 9.1, 11.3**

`pem_material` returned by `sign_csr()` (locally or via `aws_acm_pca_provider`)
always has an empty `private_key_pem`. An integration test on `ca_service`'s
`/v1/certificates` route asserts the HTTP response body contains no
`-----BEGIN PRIVATE KEY-----` (or `RSA PRIVATE KEY`/`EC PRIVATE KEY`) block in
either direction of the exchange — the request carries only a CSR, the response
only certificates.

### Property 9: `ca_service` network API is fail-closed on authentication
**Validates: Requirements 11.4**

Every route under `/v1/` and `/healthz` rejects a request lacking a matching
`Authorization: Bearer` header with `401`, regardless of which
`certificate_provider` is active, and the process refuses to start at all in
`--serve` mode without a configured token — there is no code path that leaves
the network API reachable without authentication.

### Property 10: `bootstrap_client()` issues distinct, non-impersonatable identities
**Validates: Requirements 13.6**

For `a = fixture.bootstrap_client("client-a", {"client-a"})` and
`b = fixture.bootstrap_client("client-b", {"client-b"})`, both `a.cert_path()`
and `b.cert_path()` chain-verify against `fixture.root_certificate_pem()`
(Property 1/7, whichever mode is active), but their subjects and SAN entries
differ, their key pairs are independently generated, and a TLS server
configured with `dns_names = {"client-b"}` rejects a handshake attempted with
`a`'s certificate on hostname-verification grounds — matching the "wrong
hostname" scenario from Requirement 3.4, now reachable through the higher-level
fixture rather than only through raw `issue()`.

### Property 11: Established connections survive a reload
**Validates: Requirements 16.1, 16.2, 16.4**

An HTTP client holds a keep-alive connection open through a server-side
`reload_tls_material()` call; the in-flight connection continues to serve
requests successfully afterward (its `SSL*` object was created from the old
default certificate, which `SSL_CTX_use_certificate_chain_file` does not
retroactively alter). The equivalent property test for `coap_transport` holds
an established DTLS association open across a `reload_tls_material()` call on
the server.

### Property 12: New connections after reload see the new certificate
**Validates: Requirements 16.4**

A *new* TLS/DTLS handshake started after `reload_tls_material()` presents the
newly loaded certificate — verified by inspecting the peer certificate's
serial number (or SAN, if the test rotates identity as well as validity) on
the client side of a fresh connection, and confirming it matches the
certificate passed to `reload_tls_material()`, not the one the server started
with.

### Property 13: Reload is atomic — invalid material never disrupts service
**Validates: Requirements 16.3**

Calling `reload_tls_material()` with a mismatched cert/key pair, or with a
path to a file that doesn't parse as a certificate, throws, and a *subsequent*
new connection still completes successfully against the *original*
certificate — the failed reload attempt left the live context untouched.

### Property 14: `renew()` preserves identity across the file swap
**Validates: Requirements 15.1, 15.3, 15.4**

For `a = fixture.bootstrap_client("node1", {"node1"})` and, later,
`fixture.renew("node1")`, the certificate now readable at `a.cert_path()` has
the same subject and SAN as the original but a later `notBefore`/`notAfter`
window and a different serial — chain-verifying against the same
`root_certificate_pem()` throughout. For the `ca_service` renewal route, a
client renewing via `POST /v1/certificates/renew` receives a certificate whose
subject/SAN matches what it presented, and a certificate from an unrelated
CA is rejected with `401` rather than triggering an identity-laundering
reissue.

### Property 15: `ca_state_machine::apply()` is deterministic across replicas
**Validates: Requirements 17.1, 17.2**

Applying the identical sequence of `bootstrap_ca`/`record_issuance`/
`record_revocation` commands (as raw bytes) to two independent
`ca_state_machine` instances produces byte-identical `get_state()` output from
both — verified directly, without needing a running Raft cluster, by feeding
the same command sequence to two instances in a single test process. This is
the property the whole design depends on: if it doesn't hold, replicas would
diverge the first time a leader failed over.

### Property 16: The CA private key is never persisted or replicated in plaintext
**Validates: Requirements 17.4**

Inspecting every byte sequence that reaches `ca_state_machine::apply()`,
`get_state()`, and therefore the log/snapshot files `file_persistence` writes
to disk, for the `-----BEGIN...PRIVATE KEY-----` PEM marker in cleartext finds
none — only the AES-256-GCM ciphertext produced by `ca_cluster_node` before
committing `bootstrap_ca`. A test asserts this by grepping the on-disk log and
snapshot files after a bootstrap, not just the in-memory command bytes.

### Property 17: An acknowledged issuance survives leader failover
**Validates: Requirements 17.8**

A certificate issued via `POST /v1/certificates` — for which the client
received a `200` response — appears in `ledger()` on every node that
subsequently becomes leader, including a node that was not the leader at
issuance time. Verified by issuing a certificate, killing the leader, waiting
for a new leader to be elected, and querying the new leader's ledger (e.g. via
`GET /v1/root-ca` plus a ledger-inspection path, or by attempting to renew the
issued certificate against the new leader and confirming it recognizes the
serial).

### Property 18: An ACME-obtained certificate chains to the issuing server's root
**Validates: Requirements 18.1, 18.6, 18.9**

For `m = acme_certificate_provider.sign_csr(csr, opts)` obtained against a
running `acme_test_server`, `m.certificate_pem`/`m.chain_pem` chain-verify via
`X509_verify_cert()` against `acme_test_server.root_certificate_pem()` —
exactly Property 1/7, now reached through the ACME wire protocol instead of a
direct in-process call, proving the protocol translation preserves the same
cryptographic guarantee.

### Property 19: Challenge responders are torn down regardless of outcome
**Validates: Requirements 18.3, 18.4**

Whether an authorization resolves `valid` or `invalid`, the `http-01`
responder is no longer listening and the `dns-01` TXT record is no longer
present within a bounded time after `sign_csr()`'s returned future settles
(resolved or rejected) — verified by attempting to `GET` the well-known path
(expecting connection refused) or query the TXT record (expecting `NXDOMAIN`)
immediately after both a successful and a deliberately-failed
(`skip_challenge_validation` off, wrong token) issuance attempt.

### Property 20: Pinned verification accepts only the exact expected root
**Validates: Requirements 19.3, 19.4**

`fetch_trusted_root(url, fp)` against a server whose root fingerprint is
`fp` succeeds and returns that root's PEM. The identical call against a
*different* server — backed by a different `certificate_authority` instance,
producing a perfectly valid, well-formed certificate chain, just not the one
`fp` names — fails with a mismatch error rather than silently accepting it.
"Valid X.509" and "the pinned identity" are independent checks; passing one
never substitutes for the other.

### Property 21: IP identifiers never attempt `dns-01`
**Validates: Requirements 20.3, 20.5**

For any `csr_signing_options` containing at least one IP address in
`ip_addresses`, inspecting the ACME requests `acme_certificate_provider`
sends for that identifier shows `identifier.type == "ip"` and the selected
challenge is always `http-01` — regardless of `challenge_type` being
configured to `dns_01`. A mixed request (one DNS name, one IP) validates the
DNS name however `challenge_type` says to, and the IP always via `http-01`,
in the same `sign_csr()` call.

### Property 22: `.local` validation degrades to a distinguishable error, never silent DNS
**Validates: Requirements 20.2, 20.6**

On a host with no mDNS-capable resolver configured, requesting a certificate
for a `.local` identifier through `acme_test_server` fails with an error
identifying the missing mDNS capability specifically — it does not fall
through to querying public DNS for `node1.local` (which would either time out
or, worse, resolve to something unrelated on a network with split-horizon
DNS), and it is not reported as an indistinguishable generic "challenge
failed."

## Error Handling

- **OpenSSL call failures** (key generation, signing, extension construction):
  the implementation SHALL drain the OpenSSL error queue
  (`ERR_get_error`/`ERR_error_string_n`) and throw `std::runtime_error` carrying
  the resulting message. No raw OpenSSL error codes are exposed in the public
  header.
- **Invalid `leaf_certificate_options`** (empty SAN set): `std::invalid_argument`
  per Requirement 2.5.
- **`revoke()` on an unknown serial**: `std::invalid_argument` per
  Requirement 4.3.
- **`temp_cert_files` construction failures** (cannot create directory or write
  a file): propagate as `std::filesystem::filesystem_error` — construction
  failure is a test-setup error the caller should see immediately, unlike
  destructor cleanup which is always best-effort.
- **`ca_service` argument/runtime errors**: caught in `main()`, printed to
  `stderr`, non-zero exit — never a crash or an uncaught-exception `abort()`.
- **Malformed or forged CSR** (`sign_csr()`): `X509_REQ_verify()` failing (the
  CSR's self-signature doesn't match its embedded public key) or unparseable
  PEM SHALL throw `std::invalid_argument` — never silently sign a request whose
  proof-of-possession of the private key can't be verified.
- **AWS ACM Private CA errors**: throttling, access-denied, or the target CA
  being in `DISABLED`/`PENDING_CERTIFICATE` state all reject the returned
  future with the AWS SDK's error message attached, per Requirement 10.6. A
  `GetCertificate` poll that never succeeds within `api_timeout` rejects with a
  `std::runtime_error` naming the elapsed time and the last-seen AWS status, so
  a caller can distinguish "still issuing, just slow" from "actually failed" in
  logs.
- **`ca_service --serve` authentication failures**: every route returns `401`
  with no response body beyond the status — the failure reason is never echoed
  back to an unauthenticated caller.
- **`ca_service --serve` unsupported operations**: `/v1/certificates/revoke`
  and `/v1/crl` under the `aws-acm-pca` provider (without CRL/OCSP configured)
  return `501 Not Implemented` with a short JSON body naming the unsupported
  operation, rather than a generic `500`.
- **Conflicting CLI modes**: `ca_service` invoked with both `--serve` and
  `--out-dir`/`--service` prints a usage error and exits non-zero before
  constructing any provider.
- **`cpp_httplib_server::configure_ssl_server()` genuine failures** (post-fix):
  missing/mismatched cert-key pair, invalid cipher suite string, or
  `require_client_cert` set without a CA path SHALL still throw
  `ssl_configuration_error` with a message naming the specific problem — only
  the unconditional "not fully implemented" throw is removed.
- **`reload_tls_material()` failures** (both transports): thrown as
  `ssl_configuration_error` (HTTP) / `certificate_validation_error` (CoAP);
  callers invoking it directly see the exception immediately; the background
  `enable_auto_reload()` poller catches it, logs via the transport's existing
  metrics/logger path, and continues polling (Requirement 16.7) rather than
  stopping the reload thread.
- **`renew()` on an unbootstrapped `client_id`**: `std::invalid_argument`, per
  Requirement 15.2.
- **`POST /v1/certificates/renew` without a valid client certificate**: `401`,
  per Requirement 15.4 — the same failure mode as a missing bearer token on
  every other route, just keyed on the mTLS peer certificate instead.
- **`ca_state_machine::apply()` on a duplicate `bootstrap_ca`**: returns a
  structured error result (`{"error": "already_bootstrapped"}`), per
  Requirement 17.2 — it does NOT throw, because a thrown exception from
  `apply()` would need identical handling across every replica to remain
  deterministic, and a normal returned value is simpler to keep consistent
  than an exception type.
- **Client-facing request at a non-leader `ca_cluster_node`**: `308` with a
  `Location` header pointing at the known leader's client-facing address, per
  Requirement 17.7 — an `httplib::Client` with `set_follow_location(true)`
  handles this transparently; a caller not auto-following redirects (or a
  future `ca_test_fixture` service-mode extension targeting a cluster) reads
  `Location` and retries there itself. WHEN no leader is known yet (e.g.
  mid-election), `503` with `{"error": "no_known_leader"}` instead — there is
  nothing to redirect to.
- **Unseal passphrase mismatch across nodes**: a node that decrypts
  `encrypted_bootstrap_material()` with the wrong passphrase gets garbage
  plaintext; AES-256-GCM's authentication tag makes this detectable
  (decryption fails integrity verification rather than silently producing
  wrong-but-plausible key bytes) — `ca_cluster_node` SHALL treat this as fatal
  at startup (refuse to become a serving candidate) rather than attempting to
  operate with an unusable signer.
- **`ca_cluster_node` started with `--bootstrap-ca` against an
  already-bootstrapped cluster**: the resulting `submit_command()` succeeds at
  the Raft layer (the command replicates) but `apply()`'s
  `already_bootstrapped` result means no state changed; `ca_cluster_node`
  SHALL treat this as a harmless no-op (log a warning), not a fatal error —
  operators re-running a bootstrap flag against a long-lived cluster (e.g. in
  an idempotent deployment script) shouldn't break anything.
- **ACME challenge/order failure** (`acme_certificate_provider`): a terminal
  `invalid` authorization or order status, or the `poll_timeout` elapsing
  first, rejects `sign_csr()`'s future with a message including the RFC 8555
  problem document's `type`/`detail` fields when the server supplied one.
- **ACME JWS/nonce errors**: a `badNonce` response SHALL be retried once
  automatically (RFC 8555 §6.5 explicitly allows this — the response carries a
  fresh nonce to retry with); any other `4xx`/`5xx` problem document is
  surfaced to the caller as a rejected future, not retried silently.
- **`acme_test_server` request validation**: an invalid JWS signature, unknown
  account, or reused nonce is rejected with the matching RFC 8555 problem
  type (`urn:ietf:params:acme:error:badSignature`, `...:accountDoesNotExist`,
  `...:badNonce`) rather than a generic `400`, so `acme_certificate_provider`'s
  own error-surfacing can be tested against realistic server responses.
- **`fetch_trusted_root()` fingerprint mismatch**: throws `std::runtime_error`
  naming both the expected and observed SHA-256 fingerprints (never just
  "verification failed") — an operator debugging a failed provisioning run
  needs to see both values to tell a typo in the distributed fingerprint
  apart from a genuine man-in-the-middle or misconfigured server.
- **`--print-root-fingerprint` with no TLS material configured**: prints a
  usage error and exits non-zero rather than fabricating or guessing a root
  — there's nothing to fingerprint if TLS was never enabled.
- **`.local` resolution failure** (no mDNS-capable resolver on the
  `acme_test_server` host): a distinct, named error condition (Requirement
  20.6), never conflated with "the node didn't respond to the challenge" —
  the fix for one (install/configure `nss-mdns`) is unrelated to the fix for
  the other (the node's `http-01` responder isn't actually reachable).
- **RFC 2136 UPDATE failure during `dns-01`**: also its own distinguishable
  error, per Requirement 20.6, consistent with how `rfc2136_ldns_discovery`
  already surfaces UPDATE failures rather than folding them into a generic
  DNS error.
- **`ca_test_fixture` service-mode startup failure**: if the `ca_service` child
  process never reports `/healthz` healthy within `startup_timeout`, the
  constructor kills the child (best-effort) and throws `std::runtime_error`
  naming the timeout — a test relying on this fixture fails fast at setup
  rather than hanging or failing later with a confusing connection error from
  `bootstrap_client()`.

## Testing Strategy

- **`certificate_authority_unit_test.cpp`** (new): constructs CAs with each
  `key_algorithm`, issues leaves with various SAN/EKU combinations, and asserts
  the extensions using OpenSSL parsing helpers (`X509_get_ext_d2i`, etc.) rather
  than string-matching PEM.
- **`certificate_authority_property_test.cpp`** (new): verifies Properties 1–4
  above directly against `X509_verify_cert()`, covering both algorithms
  (RSA and ECDSA) and both positive and negative cases.
- **`temp_cert_files_unit_test.cpp`** (new): asserts file contents match the
  source `pem_material`, key file permissions are `0600`, and the directory is
  gone after the object is destroyed (including when destruction happens via
  stack unwinding after a thrown exception in a `BOOST_CHECK_THROW` block).
- **Migrated existing tests** (Requirement 6): `http_ssl_mutual_tls_integration_test.cpp`
  and the `coap_*` cert/DTLS tests gain a `certificate_authority` fixture member
  and use `temp_cert_files` in place of `create_temp_cert_file(valid_cert_pem)`-style
  helpers for their valid-material cases.
- **`ca_service` integration test** (new, `tests/ca_service_integration_test.cpp`
  or a small shell-based `ctest` case): runs the built `ca_service` binary
  against a temporary `--out-dir`, then loads the resulting `root_ca.pem` and
  per-service `cert.pem`/`chain.pem` through the same OpenSSL verification
  helpers as the property tests, confirming the CLI-produced material chain-
  verifies exactly like library-produced material.
- All new Boost.Test cases use the two-argument `BOOST_AUTO_TEST_CASE(..., *
  boost::unit_test::timeout(N))` form per the project's test standards; unit
  tests use a 10–30 s timeout, property tests 60–120 s.
- **`certificate_provider`/CSR coverage** (new, part of
  `certificate_authority_property_test.cpp`): verifies Property 7
  (`sign_csr()` output chain-verifies) and Property 8 (no private key ever
  appears in `sign_csr()` output) directly, without needing `ca_service` or
  AWS.
- **`aws_acm_pca_provider_unit_test.cpp`** (new): mirrors
  `aws_quorum_manager_unit_test.cpp` — config validation and
  `fiu_enable("raft/aws/acm_pca/...")`-forced error paths, requiring no live
  AWS credentials or network access, gated `#ifdef KYTHIRA_HAS_AWS_ACM_PCA`.
- **`aws_acm_pca_provider_localstack_test.cpp`** (new): gated the same way as
  `aws_quorum_manager_localstack_test.cpp`, additionally skipping (not
  failing) when the target LocalStack instance doesn't support `acm-pca` —
  ACM Private CA emulation is a LocalStack **Pro** feature, unlike the EC2/ASG
  APIs already tested against LocalStack community edition. The skip check
  SHALL probe a lightweight `acm-pca` call first and skip the suite on any
  `NotImplemented`/`InternalFailure` response, following the same
  "unreachable/unsupported → skip" convention already used for the EC2/ASG
  LocalStack fixtures.
- **`aws_acm_pca_provider_real_test.cpp`** (new): opt-in, real-AWS integration
  test requiring a pre-provisioned ACM Private CA ARN supplied via
  `$KYTHIRA_TEST_ACM_PCA_ARN`; skipped when that variable is unset. Tagged and
  excluded from the default `ctest` run, matching
  `aws_quorum_manager_real_ec2_test.cpp`.
- **`ca_service` serve-mode integration test** (new,
  `tests/ca_service_serve_integration_test.cpp`): starts `ca_service --serve`
  as a child process with `--provider local` and a known `--auth-token`,
  exercises `/healthz`, `/v1/root-ca`, and `/v1/certificates` (submitting a CSR
  from `generate_key_and_csr()`), asserts Properties 7–9, and confirms the
  process shuts down cleanly on `SIGTERM`.
- **`tests/ca_test_fixture_unit_test.cpp`** (new): exercises
  `ca_test_fixture` in both modes — in-process (default) and
  `start_network_service = true` — asserting Property 10 (distinct,
  non-impersonatable identities from repeated `bootstrap_client()` calls) and
  that `root_certificate_pem()` returns a consistent value regardless of mode.
  Once this fixture exists, `http_ssl_mutual_tls_integration_test.cpp` and the
  `coap_*` migrations from Requirement 6 SHOULD be written against
  `ca_test_fixture::bootstrap_client()` directly, rather than constructing a
  `certificate_authority` and `temp_cert_files` by hand.
- **`cpp_httplib_server` real-TLS regression coverage** (Requirement 14): the
  three existing test files that currently tolerate
  `ssl_configuration_error("...not fully implemented...")` as an acceptable
  outcome are rewritten to assert success — a completed handshake, correct
  peer-certificate verification under `require_client_cert`, and genuine
  configuration errors (bad cipher string, mismatched key) still throwing.
- **Hot-reload property tests** (new,
  `tests/http_transport_reload_property_test.cpp` and
  `tests/coap_transport_reload_property_test.cpp`): verify Properties 11–13 —
  an in-flight connection/association survives a reload, a fresh connection
  after reload sees the new certificate, and an invalid reload attempt leaves
  the transport serving the previous certificate. `enable_auto_reload()` is
  tested with a short poll interval (e.g. 200 ms) against a file replaced via
  `ca_test_fixture::renew()`, asserting the transport picks up the change
  within a small bounded number of poll intervals without any explicit
  `reload_tls_material()` call from the test.
- **`ca_test_fixture::renew()` coverage** (extends
  `tests/ca_test_fixture_unit_test.cpp`): verifies Property 14 — identity
  preserved, validity window and serial advanced, and `renew()` on an unknown
  `client_id` throws.
- **`ca_service` renewal-route integration test** (extends
  `tests/ca_service_serve_integration_test.cpp`): bootstraps a client cert via
  `/v1/certificates`, connects to `/v1/certificates/renew` presenting it over
  mTLS, asserts the renewed certificate's subject/SAN match and its validity
  window is fresh, and asserts a request presenting an unrelated
  (`certificate_authority`-from-a-different-instance) certificate is rejected
  with `401`.
- **`tests/ca_state_machine_unit_test.cpp`** (new): feeds identical command
  sequences to two `ca_state_machine` instances and asserts byte-identical
  `get_state()` (Property 15) without needing a Raft cluster; separately
  asserts a second `bootstrap_ca` command is rejected deterministically, and
  that `restore_from_snapshot(get_state())` round-trips.
- **`tests/ca_cluster_node_test.cpp`** (new, multi-process or
  multi-in-process-node, following the pattern already used by
  `raft_multi_node_test_fixture.hpp`): brings up a 3-node `ca_cluster_node`
  cluster with `file_persistence` pointed at per-node temp directories,
  bootstraps it, issues a certificate, and asserts:
  - Property 16 by grepping the on-disk log/snapshot files under each node's
    `data_dir` for a plaintext private-key PEM marker and finding none.
  - Property 17 by killing the leader process, waiting for a new election,
    and confirming the new leader's ledger contains the pre-failover
    issuance.
  - A full node restart (kill, restart pointed at the same `data_dir`)
    recovers its state from disk without needing to resync from peers for
    already-committed entries.
  - A request to a follower's `/v1/certificates` gets `308` with a `Location`
    header pointing at the leader, and following it succeeds; a request made
    while no leader is known (e.g. immediately after killing the leader,
    before a new one is elected) gets `503`.
  This test is heavier than the rest of the suite (multi-node, real disk I/O,
  real elections) and SHOULD use timeouts at the upper end of the project's
  "network tests: 60–180 seconds" guideline.
- **`tests/acme_certificate_provider_test.cpp`** (new): runs
  `acme_certificate_provider` against `acme_test_server` end-to-end for both
  `http_01` and `dns_01` (the latter needs a reachable DNS server accepting
  RFC 2136 UPDATE — reuse the same BIND9 container/fixture already used by
  the `rfc2136_ldns_discovery` docker-chaos scenarios, or run it as a
  docker-chaos-tagged test rather than in the default `ctest` run if a local
  DNS server isn't available in every CI environment). Verifies Property 18
  (chain-of-trust) and Property 19 (responder/record teardown), plus the
  negative paths from Requirement 18.7 (invalid JWS, failed challenge,
  expired order) using `acme_test_server`'s error responses.
- **`tests/acme_test_server_unit_test.cpp`** (new): exercises the mock server
  directly (bypassing `acme_certificate_provider`) — malformed JWS, unknown
  account, nonce reuse, and the `skip_challenge_validation` fault-injection
  path, confirming each maps to the correct RFC 8555 problem-document type.
- **`tests/ca_cluster_node_aws_localstack_test.cpp`** (new, opt-in): configures
  `aws_ec2_quorum_manager` with the 3-AZ topology from component 20 (one
  placement group per AZ, `target_count = 1`) pointed at LocalStack, gated the
  same way as `aws_quorum_manager_localstack_test.cpp`. LocalStack does not
  execute `user_data` or simulate real inter-instance networking/compute (per
  the aws-quorum-manager spec), so this tier verifies only the *provisioning
  logic*: `RunInstances` is called once per AZ group with the correct subnet,
  instance tagging/status polling behaves as expected, and terminating one
  simulated instance causes `aws_ec2_quorum_manager`'s existing
  quorum-maintenance loop to issue a replacement `RunInstances` call targeting
  the *same* AZ group. It does NOT and cannot verify actual Raft consensus,
  certificate issuance, or real AZ fault tolerance — there is no real compute
  behind a LocalStack instance for `ca_cluster_node` to run on.
- **`tests/ca_cluster_node_real_ec2_test.cpp`** (new, opt-in, real AWS): the
  tier that proves the end-to-end AZ-redundancy claim for real. Provisions 3
  real EC2 instances (one per configured AZ) via `aws_ec2_quorum_manager`,
  each running a real `ca_cluster_node` started via `user_data`; bootstraps
  the cluster (`--bootstrap-ca` on one instance); issues a certificate and
  confirms chain verification (Property 1/18-style); terminates one instance
  to simulate an AZ loss and confirms (a) the remaining 2 nodes retain quorum
  and keep serving `/v1/certificates` throughout, (b) `aws_ec2_quorum_manager`
  provisions a replacement instance in the same AZ/subnet, (c) the
  replacement catches up via Raft snapshot/log replication and rejoins as a
  full member holding the identical (decrypted) CA identity, and (d) a
  certificate issued before the AZ loss is still present in the ledger of
  every node afterward — Property 17, now demonstrated on real infrastructure
  rather than only an in-process multi-node test. Requires real AWS
  credentials, a built AMI running `ca_cluster_node`, and an env-var gate
  (e.g. `KYTHIRA_TEST_CA_CLUSTER_AMI`); tagged and excluded from the default
  `ctest` run, matching `aws_quorum_manager_real_ec2_test.cpp`'s convention.
  This test has real per-run AWS cost (3 EC2 instances plus one replacement)
  and should be run sparingly — a scheduled/manual job, not on every PR.
- **`tests/ca_bootstrap_client_test.cpp`** (new): starts two independent
  `ca_test_fixture`s (each with `start_network_service = true` and its own
  `--tls-cert`/`--tls-key`), computes each one's root fingerprint via
  `--print-root-fingerprint`, and asserts Property 20 — `fetch_trusted_root()`
  against fixture A's URL with fixture A's fingerprint succeeds and returns
  fixture A's root PEM; the same call with fixture B's fingerprint (a
  perfectly valid, unrelated CA) fails with a mismatch error rather than
  succeeding against the wrong server. Also asserts a truncated/malformed
  expected-fingerprint string is rejected as a usage error rather than
  silently failing the comparison.
- **`tests/acme_identifier_type_test.cpp`** (new, extends the ACME test
  coverage from task 30): verifies Property 21 by inspecting the
  `newOrder`/challenge-selection requests `acme_certificate_provider` sends
  for a mixed DNS+IP `csr_signing_options` against `acme_test_server`,
  confirming `identifier.type` and challenge selection per row of the
  dispatch table in the new Architecture section. Verifies Property 22 by
  running the `.local` validation path once with the test harness's mDNS
  resolver capability probe forced to report "unavailable" (via a test-only
  injection point, not by actually breaking the CI machine's resolver) and
  confirming the distinguishable error, versus once with it reporting
  "available" and a real `.local` hostname resolvable on the test network
  (skipped, not failed, in CI environments without mDNS infrastructure —
  following the same skip-don't-fail convention as the LocalStack tests).

## Dependencies

```
OpenSSL       ≥ 3.0   EVP_PKEY-based RSA/EC key generation, X509/X509_CRL
                      construction and signing, X509V3 extension helpers.
                      Already an optional project dependency
                      (find_package(OpenSSL QUIET); OpenSSL::SSL / OpenSSL::Crypto).

AWS SDK C++   —       acm-pca component (Aws::ACMPCA::ACMPCAClient), detected
(acm-pca)             independently via KYTHIRA_HAS_AWS_ACM_PCA. Optional —
                      only required for aws_acm_pca_provider and
                      `ca_service --serve --provider aws-acm-pca`. The core
                      AWS SDK dependency (KYTHIRA_HAS_AWS_SDK) is already
                      documented for aws_ec2_quorum_manager /
                      aws_asg_quorum_manager.
```

No new dependency is introduced for ACME support (Requirement 18.10):
JWS/JWK signing uses OpenSSL EVP calls already linked for
`certificate_authority`; request/response bodies use `boost::json` already
linked for `file_persistence.hpp`; `dns_01` reuses `rfc2136_ldns_discovery`'s
existing libldns-based UPDATE code; `http_01` reuses the same
`httplib::Server` already used by `ca_service`.

No new dependency is introduced for Requirement 20 either: `.local`
resolution uses the standard `getaddrinfo()` already linked into every
component that does hostname resolution — it works when the host has an
mDNS-capable NSS module (`nss-mdns`/Avahi, or macOS's native resolver)
installed, and this spec does not add or require a bundled mDNS client
library (Poco's DNSSD support, already an optional project dependency per
`poco_peer_discovery`, is for *service* discovery and is not used here).
