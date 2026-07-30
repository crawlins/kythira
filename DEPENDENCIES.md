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

### AWS SDK ACM Private CA component — aws_acm_pca_provider
- **Status**: Optional — independent of the core `KYTHIRA_HAS_AWS_SDK` component set
  already used by `aws_ec2_quorum_manager`/`aws_asg_quorum_manager`
- **Purpose**: `Aws::ACMPCA::ACMPCAClient` calls (`IssueCertificate`, `GetCertificate`,
  `GetCertificateAuthorityCertificate`, `RevokeCertificate`) backing
  `aws_acm_pca_provider`, one of two `certificate_provider` implementations
- **Notes**: `find_package(AWSSDK QUIET COMPONENTS acm-pca)` defines
  `KYTHIRA_HAS_AWS_ACM_PCA`. Environments with the core AWS SDK but without this
  component still build everything except `aws_acm_pca_provider`.

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
