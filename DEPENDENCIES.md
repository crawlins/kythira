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

### Property-Based Testing Library
- **RapidCheck** or similar C++ property-based testing framework
- Required for property-based tests (tasks 4.5+)
- Installation instructions will be added when implementing property tests

## Current Build Status

The project currently builds with:
- ✅ CMake 3.28.3
- ✅ GCC 13.3.0
- ✅ Boost 1.84.0
- ⚠️  folly (not yet installed, but build system is configured)

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

The current implementation can be built without folly installed. The build system will issue a warning but will continue. However, the full implementation of async operations will require folly to be installed.

## Next Steps

1. Install folly library for async operations support
2. Install property-based testing framework for comprehensive testing
3. Verify all dependencies are correctly linked
