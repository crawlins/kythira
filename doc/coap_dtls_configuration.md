# CoAP DTLS Configuration Guide

## Overview

This guide covers the configuration and setup of DTLS (Datagram Transport Layer Security) for secure CoAP communication in the Raft consensus system. DTLS provides encryption, authentication, and integrity protection for CoAP messages, making it suitable for production deployments where security is a concern.

## DTLS Fundamentals

### What is DTLS?

DTLS is the datagram variant of TLS (Transport Layer Security), designed to provide security for UDP-based protocols like CoAP. It provides:

- **Encryption**: Message confidentiality using symmetric encryption
- **Authentication**: Peer identity verification using certificates or pre-shared keys
- **Integrity**: Message tampering detection using cryptographic hashes
- **Replay Protection**: Prevention of message replay attacks

### CoAP + DTLS = CoAPS

When CoAP is used with DTLS, it's called CoAPS (CoAP Secure). The standard port for CoAPS is 5684, compared to 5683 for plain CoAP.

## Authentication Methods

### Certificate-Based Authentication

Certificate-based authentication uses X.509 certificates for peer identity verification. This is the most secure and scalable method for production deployments.

#### Advantages
- Strong security with PKI infrastructure
- Scalable to large deployments
- Industry standard approach
- Supports certificate revocation

#### Disadvantages
- Requires PKI infrastructure
- More complex setup
- Higher computational overhead

### Pre-Shared Key (PSK) Authentication

PSK authentication uses shared secret keys for peer authentication. This is simpler to set up but requires secure key distribution.

#### Advantages
- Simple configuration
- Lower computational overhead
- No PKI infrastructure required
- Suitable for closed networks

#### Disadvantages
- Key distribution challenges
- Less scalable
- No built-in key revocation
- Shared secrets must be protected

## Certificate-Based Configuration

### Generating Certificates

#### 1. Create Certificate Authority (CA)

```bash
# Generate CA private key
openssl genrsa -out ca-key.pem 4096

# Generate CA certificate
openssl req -new -x509 -days 3650 -key ca-key.pem -out ca-cert.pem \
    -subj "/C=US/ST=CA/L=San Francisco/O=MyOrg/CN=MyCA"
```

#### 2. Generate Node Certificates

For each Raft node, generate a certificate signed by the CA:

```bash
# Generate node private key
openssl genrsa -out node1-key.pem 2048

# Generate certificate signing request
openssl req -new -key node1-key.pem -out node1-csr.pem \
    -subj "/C=US/ST=CA/L=San Francisco/O=MyOrg/CN=node1"

# Sign certificate with CA
openssl x509 -req -days 365 -in node1-csr.pem -CA ca-cert.pem -CAkey ca-key.pem \
    -CAcreateserial -out node1-cert.pem

# Clean up CSR
rm node1-csr.pem
```

#### 3. Certificate with Subject Alternative Names (SAN)

For nodes accessible by multiple hostnames or IP addresses:

```bash
# Create config file for SAN
cat > node1-san.conf << EOF
[req]
distinguished_name = req_distinguished_name
req_extensions = v3_req

[req_distinguished_name]
CN = node1

[v3_req]
subjectAltName = @alt_names

[alt_names]
DNS.1 = node1
DNS.2 = node1.example.com
IP.1 = 192.168.1.10
IP.2 = 10.0.0.10
EOF

# Generate CSR with SAN
openssl req -new -key node1-key.pem -out node1-csr.pem -config node1-san.conf \
    -subj "/C=US/ST=CA/L=San Francisco/O=MyOrg/CN=node1"

# Sign certificate with SAN
openssl x509 -req -days 365 -in node1-csr.pem -CA ca-cert.pem -CAkey ca-key.pem \
    -CAcreateserial -out node1-cert.pem -extensions v3_req -extfile node1-san.conf
```

### Client Configuration

```cpp
#include <raft/coap_transport.hpp>

coap_client_config config;

// Enable DTLS
config.enable_dtls = true;

// Certificate-based authentication
config.cert_file = "/path/to/node1-cert.pem";
config.key_file = "/path/to/node1-key.pem";
config.ca_file = "/path/to/ca-cert.pem";

// Enable peer certificate verification
config.verify_peer_cert = true;

// Create endpoints with CoAPS URLs
std::unordered_map<std::uint64_t, std::string> endpoints = {
    {1, "coaps://node1.example.com:5684"},
    {2, "coaps://node2.example.com:5684"},
    {3, "coaps://node3.example.com:5684"}
};

auto client = coap_client<json_serializer>(endpoints, config, metrics);
```

### Server Configuration

```cpp
#include <raft/coap_transport.hpp>

coap_server_config config;

// Enable DTLS
config.enable_dtls = true;

// Certificate-based authentication
config.cert_file = "/path/to/node1-cert.pem";
config.key_file = "/path/to/node1-key.pem";
config.ca_file = "/path/to/ca-cert.pem";

// Enable peer certificate verification
config.verify_peer_cert = true;

auto server = coap_server<json_serializer>(
    "0.0.0.0",  // bind address
    5684,       // CoAPS port
    config,
    metrics
);

server.start();
```

## Pre-Shared Key (PSK) Configuration

### Generating PSK

```bash
# Generate random PSK (32 bytes = 256 bits)
openssl rand -hex 32 > psk.key

# Example output: a1b2c3d4e5f6789012345678901234567890abcdef1234567890abcdef123456
```

### Client Configuration

```cpp
#include <raft/coap_transport.hpp>

coap_client_config config;

// Enable DTLS
config.enable_dtls = true;

// PSK-based authentication
config.psk_identity = "raft-cluster-1";

// Convert hex string to bytes
std::string psk_hex = "a1b2c3d4e5f6789012345678901234567890abcdef1234567890abcdef123456";
config.psk_key.reserve(psk_hex.length() / 2);
for (size_t i = 0; i < psk_hex.length(); i += 2) {
    std::string byte_str = psk_hex.substr(i, 2);
    auto byte_val = static_cast<std::byte>(std::stoul(byte_str, nullptr, 16));
    config.psk_key.push_back(byte_val);
}

// Disable certificate verification for PSK
config.verify_peer_cert = false;

std::unordered_map<std::uint64_t, std::string> endpoints = {
    {1, "coaps://192.168.1.10:5684"},
    {2, "coaps://192.168.1.11:5684"},
    {3, "coaps://192.168.1.12:5684"}
};

auto client = coap_client<json_serializer>(endpoints, config, metrics);
```

### Server Configuration

```cpp
#include <raft/coap_transport.hpp>

coap_server_config config;

// Enable DTLS
config.enable_dtls = true;

// PSK-based authentication (same as client)
config.psk_identity = "raft-cluster-1";
config.psk_key = /* same PSK key as client */;

// Disable certificate verification for PSK
config.verify_peer_cert = false;

auto server = coap_server<json_serializer>(
    "0.0.0.0",
    5684,
    config,
    metrics
);

server.start();
```

## Advanced DTLS Configuration

### Cipher Suite Selection

```cpp
// Example: Restrict to specific cipher suites (implementation-dependent)
coap_client_config config;
config.enable_dtls = true;
// Note: Cipher suite configuration depends on libcoap build options
// Consult libcoap documentation for available cipher suites
```

### Session Resumption

DTLS session resumption reduces handshake overhead for repeated connections:

```cpp
coap_client_config config;
config.enable_dtls = true;
config.session_timeout = std::chrono::seconds{3600};  // 1 hour session lifetime
```

### Certificate Revocation

For production deployments, consider certificate revocation checking:

```cpp
coap_client_config config;
config.enable_dtls = true;
config.cert_file = "/path/to/cert.pem";
config.key_file = "/path/to/key.pem";
config.ca_file = "/path/to/ca-cert.pem";
config.verify_peer_cert = true;
// Note: CRL/OCSP support depends on libcoap build configuration
```

## Security Best Practices

### Certificate Management

1. **Use Strong Key Sizes**
   - RSA: Minimum 2048 bits, recommended 4096 bits
   - ECDSA: Minimum 256 bits (P-256), recommended 384 bits (P-384)

2. **Certificate Validity Periods**
   - CA certificates: 10-20 years
   - Node certificates: 1-2 years
   - Implement automated certificate renewal

3. **Secure Key Storage**
   - Protect private keys with appropriate file permissions (600)
   - Consider hardware security modules (HSMs) for CA keys
   - Use encrypted key files where possible

4. **Certificate Validation**
   - Always enable peer certificate verification in production
   - Validate certificate chains properly
   - Check certificate expiration dates

### PSK Management

1. **Key Generation**
   - Use cryptographically secure random number generators
   - Minimum 128-bit keys, recommended 256-bit keys
   - Generate unique keys per cluster/deployment

2. **Key Distribution**
   - Use secure channels for key distribution
   - Avoid transmitting keys over unencrypted channels
   - Consider key derivation from master secrets

3. **Key Rotation**
   - Implement regular key rotation schedules
   - Support graceful key transitions
   - Maintain key version tracking

### Network Security

1. **Firewall Configuration**
   - Restrict CoAPS traffic to known peers
   - Block plain CoAP (port 5683) in production
   - Use network segmentation where possible

2. **Monitoring**
   - Log DTLS handshake failures
   - Monitor certificate expiration dates
   - Alert on security-related events

## Troubleshooting

### Common DTLS Issues

#### Handshake Failures

**Symptoms:**
- `coap_security_error` exceptions
- Connection timeouts
- "DTLS handshake failed" log messages

**Causes and Solutions:**

1. **Certificate Issues**
   ```bash
   # Verify certificate validity
   openssl x509 -in cert.pem -text -noout
   
   # Check certificate chain
   openssl verify -CAfile ca-cert.pem cert.pem
   
   # Verify certificate dates
   openssl x509 -in cert.pem -dates -noout
   ```

2. **Clock Synchronization**
   - Ensure system clocks are synchronized (use NTP)
   - Certificate validity depends on accurate time

3. **Hostname/IP Mismatch**
   - Verify certificate Subject Alternative Names
   - Use IP addresses in certificates if connecting by IP

#### PSK Authentication Failures

**Symptoms:**
- Authentication errors during handshake
- "PSK identity not found" errors

**Solutions:**

1. **Verify PSK Configuration**
   ```cpp
   // Ensure identical PSK identity and key on all nodes
   config.psk_identity = "exact-same-identity";
   config.psk_key = /* identical key bytes */;
   ```

2. **Check Key Format**
   ```cpp
   // Verify hex-to-bytes conversion
   std::string hex = "deadbeef";
   std::vector<std::byte> key;
   for (size_t i = 0; i < hex.length(); i += 2) {
       auto byte_val = static_cast<std::byte>(
           std::stoul(hex.substr(i, 2), nullptr, 16));
       key.push_back(byte_val);
   }
   ```

#### Performance Issues

**Symptoms:**
- Slow connection establishment
- High CPU usage during handshakes
- Timeout errors under load

**Solutions:**

1. **Session Resumption**
   ```cpp
   config.session_timeout = std::chrono::seconds{3600};
   ```

2. **Connection Pooling**
   ```cpp
   config.max_sessions = 50;  // Adjust based on load
   ```

3. **Hardware Acceleration**
   - Use OpenSSL with hardware crypto acceleration
   - Consider dedicated crypto hardware for high-throughput scenarios

### Debugging Tools

#### OpenSSL s_client

Test DTLS connectivity:

```bash
# Test DTLS connection (requires OpenSSL 1.1.1+)
openssl s_client -dtls1_2 -connect node1:5684 -cert client-cert.pem -key client-key.pem -CAfile ca-cert.pem
```

#### Wireshark Analysis

Capture and analyze DTLS traffic:

1. Capture on CoAPS port (5684)
2. Use "dtls" filter to show DTLS handshake
3. Look for handshake completion and encrypted application data

#### libcoap Debugging

Enable libcoap debug logging:

```cpp
// Enable maximum libcoap logging (implementation-dependent)
coap_set_log_level(LOG_DEBUG);
```

### Certificate Validation Errors

#### Self-Signed Certificates

For testing with self-signed certificates:

```cpp
coap_client_config config;
config.enable_dtls = true;
config.cert_file = "/path/to/self-signed-cert.pem";
config.key_file = "/path/to/private-key.pem";
config.verify_peer_cert = false;  // Disable for self-signed testing
```

**Warning:** Never disable certificate verification in production!

#### Certificate Chain Issues

Verify complete certificate chain:

```bash
# Check if intermediate certificates are needed
openssl s_client -connect node1:5684 -showcerts

# Create certificate bundle if needed
cat node-cert.pem intermediate-cert.pem > cert-bundle.pem
```

## Production Deployment Checklist

### Security Configuration

- [ ] DTLS enabled on all nodes
- [ ] Strong cipher suites configured
- [ ] Certificate validation enabled
- [ ] Certificates from trusted CA
- [ ] Private keys properly protected (file permissions)
- [ ] Certificate expiration monitoring in place
- [ ] Key rotation procedures documented

### Network Configuration

- [ ] Firewall rules restrict access to CoAPS ports
- [ ] Plain CoAP (port 5683) blocked in production
- [ ] Network segmentation implemented
- [ ] Intrusion detection monitoring DTLS traffic

### Operational Procedures

- [ ] Certificate renewal automation
- [ ] Security incident response procedures
- [ ] Regular security audits scheduled
- [ ] Backup and recovery procedures for certificates/keys
- [ ] Monitoring and alerting for security events

### Testing

- [ ] DTLS handshake testing in staging environment
- [ ] Certificate validation testing
- [ ] Performance testing with DTLS enabled
- [ ] Failover testing with certificate issues
- [ ] Security penetration testing completed

## Example Configurations

### Development Environment

```cpp
// Simple PSK configuration for development
coap_client_config dev_config;
dev_config.enable_dtls = true;
dev_config.psk_identity = "dev-cluster";
dev_config.psk_key = {std::byte{0xde}, std::byte{0xad}, std::byte{0xbe}, std::byte{0xef}};
dev_config.verify_peer_cert = false;
```

### Production Environment

```cpp
// Certificate-based configuration for production
coap_client_config prod_config;
prod_config.enable_dtls = true;
prod_config.cert_file = "/etc/raft/certs/node.crt";
prod_config.key_file = "/etc/raft/private/node.key";
prod_config.ca_file = "/etc/raft/certs/ca.crt";
prod_config.verify_peer_cert = true;
prod_config.session_timeout = std::chrono::seconds{1800};  // 30 minutes
```

### High-Security Environment

```cpp
// Restrictive configuration for high-security deployments
coap_client_config secure_config;
secure_config.enable_dtls = true;
secure_config.cert_file = "/etc/raft/certs/node.crt";
secure_config.key_file = "/etc/raft/private/node.key";
secure_config.ca_file = "/etc/raft/certs/ca.crt";
secure_config.verify_peer_cert = true;
secure_config.session_timeout = std::chrono::seconds{300};  // 5 minutes (short sessions)
// Additional security measures would be configured at the libcoap level
```

This guide provides comprehensive coverage of DTLS configuration for CoAP transport in Raft deployments, from basic setup to production-ready security configurations.