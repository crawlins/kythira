# HTTP Transport SSL/TLS Troubleshooting Guide

This guide helps diagnose and resolve common SSL/TLS configuration issues with the HTTP transport implementation.

## Table of Contents

1. [Common SSL Configuration Errors](#common-ssl-configuration-errors)
2. [Certificate Issues](#certificate-issues)
3. [OpenSSL Compatibility](#openssl-compatibility)
4. [Network and Connection Issues](#network-and-connection-issues)
5. [Debugging Tools and Techniques](#debugging-tools-and-techniques)
6. [Performance Considerations](#performance-considerations)

## Common SSL Configuration Errors

### Error: "SSL configuration error: OpenSSL support not available"

**Cause**: The application was built without OpenSSL support or OpenSSL libraries are not properly linked.

**Solutions**:
1. Verify OpenSSL is installed:
   ```bash
   # Ubuntu/Debian
   sudo apt-get install libssl-dev
   
   # CentOS/RHEL
   sudo yum install openssl-devel
   
   # macOS
   brew install openssl
   ```

2. Check CMake configuration:
   ```cmake
   find_package(OpenSSL REQUIRED)
   target_link_libraries(your_target PRIVATE OpenSSL::SSL OpenSSL::Crypto)
   ```

3. Verify OpenSSL version compatibility:
   ```bash
   openssl version
   ```

### Error: "Failed to load certificate file"

**Cause**: Certificate file path is incorrect, file doesn't exist, or insufficient permissions.

**Solutions**:
1. Verify file exists and is readable:
   ```bash
   ls -la /path/to/certificate.pem
   ```

2. Check file permissions:
   ```bash
   chmod 644 /path/to/certificate.pem
   chmod 600 /path/to/private_key.pem  # Private keys should be more restrictive
   ```

3. Validate certificate format:
   ```bash
   openssl x509 -in /path/to/certificate.pem -text -noout
   ```
### Error: "Private key does not match certificate"

**Cause**: The private key file doesn't correspond to the certificate file.

**Solutions**:
1. Verify key-certificate pair:
   ```bash
   # Compare certificate and key modulus (should be identical)
   openssl x509 -noout -modulus -in certificate.pem | openssl md5
   openssl rsa -noout -modulus -in private_key.pem | openssl md5
   ```

2. Check if key is encrypted:
   ```bash
   openssl rsa -in private_key.pem -check
   ```

3. Generate matching key pair if needed:
   ```bash
   # Generate new private key
   openssl genrsa -out private_key.pem 2048
   
   # Generate certificate signing request
   openssl req -new -key private_key.pem -out certificate.csr
   
   # Self-sign for testing (use proper CA for production)
   openssl x509 -req -days 365 -in certificate.csr -signkey private_key.pem -out certificate.pem
   ```

## Certificate Issues

### Certificate Expiration

**Check certificate validity**:
```bash
openssl x509 -in certificate.pem -dates -noout
```

**Monitor expiration**:
```bash
# Check if certificate expires within 30 days
openssl x509 -checkend 2592000 -noout -in certificate.pem
echo $?  # 0 = valid, 1 = expires soon
```

**Automated monitoring script**:
```bash
#!/bin/bash
CERT_FILE="/path/to/certificate.pem"
WARN_DAYS=30

EXP_DATE=$(openssl x509 -enddate -noout -in "$CERT_FILE" | cut -d= -f2)
EXP_EPOCH=$(date -d "$EXP_DATE" +%s)
CURR_EPOCH=$(date +%s)
DAYS_LEFT=$(( (EXP_EPOCH - CURR_EPOCH) / 86400 ))

if [ $DAYS_LEFT -lt $WARN_DAYS ]; then
    echo "WARNING: Certificate expires in $DAYS_LEFT days"
fi
```
### Certificate Chain Validation

**Verify certificate chain**:
```bash
# Verify against CA certificate
openssl verify -CAfile ca_certificate.pem certificate.pem

# Verify full chain
openssl verify -CAfile ca_certificate.pem -untrusted intermediate.pem certificate.pem
```

**Check certificate chain order**:
```bash
# Display certificate chain
openssl crl2pkcs7 -nocrl -certfile certificate_chain.pem | openssl pkcs7 -print_certs -text -noout
```

### Self-Signed Certificates for Testing

**Generate self-signed certificate**:
```bash
# Generate private key
openssl genrsa -out test_key.pem 2048

# Generate self-signed certificate
openssl req -new -x509 -key test_key.pem -out test_cert.pem -days 365 \
  -subj "/C=US/ST=Test/L=Test/O=Test/CN=localhost"

# Generate certificate with SAN for multiple hostnames
openssl req -new -x509 -key test_key.pem -out test_cert.pem -days 365 \
  -subj "/C=US/ST=Test/L=Test/O=Test/CN=localhost" \
  -extensions v3_req -config <(
echo '[req]'
echo 'distinguished_name = req'
echo '[v3_req]'
echo 'subjectAltName = @alt_names'
echo '[alt_names]'
echo 'DNS.1 = localhost'
echo 'DNS.2 = 127.0.0.1'
echo 'IP.1 = 127.0.0.1'
)
```

## OpenSSL Compatibility

### Version Compatibility Matrix

| OpenSSL Version | TLS 1.3 Support | Recommended | Notes |
|-----------------|-----------------|-------------|-------|
| 1.0.2 | No | No | End of life, security vulnerabilities |
| 1.1.0 | No | No | End of life |
| 1.1.1 | Yes | Yes | LTS version, widely supported |
| 3.0.x | Yes | Yes | Current stable, some API changes |
| 3.1.x+ | Yes | Yes | Latest features |
### API Compatibility Issues

**OpenSSL 3.0+ Deprecation Warnings**:
```cpp
// Old API (deprecated in OpenSSL 3.0)
int result = EVP_PKEY_cmp(key1, key2);

// New API (OpenSSL 3.0+)
int result = EVP_PKEY_eq(key1, key2);

// Compatibility macro
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
    int result = EVP_PKEY_eq(key1, key2);
#else
    int result = EVP_PKEY_cmp(key1, key2);
#endif
```

**ASN1_TIME Handling**:
```cpp
// OpenSSL 1.1.1+ has ASN1_TIME_to_tm
#if OPENSSL_VERSION_NUMBER >= 0x10101000L
    struct tm tm_time;
    if (ASN1_TIME_to_tm(asn1_time, &tm_time) == 1) {
        return mktime(&tm_time);
    }
#else
    // Manual parsing for older versions
    // ... implementation ...
#endif
```

### Building with Different OpenSSL Versions

**CMake configuration for specific OpenSSL version**:
```cmake
# Find specific OpenSSL version
find_package(OpenSSL 1.1.1 REQUIRED)

# Or use pkg-config for more control
find_package(PkgConfig REQUIRED)
pkg_check_modules(OPENSSL REQUIRED openssl>=1.1.1)

# Link libraries
target_link_libraries(your_target PRIVATE OpenSSL::SSL OpenSSL::Crypto)

# Add version-specific definitions
target_compile_definitions(your_target PRIVATE 
    OPENSSL_VERSION_NUMBER=${OPENSSL_VERSION_NUMBER}
)
```

## Network and Connection Issues

### SSL Handshake Failures

**Debug SSL handshake**:
```bash
# Test SSL connection with openssl s_client
openssl s_client -connect localhost:8443 -servername localhost -verify_return_error

# Test with specific TLS version
openssl s_client -connect localhost:8443 -tls1_2
openssl s_client -connect localhost:8443 -tls1_3

# Test with specific cipher
openssl s_client -connect localhost:8443 -cipher ECDHE-RSA-AES256-GCM-SHA384
```
**Common handshake errors**:

1. **"no shared cipher"**: Client and server have no common cipher suites
   - Solution: Review and align cipher suite configurations

2. **"certificate verify failed"**: Certificate validation failed
   - Solution: Check certificate chain, CA certificates, and hostname matching

3. **"protocol version"**: TLS version mismatch
   - Solution: Ensure compatible TLS version ranges

### Cipher Suite Issues

**List available cipher suites**:
```bash
# List all available ciphers
openssl ciphers -v

# List specific cipher suite
openssl ciphers -v 'ECDHE-RSA-AES256-GCM-SHA384'

# List TLS 1.3 cipher suites
openssl ciphers -v -tls1_3
```

**Recommended cipher suites**:
```cpp
// Strong cipher suites (prefer ECDHE for forward secrecy)
const char* strong_ciphers = 
    "ECDHE-ECDSA-AES256-GCM-SHA384:"
    "ECDHE-RSA-AES256-GCM-SHA384:"
    "ECDHE-ECDSA-AES128-GCM-SHA256:"
    "ECDHE-RSA-AES128-GCM-SHA256";

// TLS 1.3 cipher suites (automatically negotiated)
// TLS_AES_256_GCM_SHA384
// TLS_AES_128_GCM_SHA256
// TLS_CHACHA20_POLY1305_SHA256
```

### Mutual TLS (mTLS) Issues

**Client certificate not sent**:
1. Verify client certificate configuration
2. Check that server requests client certificate
3. Ensure client certificate is valid and not expired

**Client certificate rejected**:
1. Verify client certificate is signed by trusted CA
2. Check certificate purpose (client authentication)
3. Validate certificate chain

**Test mTLS configuration**:
```bash
# Test client certificate authentication
openssl s_client -connect localhost:8443 \
  -cert client_cert.pem \
  -key client_key.pem \
  -CAfile ca_cert.pem
```
## Debugging Tools and Techniques

### Logging Configuration

**Enable SSL debug logging** (if supported by cpp-httplib):
```cpp
// Enable verbose logging for SSL debugging
server.set_logger([](const httplib::Request& req, const httplib::Response& res) {
    std::cout << "SSL Request: " << req.method << " " << req.path << std::endl;
    std::cout << "SSL Version: " << req.get_header_value("SSL_PROTOCOL") << std::endl;
    std::cout << "Cipher: " << req.get_header_value("SSL_CIPHER") << std::endl;
});
```

### Network Analysis

**Capture SSL traffic with tcpdump/Wireshark**:
```bash
# Capture traffic on port 8443
sudo tcpdump -i any -w ssl_traffic.pcap port 8443

# Analyze with tshark
tshark -r ssl_traffic.pcap -Y "ssl.handshake.type == 1" -T fields -e frame.number -e ssl.handshake.ciphersuite
```

**SSL/TLS analysis with sslyze**:
```bash
# Install sslyze
pip install sslyze

# Analyze SSL configuration
sslyze --regular localhost:8443
```

### Application-Level Debugging

**Custom SSL verification callback**:
```cpp
// Example of custom certificate verification (pseudo-code)
auto verify_callback = [](bool preverified, X509_STORE_CTX* ctx) -> bool {
    X509* cert = X509_STORE_CTX_get_current_cert(ctx);
    int depth = X509_STORE_CTX_get_error_depth(ctx);
    int err = X509_STORE_CTX_get_error(ctx);
    
    // Log certificate details
    char subject[256];
    X509_NAME_oneline(X509_get_subject_name(cert), subject, sizeof(subject));
    
    std::cout << "Certificate verification:" << std::endl;
    std::cout << "  Subject: " << subject << std::endl;
    std::cout << "  Depth: " << depth << std::endl;
    std::cout << "  Preverified: " << preverified << std::endl;
    
    if (!preverified) {
        std::cout << "  Error: " << X509_verify_cert_error_string(err) << std::endl;
    }
    
    return preverified;
};
```
## Performance Considerations

### SSL Performance Optimization

**Session Resumption**:
- Enable SSL session caching to reduce handshake overhead
- Configure appropriate session timeout values
- Consider session tickets for stateless resumption

**Cipher Suite Performance**:
```cpp
// Performance-optimized cipher suites (AES-NI hardware acceleration)
const char* fast_ciphers = 
    "ECDHE-RSA-AES128-GCM-SHA256:"    // Fast with AES-NI
    "ECDHE-RSA-AES256-GCM-SHA384:"    // Strong but slower
    "ECDHE-RSA-CHACHA20-POLY1305";    // Good for mobile/ARM
```

**Connection Pooling**:
- Reuse SSL connections when possible
- Configure appropriate connection timeouts
- Monitor connection pool metrics

### Memory and CPU Usage

**Certificate Loading Optimization**:
```cpp
// Load certificates once at startup, not per connection
static X509* cached_cert = nullptr;
static EVP_PKEY* cached_key = nullptr;

// Initialize once
if (!cached_cert) {
    cached_cert = load_certificate(cert_path);
    cached_key = load_private_key(key_path);
}
```

**SSL Context Reuse**:
```cpp
// Create SSL context once and reuse
static SSL_CTX* ssl_ctx = nullptr;

if (!ssl_ctx) {
    ssl_ctx = SSL_CTX_new(TLS_method());
    // Configure context once
    SSL_CTX_use_certificate(ssl_ctx, cert);
    SSL_CTX_use_PrivateKey(ssl_ctx, key);
}
```

## Troubleshooting Checklist

### Pre-deployment Checklist

- [ ] OpenSSL version compatibility verified
- [ ] All certificate files exist and are readable
- [ ] Certificate and private key pairs match
- [ ] Certificate chain is complete and valid
- [ ] Certificate expiration dates are acceptable
- [ ] Cipher suites are properly configured
- [ ] TLS version constraints are appropriate
- [ ] CA certificates are properly configured
- [ ] File permissions are secure but accessible
### Runtime Troubleshooting Steps

1. **Verify basic connectivity**:
   ```bash
   telnet localhost 8443
   ```

2. **Test SSL handshake**:
   ```bash
   openssl s_client -connect localhost:8443
   ```

3. **Check certificate validity**:
   ```bash
   openssl x509 -in cert.pem -text -noout
   ```

4. **Verify cipher suite compatibility**:
   ```bash
   openssl s_client -connect localhost:8443 -cipher 'ECDHE-RSA-AES256-GCM-SHA384'
   ```

5. **Test with different TLS versions**:
   ```bash
   openssl s_client -connect localhost:8443 -tls1_2
   openssl s_client -connect localhost:8443 -tls1_3
   ```

6. **Check application logs** for SSL-specific error messages

7. **Monitor system resources** (CPU, memory) during SSL operations

### Common Error Messages and Solutions

| Error Message | Likely Cause | Solution |
|---------------|--------------|----------|
| "certificate verify failed" | Invalid certificate chain | Check CA certificates and certificate order |
| "no shared cipher" | Cipher suite mismatch | Align client/server cipher configurations |
| "protocol version" | TLS version incompatibility | Check min/max TLS version settings |
| "bad certificate" | Certificate format/corruption | Verify certificate file integrity |
| "unknown ca" | CA not trusted | Add CA certificate to trust store |
| "certificate expired" | Certificate past expiration | Renew certificate |
| "handshake failure" | General SSL negotiation failure | Check all SSL parameters systematically |

## Additional Resources

- [OpenSSL Documentation](https://www.openssl.org/docs/)
- [RFC 8446 - TLS 1.3](https://tools.ietf.org/html/rfc8446)
- [Mozilla SSL Configuration Generator](https://ssl-config.mozilla.org/)
- [SSL Labs Server Test](https://www.ssllabs.com/ssltest/)
- [OWASP Transport Layer Protection Cheat Sheet](https://cheatsheetseries.owasp.org/cheatsheets/Transport_Layer_Protection_Cheat_Sheet.html)

---

*This troubleshooting guide is part of the Kythira Raft HTTP Transport implementation. For implementation-specific issues, consult the source code and test cases in the project repository.*