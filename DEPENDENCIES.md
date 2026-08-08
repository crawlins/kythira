# Dependencies

This document lists the dependencies required to build and use the network simulator library.

## Required Dependencies

### C++ Compiler
- **GCC 13+**, **Clang 16+**, or **MSVC 2022+**
- Must support C++23 standard
- Concepts support required

### Build System
- **CMake 3.20 or higher**

### Libraries

#### folly (Facebook Open-source Library)
- **Status**: Required for full implementation
- **Purpose**: Provides Future/Promise implementation and executor framework
- **Installation**:
  ```bash
  # Ubuntu/Debian
  sudo apt-get install libfolly-dev

  # macOS (Homebrew)
  brew install folly

  # From source
  git clone https://github.com/facebook/folly.git
  cd folly
  mkdir build && cd build
  cmake ..
  make
  sudo make install
  ```

#### Boost
- **Status**: Required
- **Components**: system, thread, unit_test_framework
- **Minimum Version**: 1.70+
- **Installation**:
  ```bash
  # Ubuntu/Debian
  sudo apt-get install libboost-all-dev

  # macOS (Homebrew)
  brew install boost
  ```

## Optional Dependencies

### libfiu (Fault Injection Userspace) — test-only
- **Status**: Optional, test-only — chaos test targets only compiled when detected
- **Purpose**: Provides `fiu_do_on()` / `fiu_fail()` fault injection API used by chaos tests
- **Minimum Version**: 0.6
- **Installation**:
  ```bash
  # Ubuntu/Debian
  sudo apt install libfiu-dev

  # Verify
  pkg-config --modversion libfiu
  ls /usr/include/fiu.h /usr/include/fiu-local.h /usr/include/fiu-control.h
  ```
- **Notes**: When libfiu is absent, the build is fully clean; chaos test targets are simply
  not compiled. The production library and all other tests are never affected.

### OpenSSL ≥ 3.0 — certificate_authority (CA testing/provisioning framework)
- **Status**: Optional — already a project dependency (HTTPS/TLS support); this component
  reuses the same `find_package(OpenSSL QUIET)` detection
- **Purpose**: `EVP_PKEY` key generation, `X509`/`X509_CRL` construction and signing, and
  `X509V3` extension handling for `include/raft/certificate_authority.hpp`, `ca_service`,
  and `ca_cluster_node`
- **Notes**: When OpenSSL is not detected, the `certificate_authority` target, `ca_service`,
  `ca_cluster_node`, and any test target depending on them are simply not defined; the rest
  of the build is unaffected (`KYTHIRA_HAS_OPENSSL` mirrors the existing `KYTHIRA_HAS_LDNS` /
  `KYTHIRA_HAS_POCO_DNSSD` optional-dependency pattern).

### gRPC + Protocol Buffers — gRPC transport
- **Status**: Optional — `raft_grpc_transport` target only compiled when detected
  (and Folly is available); mirrors the libcoap graceful-degradation pattern
- **Purpose**: HTTP/2 + Protocol Buffers implementation of the
  `network_client`/`network_server` concept family
  (`include/raft/grpc_transport.hpp`, `grpc_transport_impl.hpp`). `protoc` +
  `grpc_cpp_plugin` generate `raft.pb.{h,cc}`/`raft.grpc.pb.{h,cc}` from
  `proto/raft.proto` at build time (generated code is not checked in)
- **Minimum Version**: gRPC ≥ 1.42 provides the stable (non-experimental)
  callback API this transport uses; the version is left unpinned in `vcpkg.json`
  so the project's pinned `builtin-baseline` supplies it (that baseline's gRPC
  is well past 1.42), avoiding an unsatisfiable `version>=` constraint against
  the baseline
- **Installation**: `grpc` vcpkg port (declared in `vcpkg.json`); pulls in
  Protobuf transitively
- **Notes**: `find_package(gRPC CONFIG)` / `find_package(Protobuf CONFIG)` back
  `GRPC_TRANSPORT_FOUND` and the `GRPC_TRANSPORT` Kconfig symbol. When gRPC/
  Protobuf are absent the `raft_grpc_transport` target and its tests/example are
  simply not defined; the rest of the build is unaffected. Under
  `-DKYTHIRA_KCONFIG_STRICT=ON`, `CONFIG_GRPC_TRANSPORT=y` with gRPC missing is a
  hard configure error. See `.kiro/specs/grpc-transport/` and
  `doc/grpc_transport_README.md`.

### libnyoci (MIT) — alternate CoAP transport backend
- **Status**: Optional and **opt-in** — not part of a default install. Enabled by
  the `coap-libnyoci` vcpkg feature and the `COAP_TRANSPORT_LIBNYOCI` Kconfig
  symbol (default `n`). Additive rather than a replacement: it does *not* depend
  on `COAP_TRANSPORT`, and both CoAP backends can be built into one tree
- **Purpose**: a second implementation of the `network_client`/`network_server`
  concept family speaking CoAP —
  `coap_libnyoci_client`/`coap_libnyoci_server`
  (`include/raft/coap_transport_libnyoci_impl.hpp`) — over
  [libnyoci][libnyoci] instead of libcoap. libnyoci is a full RFC 7252 stack, so
  it owns the socket, the message/token layer, retransmission of confirmable
  messages, duplicate suppression and Block2 transfer; the adapter only bridges
  its C callbacks into `future_template<...>` promises
- **Version**: pinned to commit `11a6e1b` (master, 2019-04-15), **not** the
  `0.07.00rc1` tag — that tag is the 2017 initial release and predates both the
  fuzzing/robustness fixes and the `nyoci_inbound_get_path()` signature the
  adapter calls
- **Installation**: built from source by the `vcpkg-overlays/libnyoci` overlay
  port. Because libnyoci is **autotools**, the port uses `vcpkg_configure_make`
  + `vcpkg_install_make` + `vcpkg_fixup_pkgconfig`, and the host needs the
  autotools chain *plus* `autoconf-archive` (`configure.ac` calls `AX_PTHREAD`):
  ```bash
  sudo apt install autoconf automake libtool autoconf-archive pkg-config
  cmake -S . -B build -DVCPKG_MANIFEST_FEATURES=coap-libnyoci
  ```
- **Notes**: discovery is `pkg_check_modules(LIBNYOCI QUIET libnyoci)` — libnyoci
  ships no CMake config package — and backs `LIBNYOCI_AVAILABLE`. When absent,
  the adapter header still compiles (keeping its full concept surface, so the
  conformance test's `static_assert`s stay meaningful) and its integration tests
  skip rather than fail. Under `-DKYTHIRA_KCONFIG_STRICT=ON`,
  `CONFIG_COAP_TRANSPORT_LIBNYOCI=y` with libnyoci missing is a hard configure
  error.

  **Security**: the port enables libnyoci's OpenSSL DTLS plugin
  (`--enable-tls`), which backs `dtls_psk` and `dtls_pki`. kythira's own
  `coap_security_provider` is not reusable here — it is expressed entirely in
  libcoap types — so DTLS *configuration* forks per backend, though the config
  surface (`coap_client_config`, `translate_legacy_fields()`) is shared.
  `oscore` and `dtls_rpk` are **refused at construction** rather than silently
  downgraded: libnyoci ships no OSCORE and kythira has no implementation of its
  own to lend, and raw public keys need certificate-type extensions that arrived
  only in OpenSSL 3.2. Note the plugin is upstream-"experimental" and calls
  OpenSSL 1.x-era APIs that are deprecated-but-present in 3.x.

  **DTLS-PKI needs small certificates.** libnyoci reads every inbound datagram
  into a fixed `char packet[NYOCI_MAX_PACKET_LENGTH+1]` — 1033 bytes by default
  — and that applies to DTLS handshake records too. An RSA-2048 certificate
  flight overruns it and is silently truncated, so the handshake stalls and the
  request times out with no diagnostic. ECDSA P-256 fits comfortably; use it, or
  build libnyoci with a larger `NYOCI_MAX_CONTENT_LENGTH`. PSK is unaffected.

  **Block-wise**: libnyoci implements **Block2 but no Block1**, so an over-large
  *request* is rejected with a descriptive error instead of being split — this
  bounds InstallSnapshot. See `.kiro/specs/coap-transport-libnyoci/`,
  `doc/coap_library_alternatives.md`, and `doc/TODO.md` for the OSCORE
  follow-up.

  A translation unit must include **either** `raft/coap_transport.hpp` **or**
  `raft/coap_transport_libnyoci_impl.hpp`, never both: libcoap defines the CoAP
  option numbers as macros and libnyoci as enumerators, so the two C headers
  cannot coexist. They do not conflict at link time.

### AWS SDK ACM Private CA component — aws_acm_pca_provider
- **Status**: Optional — independent of the core `KYTHIRA_HAS_AWS_SDK` component set
  already used by `aws_ec2_quorum_manager`/`aws_asg_quorum_manager`
- **Purpose**: `Aws::ACMPCA::ACMPCAClient` calls (`IssueCertificate`, `GetCertificate`,
  `GetCertificateAuthorityCertificate`, `RevokeCertificate`) backing
  `aws_acm_pca_provider`, one of two `certificate_provider` implementations
- **Notes**: `find_package(AWSSDK QUIET COMPONENTS acm-pca)` defines
  `KYTHIRA_HAS_AWS_ACM_PCA`. Environments with the core AWS SDK but without this
  component still build everything except `aws_acm_pca_provider`.

### google-cloud-cpp ≥ 2.20 (compute component) — GCP Compute Engine and Managed Instance Group quorum managers
- **Status**: Optional — the GCP analogue of the core AWS SDK component set; the
  two quorum managers are only compiled when detected
- **Purpose**: `google::cloud::compute_instances_v1::InstancesClient`,
  `compute_instance_group_managers_v1::InstanceGroupManagersClient`, and
  `compute_zone_operations_v1::ZoneOperationsClient` calls
  (`instances.insert`/`.list`/`.get`/`.delete`/`.setLabels`,
  `instanceGroupManagers.get`/`.resize`/`.listManagedInstances`/
  `.deleteInstances`, `zoneOperations.get`) backing `gcp_compute_quorum_manager`
  and `gcp_mig_quorum_manager`
- **Minimum Version**: google-cloud-cpp ≥ 2.20 (CI builds 2.37.0)
- **Installation**: `google-cloud-cpp` vcpkg port with the `compute` and
  `privateca` features, declared in `vcpkg.json` under the opt-in `gcp`
  manifest feature (`--x-feature=gcp`) rather than the default dependency
  set. The `compute` component builds the entire Compute API surface (large),
  so keeping it behind a feature avoids forcing that build onto every CI
  matrix leg; the dedicated `gcp-sdk-build` CI job enables the feature and
  compiles the SDK-gated code against it. Operators wanting the GCP backends
  install with `--x-feature=gcp` (or add it to their own default set).
- **Notes**: `find_package(google_cloud_cpp_compute QUIET COMPONENTS
  compute_instances compute_instance_group_managers compute_zone_operations)`
  defines `KYTHIRA_HAS_GCP_SDK` when all three components are present. Backs the
  `GCP_SDK` Kconfig symbol. When absent, both GCP quorum managers are simply not
  defined; the rest of the build is unaffected.

### google-cloud-cpp (privateca component) — GCP Certificate Authority Service certificate provider
- **Status**: Optional — independent of `KYTHIRA_HAS_GCP_SDK` (mirrors how
  `KYTHIRA_HAS_AWS_ACM_PCA` is independent of `KYTHIRA_HAS_AWS_SDK`)
- **Purpose**: `google::cloud::privateca_v1::CertificateAuthorityServiceClient`
  calls (`CreateCertificate`, `GetCertificateAuthority`,
  `ListCertificateAuthorities`, `RevokeCertificate`) backing
  `gcp_privateca_certificate_provider`, the GCP analogue of `aws_acm_pca_provider`
- **Installation**: `google-cloud-cpp` vcpkg port with the `privateca` feature,
  declared under the opt-in `gcp` manifest feature (see the compute entry above)
- **Notes**: `find_package(google_cloud_cpp_privateca QUIET)` defines
  `KYTHIRA_HAS_GCP_PRIVATECA`. Backs the `GCP_PRIVATECA` Kconfig symbol.
  Environments with the compute components but without `privateca` still build
  everything except `gcp_privateca_certificate_provider`, and vice versa.
### Azure SDK for C++ (azure-core-cpp, azure-identity-cpp) — azure_vm_quorum_manager, azure_vmss_quorum_manager
- **Status**: Optional — `find_package(azure-core-cpp CONFIG)` +
  `find_package(azure-identity-cpp CONFIG)`
- **Purpose**: `Azure::Core::Http::HttpPipeline`-based ARM REST calls (there is no
  generated ARM Compute/Network management-plane client in the C++ SDK ecosystem)
  backing `azure_vm_quorum_manager` and `azure_vmss_quorum_manager`, plus
  `Azure::Identity::{EnvironmentCredential,ManagedIdentityCredential,AzureCliCredential,
  ChainedTokenCredential}` for authentication
- **Notes**: Defines `KYTHIRA_HAS_AZURE_SDK`. Independent of the AWS SDK entries
  above — an environment can have either, both, or neither.

### Azure Key Vault Keys SDK (azure-security-keyvault-keys-cpp) — azure_key_vault_ca_provider
- **Status**: Optional — independent of the core `KYTHIRA_HAS_AZURE_SDK` component
  set above, mirroring `AWS SDK ACM Private CA component`'s independence from
  `KYTHIRA_HAS_AWS_SDK`
- **Purpose**: `Azure::Security::KeyVault::Keys::KeyClient::Sign()` backing
  `azure_key_vault_ca_provider`, one of three `certificate_provider` implementations
- **Notes**: `find_package(azure-security-keyvault-keys-cpp CONFIG)` defines
  `KYTHIRA_HAS_AZURE_KEY_VAULT`. Environments with the core Azure SDK but without
  this component still build everything except `azure_key_vault_ca_provider`.

### libssh2 — real-EC2 `ca_cluster_node` deployment test
- **Status**: Optional, test-only — `ca_cluster_node_real_ec2_test` only compiled when
  detected (and only actually run against real AWS when `KYTHIRA_AWS_REAL_EC2_TESTS`
  is set at runtime — always compiled but runtime-skipped otherwise)
- **Purpose**: SSHes into freshly-launched EC2 instances to start `ca_cluster_node`
  once all three peers' addresses are known (the node binary itself never needs SSH)
- **Notes**: `find_package(libssh2 QUIET)`, falling back to a pkg-config check, defines
  `LIBSSH2_FOUND`. Without it, `ca_cluster_node_real_ec2_test` is simply not compiled;
  everything else is unaffected.

### ccache — faster rebuilds
- **Status**: Optional — auto-detected via `find_program(ccache)`; absent, the build is
  identical to today (`-DKYTHIRA_ENABLE_CCACHE=OFF` to force off even when installed)
- **Purpose**: Skips recompiling a translation unit whose preprocessed content and
  compiler flags exactly match a prior compile. Measured on this project's own CI: a
  from-scratch rebuild where nothing changed since the last build dropped from 29m07s to
  11m59s (~59% reduction) — the remaining time is link time, which ccache cannot cache;
  a build that touches a widely-included header will see less benefit than this best case.
- **Installation**:
  ```bash
  # Ubuntu/Debian
  sudo apt install ccache

  # macOS (Homebrew)
  brew install ccache
  ```
- **Notes**: No `--max-size` configuration is required for local use — ccache's own
  5 GB default applies. Its own *default cache directory* is `~/.cache/ccache`
  (ccache ≥4.0 follows the XDG Base Directory spec; older `~/.ccache`-based
  documentation you may find online is for ccache 3.x) — CI explicitly sets
  `CCACHE_DIR=~/.ccache` to keep its `actions/cache` step's tracked path simple
  and predictable, but a local install needs no such override and can just use
  its real default. CI also uses smaller, explicit `--max-size` values scoped to
  each job's own disk budget (`.kiro/specs/ccache-adoption/`); that sizing is
  CI-only and not relevant to local use.

### kconfiglib — build configuration tooling (Kconfig)
- **Status**: Optional Python *tooling*, not a C++ library — distinct from every
  dependency above. Only needed to run `menuconfig`/`guiconfig`/`savedefconfig`
  or to regenerate the Kconfig-derived autoconf artifacts from a changed
  `.config`; a machine without it configures and builds identically (every
  `find_package()` call falls back to unconditional probing, equivalent to
  every optional symbol defaulting to `y`)
- **Purpose**: Pure-Python parser/menu engine for the root [`Kconfig`](Kconfig)
  file — the same configuration language used by the Linux kernel, Zephyr,
  Buildroot, and coreboot — layered over the `find_package()`-driven optional-
  dependency matrix documented throughout this file. See the "Build
  Configuration (Kconfig)" section of [README.md](README.md) and
  [`.kiro/specs/kconfig-integration/`](.kiro/specs/kconfig-integration/) for
  the full design.
- **Installation**:
  ```bash
  pip install -r scripts/kconfig/requirements.txt
  ```

### Property-Based Testing Library
- **RapidCheck** or similar C++ property-based testing framework
- Required for property-based tests (tasks 4.5+)
- Installation instructions will be added when implementing property tests

## Current Build Status

This section predates the project's move to vcpkg manifest mode for
dependency management; see [README.md](README.md)'s Requirements/Building
sections for the current, actively-maintained build instructions. For
folly specifically, see "Building Without folly" below.

## Verifying Dependencies

To check if dependencies are installed:

```bash
# Check compiler version
g++ --version

# Check CMake version
cmake --version

# Check Boost
dpkg -l | grep libboost  # Ubuntu/Debian
brew list boost          # macOS

# Check folly
pkg-config --exists folly && echo "folly found" || echo "folly not found"
```

## Building Without folly

As of July 2026 (see `doc/TODO.md`'s "Known Follow-ups" for the two-part
Folly-decoupling work), `certificate_authority` and `tests/` build and pass
without folly installed, provided an alternate `kythira::Future` backend is
selected instead (`CONFIG_STDEXEC_BACKEND=y` or `CONFIG_BOOST_FUTURE_BACKEND=y`
in Kconfig, or `-DKYTHIRA_DEFAULT_FUTURE_BACKEND=stdexec`/`boost` without
Kconfig). Tests that are genuinely Folly-specific by design (HTTP/CoAP
transport tests, Folly concept-wrapper tests, cross-backend benchmarks) are
individually skipped rather than failing the build. `examples/` and the
standalone `cmd/*` production executables (`ca_cluster_node`, `chaos_node`,
the discovery-node binaries) remain folly-only, since their own `main()`s
call `folly::init()` directly. Without folly *and* without selecting an
alternate backend, the build system issues a warning and skips the
Folly-dependent targets, same as always.

## Next Steps

1. Install folly library for async operations support
2. Install property-based testing framework for comprehensive testing
3. Verify all dependencies are correctly linked

[libnyoci]: https://github.com/darconeous/libnyoci
