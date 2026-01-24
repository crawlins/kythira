#pragma once

#include <raft/http_transport.hpp>
#include <httplib.h>
#include <format>
#include <stdexcept>
#include <thread>
#include <fstream>
#include <filesystem>

// Conditional includes based on folly availability
#ifdef FOLLY_AVAILABLE
#include <folly/Future.h>
#else
#include <network_simulator/types.hpp>
#include <future>
#endif

// OpenSSL includes for certificate validation (when available)
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <openssl/pem.h>
#include <openssl/err.h>
#endif

namespace kythira {

namespace {
    constexpr const char* endpoint_request_vote = "/v1/raft/request_vote";
    constexpr const char* endpoint_append_entries = "/v1/raft/append_entries";
    constexpr const char* endpoint_install_snapshot = "/v1/raft/install_snapshot";
    constexpr const char* content_type_json = "application/json";
    constexpr const char* header_content_type = "Content-Type";
    constexpr const char* header_content_length = "Content-Length";
    constexpr const char* header_user_agent = "User-Agent";
    
    // Forward declaration for ASN1_TIME helper function
    auto ASN1_TIME_to_time_t(ASN1_TIME* asn1_time) -> time_t;
    
    // SSL certificate validation helpers
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
    auto validate_certificate_file(const std::string& cert_path) -> void {
        if (cert_path.empty()) {
            return; // Empty path is valid (optional certificate)
        }
        
        if (!std::filesystem::exists(cert_path)) {
            throw kythira::ssl_configuration_error(
                std::format("Certificate file does not exist: {}", cert_path));
        }
        
        // Try to load and validate the certificate
        std::ifstream cert_file(cert_path, std::ios::binary);
        if (!cert_file.is_open()) {
            throw kythira::ssl_configuration_error(
                std::format("Cannot open certificate file: {}", cert_path));
        }
        
        // Read certificate content
        std::string cert_content((std::istreambuf_iterator<char>(cert_file)),
                                std::istreambuf_iterator<char>());
        cert_file.close();
        
        // Create BIO from certificate content
        BIO* bio = BIO_new_mem_buf(cert_content.c_str(), static_cast<int>(cert_content.length()));
        if (!bio) {
            throw kythira::ssl_configuration_error(
                std::format("Failed to create BIO for certificate: {}", cert_path));
        }
        
        // Try to load as PEM first
        X509* cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
        if (!cert) {
            // Reset BIO and try DER format
            BIO_reset(bio);
            cert = d2i_X509_bio(bio, nullptr);
        }
        
        BIO_free(bio);
        
        if (!cert) {
            unsigned long err = ERR_get_error();
            char err_buf[256];
            ERR_error_string_n(err, err_buf, sizeof(err_buf));
            throw kythira::certificate_validation_error(
                std::format("Invalid certificate format in {}: {}", cert_path, err_buf));
        }
        
        // Check certificate validity period
        ASN1_TIME* not_before = X509_get_notBefore(cert);
        ASN1_TIME* not_after = X509_get_notAfter(cert);
        
        if (X509_cmp_current_time(not_before) > 0) {
            X509_free(cert);
            throw kythira::certificate_validation_error(
                std::format("Certificate not yet valid: {}", cert_path));
        }
        
        if (X509_cmp_current_time(not_after) < 0) {
            X509_free(cert);
            throw kythira::certificate_validation_error(
                std::format("Certificate has expired: {}", cert_path));
        }
        
        X509_free(cert);
    }
    
    auto validate_private_key_file(const std::string& key_path) -> void {
        if (key_path.empty()) {
            return; // Empty path is valid (optional key)
        }
        
        if (!std::filesystem::exists(key_path)) {
            throw kythira::ssl_configuration_error(
                std::format("Private key file does not exist: {}", key_path));
        }
        
        // Try to load and validate the private key
        std::ifstream key_file(key_path, std::ios::binary);
        if (!key_file.is_open()) {
            throw kythira::ssl_configuration_error(
                std::format("Cannot open private key file: {}", key_path));
        }
        
        // Read key content
        std::string key_content((std::istreambuf_iterator<char>(key_file)),
                               std::istreambuf_iterator<char>());
        key_file.close();
        
        // Create BIO from key content
        BIO* bio = BIO_new_mem_buf(key_content.c_str(), static_cast<int>(key_content.length()));
        if (!bio) {
            throw kythira::ssl_configuration_error(
                std::format("Failed to create BIO for private key: {}", key_path));
        }
        
        // Try to load private key
        EVP_PKEY* pkey = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
        BIO_free(bio);
        
        if (!pkey) {
            unsigned long err = ERR_get_error();
            char err_buf[256];
            ERR_error_string_n(err, err_buf, sizeof(err_buf));
            throw kythira::certificate_validation_error(
                std::format("Invalid private key format in {}: {}", key_path, err_buf));
        }
        
        EVP_PKEY_free(pkey);
    }
    
    auto validate_certificate_key_pair(const std::string& cert_path, const std::string& key_path) -> void {
        if (cert_path.empty() || key_path.empty()) {
            return; // Skip validation if either is empty
        }
        
        // Load certificate
        std::ifstream cert_file(cert_path, std::ios::binary);
        std::string cert_content((std::istreambuf_iterator<char>(cert_file)),
                                std::istreambuf_iterator<char>());
        cert_file.close();
        
        BIO* cert_bio = BIO_new_mem_buf(cert_content.c_str(), static_cast<int>(cert_content.length()));
        X509* cert = PEM_read_bio_X509(cert_bio, nullptr, nullptr, nullptr);
        if (!cert) {
            BIO_reset(cert_bio);
            cert = d2i_X509_bio(cert_bio, nullptr);
        }
        BIO_free(cert_bio);
        
        if (!cert) {
            throw kythira::certificate_validation_error(
                std::format("Failed to load certificate for key pair validation: {}", cert_path));
        }
        
        // Load private key
        std::ifstream key_file(key_path, std::ios::binary);
        std::string key_content((std::istreambuf_iterator<char>(key_file)),
                               std::istreambuf_iterator<char>());
        key_file.close();
        
        BIO* key_bio = BIO_new_mem_buf(key_content.c_str(), static_cast<int>(key_content.length()));
        EVP_PKEY* pkey = PEM_read_bio_PrivateKey(key_bio, nullptr, nullptr, nullptr);
        BIO_free(key_bio);
        
        if (!pkey) {
            X509_free(cert);
            throw kythira::certificate_validation_error(
                std::format("Failed to load private key for key pair validation: {}", key_path));
        }
        
        // Verify that the private key matches the certificate
        EVP_PKEY* cert_pkey = X509_get_pubkey(cert);
        if (!cert_pkey) {
            EVP_PKEY_free(pkey);
            X509_free(cert);
            throw kythira::certificate_validation_error(
                std::format("Failed to extract public key from certificate: {}", cert_path));
        }
        
        // Verify that the private key matches the certificate
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
        // OpenSSL 3.0+ - use EVP_PKEY_eq
        int key_match = EVP_PKEY_eq(pkey, cert_pkey);
#else
        // OpenSSL 1.1.1 and earlier - use EVP_PKEY_cmp
        int key_match = EVP_PKEY_cmp(pkey, cert_pkey);
#endif
        
        EVP_PKEY_free(cert_pkey);
        EVP_PKEY_free(pkey);
        X509_free(cert);
        
        if (key_match != 1) {
            throw kythira::certificate_validation_error(
                std::format("Private key does not match certificate: {} and {}", key_path, cert_path));
        }
    }
    
    auto validate_certificate_chain(const std::string& cert_path, const std::string& ca_cert_path) -> void {
        if (cert_path.empty()) {
            return; // No certificate to validate
        }
        
        // Load the certificate to validate
        std::ifstream cert_file(cert_path, std::ios::binary);
        std::string cert_content((std::istreambuf_iterator<char>(cert_file)),
                                std::istreambuf_iterator<char>());
        cert_file.close();
        
        BIO* cert_bio = BIO_new_mem_buf(cert_content.c_str(), static_cast<int>(cert_content.length()));
        X509* cert = PEM_read_bio_X509(cert_bio, nullptr, nullptr, nullptr);
        if (!cert) {
            BIO_reset(cert_bio);
            cert = d2i_X509_bio(cert_bio, nullptr);
        }
        BIO_free(cert_bio);
        
        if (!cert) {
            throw kythira::certificate_validation_error(
                std::format("Failed to load certificate for chain validation: {}", cert_path));
        }
        
        // Create certificate store
        X509_STORE* store = X509_STORE_new();
        if (!store) {
            X509_free(cert);
            throw kythira::certificate_validation_error("Failed to create certificate store");
        }
        
        // Add CA certificate to store if provided
        if (!ca_cert_path.empty()) {
            std::ifstream ca_file(ca_cert_path, std::ios::binary);
            std::string ca_content((std::istreambuf_iterator<char>(ca_file)),
                                  std::istreambuf_iterator<char>());
            ca_file.close();
            
            BIO* ca_bio = BIO_new_mem_buf(ca_content.c_str(), static_cast<int>(ca_content.length()));
            X509* ca_cert = PEM_read_bio_X509(ca_bio, nullptr, nullptr, nullptr);
            if (!ca_cert) {
                BIO_reset(ca_bio);
                ca_cert = d2i_X509_bio(ca_bio, nullptr);
            }
            BIO_free(ca_bio);
            
            if (!ca_cert) {
                X509_STORE_free(store);
                X509_free(cert);
                throw kythira::certificate_validation_error(
                    std::format("Failed to load CA certificate: {}", ca_cert_path));
            }
            
            if (X509_STORE_add_cert(store, ca_cert) != 1) {
                X509_free(ca_cert);
                X509_STORE_free(store);
                X509_free(cert);
                throw kythira::certificate_validation_error(
                    std::format("Failed to add CA certificate to store: {}", ca_cert_path));
            }
            
            X509_free(ca_cert);
        } else {
            // Load default CA certificates from system
            if (X509_STORE_set_default_paths(store) != 1) {
                X509_STORE_free(store);
                X509_free(cert);
                throw kythira::certificate_validation_error("Failed to load default CA certificates");
            }
        }
        
        // Create certificate store context for validation
        X509_STORE_CTX* ctx = X509_STORE_CTX_new();
        if (!ctx) {
            X509_STORE_free(store);
            X509_free(cert);
            throw kythira::certificate_validation_error("Failed to create certificate store context");
        }
        
        // Initialize context for certificate validation
        if (X509_STORE_CTX_init(ctx, store, cert, nullptr) != 1) {
            X509_STORE_CTX_free(ctx);
            X509_STORE_free(store);
            X509_free(cert);
            throw kythira::certificate_validation_error("Failed to initialize certificate store context");
        }
        
        // Perform certificate chain validation
        int verify_result = X509_verify_cert(ctx);
        
        if (verify_result != 1) {
            int error = X509_STORE_CTX_get_error(ctx);
            const char* error_string = X509_verify_cert_error_string(error);
            
            X509_STORE_CTX_free(ctx);
            X509_STORE_free(store);
            X509_free(cert);
            
            throw kythira::certificate_validation_error(
                std::format("Certificate chain validation failed for {}: {} (error {})", 
                           cert_path, error_string, error));
        }
        
        // Check certificate revocation if supported
        // Note: This would require CRL or OCSP checking, which is complex
        // For now, we'll skip revocation checking but the framework is here
        
        X509_STORE_CTX_free(ctx);
        X509_STORE_free(store);
        X509_free(cert);
    }
    
    auto check_certificate_expiration(const std::string& cert_path) -> void {
        if (cert_path.empty()) {
            return; // No certificate to check
        }
        
        // Load certificate
        std::ifstream cert_file(cert_path, std::ios::binary);
        std::string cert_content((std::istreambuf_iterator<char>(cert_file)),
                                std::istreambuf_iterator<char>());
        cert_file.close();
        
        BIO* cert_bio = BIO_new_mem_buf(cert_content.c_str(), static_cast<int>(cert_content.length()));
        X509* cert = PEM_read_bio_X509(cert_bio, nullptr, nullptr, nullptr);
        if (!cert) {
            BIO_reset(cert_bio);
            cert = d2i_X509_bio(cert_bio, nullptr);
        }
        BIO_free(cert_bio);
        
        if (!cert) {
            throw kythira::certificate_validation_error(
                std::format("Failed to load certificate for expiration check: {}", cert_path));
        }
        
        // Check certificate validity period
        ASN1_TIME* not_before = X509_get_notBefore(cert);
        ASN1_TIME* not_after = X509_get_notAfter(cert);
        
        if (X509_cmp_current_time(not_before) > 0) {
            X509_free(cert);
            throw kythira::certificate_validation_error(
                std::format("Certificate not yet valid: {}", cert_path));
        }
        
        if (X509_cmp_current_time(not_after) < 0) {
            X509_free(cert);
            throw kythira::certificate_validation_error(
                std::format("Certificate has expired: {}", cert_path));
        }
        
        // Check if certificate expires soon (within 30 days)
        time_t now = time(nullptr);
        time_t expires = ASN1_TIME_to_time_t(not_after);
        
        if (expires - now < 30 * 24 * 60 * 60) { // 30 days in seconds
            // Log warning about upcoming expiration
            // For now, we'll just continue - in production, this should log a warning
        }
        
        X509_free(cert);
    }
    
    // Helper to convert ASN1_TIME to time_t
    auto ASN1_TIME_to_time_t(ASN1_TIME* asn1_time) -> time_t {
        if (!asn1_time) return 0;
        
        // Use OpenSSL's ASN1_TIME_to_tm function if available (OpenSSL 1.1.1+)
        struct tm tm_time;
        if (ASN1_TIME_to_tm(asn1_time, &tm_time) == 1) {
            return mktime(&tm_time);
        }
        
        // Fallback: manual parsing for older OpenSSL versions
        memset(&tm_time, 0, sizeof(tm_time));
        
        // Parse ASN1_TIME format (YYMMDDHHMMSSZ or YYYYMMDDHHMMSSZ)
        const char* time_str = reinterpret_cast<const char*>(asn1_time->data);
        int len = asn1_time->length;
        
        if (asn1_time->type == V_ASN1_UTCTIME) {
            // YYMMDDHHMMSSZ format
            if (len != 13) return 0;
            
            int year = (time_str[0] - '0') * 10 + (time_str[1] - '0');
            if (year < 50) year += 100; // Y2K handling: 00-49 = 2000-2049, 50-99 = 1950-1999
            tm_time.tm_year = year;
            
            tm_time.tm_mon = (time_str[2] - '0') * 10 + (time_str[3] - '0') - 1;
            tm_time.tm_mday = (time_str[4] - '0') * 10 + (time_str[5] - '0');
            tm_time.tm_hour = (time_str[6] - '0') * 10 + (time_str[7] - '0');
            tm_time.tm_min = (time_str[8] - '0') * 10 + (time_str[9] - '0');
            tm_time.tm_sec = (time_str[10] - '0') * 10 + (time_str[11] - '0');
        } else if (asn1_time->type == V_ASN1_GENERALIZEDTIME) {
            // YYYYMMDDHHMMSSZ format
            if (len != 15) return 0;
            
            tm_time.tm_year = (time_str[0] - '0') * 1000 + (time_str[1] - '0') * 100 + 
                             (time_str[2] - '0') * 10 + (time_str[3] - '0') - 1900;
            tm_time.tm_mon = (time_str[4] - '0') * 10 + (time_str[5] - '0') - 1;
            tm_time.tm_mday = (time_str[6] - '0') * 10 + (time_str[7] - '0');
            tm_time.tm_hour = (time_str[8] - '0') * 10 + (time_str[9] - '0');
            tm_time.tm_min = (time_str[10] - '0') * 10 + (time_str[11] - '0');
            tm_time.tm_sec = (time_str[12] - '0') * 10 + (time_str[13] - '0');
        } else {
            return 0; // Unsupported format
        }
        
        return mktime(&tm_time);
    }
    
    auto validate_cipher_suites(const std::string& cipher_suites) -> void {
        if (cipher_suites.empty()) {
            return; // Empty cipher suites is valid (use defaults)
        }
        
        // Create a temporary SSL context to validate cipher suites
        SSL_CTX* ctx = SSL_CTX_new(TLS_method());
        if (!ctx) {
            throw kythira::ssl_context_error("Failed to create SSL context for cipher suite validation");
        }
        
        // Try to set the cipher suites
        if (SSL_CTX_set_cipher_list(ctx, cipher_suites.c_str()) != 1) {
            SSL_CTX_free(ctx);
            unsigned long err = ERR_get_error();
            char err_buf[256];
            ERR_error_string_n(err, err_buf, sizeof(err_buf));
            throw kythira::ssl_configuration_error(
                std::format("Invalid cipher suites '{}': {}", cipher_suites, err_buf));
        }
        
        SSL_CTX_free(ctx);
    }
    
    auto validate_tls_version(const std::string& version) -> int {
        if (version == "TLSv1.0") return TLS1_VERSION;
        if (version == "TLSv1.1") return TLS1_1_VERSION;
        if (version == "TLSv1.2") return TLS1_2_VERSION;
        if (version == "TLSv1.3") return TLS1_3_VERSION;
        
        throw kythira::ssl_configuration_error(
            std::format("Unsupported TLS version: {}", version));
    }
    
    auto validate_tls_version_range(const std::string& min_version, const std::string& max_version) -> void {
        if (min_version.empty() && max_version.empty()) {
            return; // Empty versions are valid (use defaults)
        }
        
        int min_ver = min_version.empty() ? TLS1_2_VERSION : validate_tls_version(min_version);
        int max_ver = max_version.empty() ? TLS1_3_VERSION : validate_tls_version(max_version);
        
        if (min_ver > max_ver) {
            throw kythira::ssl_configuration_error(
                std::format("Minimum TLS version ({}) is higher than maximum TLS version ({})", 
                           min_version, max_version));
        }
        
        // Ensure minimum security standards (TLS 1.2 or higher)
        if (min_ver < TLS1_2_VERSION) {
            throw kythira::ssl_configuration_error(
                std::format("Minimum TLS version ({}) is below security requirements (TLS 1.2 minimum)", 
                           min_version));
        }
    }
    
    auto configure_ssl_context(SSL_CTX* ctx, const std::string& cipher_suites, 
                              const std::string& min_tls_version, const std::string& max_tls_version) -> void {
        if (!ctx) {
            throw kythira::ssl_context_error("Cannot configure null SSL context");
        }
        
        // Configure cipher suites
        if (!cipher_suites.empty()) {
            if (SSL_CTX_set_cipher_list(ctx, cipher_suites.c_str()) != 1) {
                unsigned long err = ERR_get_error();
                char err_buf[256];
                ERR_error_string_n(err, err_buf, sizeof(err_buf));
                throw kythira::ssl_context_error(
                    std::format("Failed to set cipher suites '{}': {}", cipher_suites, err_buf));
            }
        }
        
        // Configure TLS version range
        if (!min_tls_version.empty()) {
            int min_ver = validate_tls_version(min_tls_version);
            if (SSL_CTX_set_min_proto_version(ctx, min_ver) != 1) {
                throw kythira::ssl_context_error(
                    std::format("Failed to set minimum TLS version: {}", min_tls_version));
            }
        }
        
        if (!max_tls_version.empty()) {
            int max_ver = validate_tls_version(max_tls_version);
            if (SSL_CTX_set_max_proto_version(ctx, max_ver) != 1) {
                throw kythira::ssl_context_error(
                    std::format("Failed to set maximum TLS version: {}", max_tls_version));
            }
        }
        
        // Set security options
        SSL_CTX_set_options(ctx, SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3 | SSL_OP_NO_COMPRESSION);
        
        // Set verification mode for better security
        SSL_CTX_set_verify_depth(ctx, 10); // Allow certificate chains up to 10 levels deep
    }
    
    auto verify_client_certificate(X509* client_cert, X509_STORE* ca_store) -> bool {
        if (!client_cert || !ca_store) {
            return false;
        }
        
        // Create certificate store context for validation
        X509_STORE_CTX* ctx = X509_STORE_CTX_new();
        if (!ctx) {
            return false;
        }
        
        // Initialize context for certificate validation
        if (X509_STORE_CTX_init(ctx, ca_store, client_cert, nullptr) != 1) {
            X509_STORE_CTX_free(ctx);
            return false;
        }
        
        // Perform certificate validation
        int verify_result = X509_verify_cert(ctx);
        
        X509_STORE_CTX_free(ctx);
        
        return verify_result == 1;
    }
    
    auto extract_client_certificate_info(X509* client_cert) -> std::string {
        if (!client_cert) {
            return "No client certificate";
        }
        
        // Extract subject name
        X509_NAME* subject = X509_get_subject_name(client_cert);
        if (!subject) {
            return "Invalid client certificate subject";
        }
        
        // Convert subject name to string
        BIO* bio = BIO_new(BIO_s_mem());
        if (!bio) {
            return "Failed to create BIO for subject";
        }
        
        if (X509_NAME_print_ex(bio, subject, 0, XN_FLAG_ONELINE) <= 0) {
            BIO_free(bio);
            return "Failed to print subject name";
        }
        
        char* subject_str = nullptr;
        long subject_len = BIO_get_mem_data(bio, &subject_str);
        
        std::string result;
        if (subject_str && subject_len > 0) {
            result = std::string(subject_str, subject_len);
        } else {
            result = "Empty subject name";
        }
        
        BIO_free(bio);
        return result;
    }
#endif
    
    // Helper function to create futures with exceptions
    template<typename Types, typename Response>
    auto make_future_with_exception(const std::exception& e) -> typename Types::template future_template<Response> {
#ifdef FOLLY_AVAILABLE
        if constexpr (std::is_same_v<typename Types::template future_template<Response>, folly::Future<Response>>) {
            return folly::makeFuture<Response>(e);
        } else
#endif
        {
            // For SimpleFuture or std::future
            return typename Types::template future_template<Response>(std::make_exception_ptr(e));
        }
    }
    
    // Helper function to create futures with values
    template<typename Types, typename Response>
    auto make_future_with_value(Response&& value) -> typename Types::template future_template<Response> {
#ifdef FOLLY_AVAILABLE
        if constexpr (std::is_same_v<typename Types::template future_template<Response>, folly::Future<Response>>) {
            return folly::makeFuture<Response>(std::forward<Response>(value));
        } else
#endif
        {
            // For SimpleFuture or std::future
            return typename Types::template future_template<Response>(std::forward<Response>(value));
        }
    }
}

// Constructor implementation
template<typename Types>
requires kythira::transport_types<Types>
cpp_httplib_client<Types>::cpp_httplib_client(
    std::unordered_map<std::uint64_t, std::string> node_id_to_url_map,
    cpp_httplib_client_config config,
    typename Types::metrics_type metrics
)
    : _serializer{}
    , _node_id_to_url{std::move(node_id_to_url_map)}
    , _http_clients{}
    , _config{std::move(config)}
    , _metrics{std::move(metrics)}
    , _mutex{}
{
    // Validate SSL certificate configuration if provided
    try {
        validate_certificate_files();
        load_client_certificates();
    } catch (const std::exception& e) {
        throw kythira::ssl_configuration_error(
            std::format("SSL configuration error during client construction: {}", e.what()));
    }
}

// Destructor implementation
template<typename Types>
requires kythira::transport_types<Types>
cpp_httplib_client<Types>::~cpp_httplib_client() = default;

// Validate certificate files
template<typename Types>
requires kythira::transport_types<Types>
auto cpp_httplib_client<Types>::validate_certificate_files() const -> void {
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
    // Validate cipher suites configuration
    validate_cipher_suites(_config.cipher_suites);
    
    // Validate TLS version range
    validate_tls_version_range(_config.min_tls_version, _config.max_tls_version);
    
    // Validate CA certificate if provided
    if (!_config.ca_cert_path.empty()) {
        validate_certificate_file(_config.ca_cert_path);
        check_certificate_expiration(_config.ca_cert_path);
    }
    
    // Validate client certificate and key if provided
    if (!_config.client_cert_path.empty()) {
        validate_certificate_file(_config.client_cert_path);
        check_certificate_expiration(_config.client_cert_path);
        
        if (_config.client_key_path.empty()) {
            throw kythira::ssl_configuration_error(
                "Client certificate provided but no private key specified");
        }
        
        validate_private_key_file(_config.client_key_path);
        validate_certificate_key_pair(_config.client_cert_path, _config.client_key_path);
        
        // Validate certificate chain if CA certificate is provided
        if (!_config.ca_cert_path.empty()) {
            validate_certificate_chain(_config.client_cert_path, _config.ca_cert_path);
        } else {
            // Validate against system CA certificates
            validate_certificate_chain(_config.client_cert_path, "");
        }
    } else if (!_config.client_key_path.empty()) {
        throw kythira::ssl_configuration_error(
            "Client private key provided but no certificate specified");
    }
#else
    // If SSL support is not available, check if SSL configuration is attempted
    if (!_config.ca_cert_path.empty() || !_config.client_cert_path.empty() || 
        !_config.client_key_path.empty() || !_config.cipher_suites.empty()) {
        throw kythira::ssl_configuration_error(
            "SSL configuration provided but OpenSSL support not available");
    }
#endif
}

// Load client certificates
template<typename Types>
requires kythira::transport_types<Types>
auto cpp_httplib_client<Types>::load_client_certificates() -> void {
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
    // Certificate loading is handled per-client in get_or_create_client
    // This method validates that certificates can be loaded
    if (!_config.client_cert_path.empty() && !_config.client_key_path.empty()) {
        // Validate that we can load the certificates
        validate_certificate_key_pair(_config.client_cert_path, _config.client_key_path);
    }
#endif
}

// Configure SSL for a client
template<typename Types>
requires kythira::transport_types<Types>
auto cpp_httplib_client<Types>::configure_ssl_client(httplib::Client* client) -> void {
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
    if (!client) {
        throw kythira::ssl_configuration_error("Cannot configure SSL on null client");
    }
    
    // Set CA certificate path for server verification
    if (!_config.ca_cert_path.empty()) {
        client->set_ca_cert_path(_config.ca_cert_path.c_str());
    }
    
    // Enable/disable certificate verification
    client->enable_server_certificate_verification(_config.enable_ssl_verification);
    
    // Set client certificate and key for mutual TLS
    if (!_config.client_cert_path.empty() && !_config.client_key_path.empty()) {
        // Note: cpp-httplib may not support client certificate configuration directly
        // This would require either:
        // 1. Using a different HTTP library with full SSL client cert support
        // 2. Patching cpp-httplib to add client certificate support
        // 3. Using custom SSL context configuration if supported by cpp-httplib version
        
        // For now, we validate that the certificates are loadable but note that
        // client certificate authentication may not be fully functional with cpp-httplib
        try {
            validate_certificate_key_pair(_config.client_cert_path, _config.client_key_path);
        } catch (const std::exception& e) {
            throw kythira::ssl_configuration_error(
                std::format("Client certificate validation failed: {}", e.what()));
        }
        
        // Log that client certificate is configured but may not be applied
        // In a production implementation, this would require either:
        // 1. Using a different HTTP library with full SSL client cert support
        // 2. Custom SSL context handling if supported by the cpp-httplib version
    }
    
    // Configure SSL context parameters
    // Note: cpp-httplib may not expose direct SSL context configuration
    // This is a limitation of the library - for full SSL context control,
    // a different HTTP library or custom SSL context handling would be needed
    
    // For now, we validate the configuration but note that some advanced
    // SSL context parameters may not be fully configurable through cpp-httplib
    if (!_config.cipher_suites.empty() || 
        _config.min_tls_version != "TLSv1.2" || 
        _config.max_tls_version != "TLSv1.3") {
        
        // Log that advanced SSL configuration is validated but may not be fully applied
        // In a production implementation, this would require either:
        // 1. Using a different HTTP library with full SSL context control
        // 2. Patching cpp-httplib to expose SSL context configuration
        // 3. Using custom SSL context callbacks if supported by the cpp-httplib version
    }
#else
    throw kythira::ssl_configuration_error("SSL support not available (OpenSSL not enabled)");
#endif
}

// Helper to get base URL for a node
template<typename Types>
requires kythira::transport_types<Types>
auto cpp_httplib_client<Types>::get_base_url(std::uint64_t node_id) const -> std::string {
    std::lock_guard<std::mutex> lock(_mutex);
    auto it = _node_id_to_url.find(node_id);
    if (it == _node_id_to_url.end()) {
        throw std::runtime_error(std::format("No URL mapping found for node {}", node_id));
    }
    return it->second;
}

// Helper to get or create HTTP client for a node
template<typename Types>
requires kythira::transport_types<Types>
auto cpp_httplib_client<Types>::get_or_create_client(std::uint64_t node_id) -> httplib::Client* {
    std::lock_guard<std::mutex> lock(_mutex);
    
    auto it = _http_clients.find(node_id);
    if (it != _http_clients.end()) {
        // Emit connection reused metric
        auto metric = _metrics;
        metric.set_metric_name("http.client.connection.reused");
        metric.add_dimension("target_node_id", std::to_string(node_id));
        metric.add_one();
        metric.emit();
        
        return it->second.get();
    }
    
    // Get base URL
    auto url_it = _node_id_to_url.find(node_id);
    if (url_it == _node_id_to_url.end()) {
        throw std::runtime_error(std::format("No URL mapping found for node {}", node_id));
    }
    
    const auto& base_url = url_it->second;
    
    // Parse URL to determine if HTTPS
    bool is_https = base_url.starts_with("https://");
    
    // Create new client
    std::unique_ptr<httplib::Client> client;
    try {
        if (is_https) {
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
            // For HTTPS, we need to use the universal Client constructor
            client = std::make_unique<httplib::Client>(base_url);
            
            // Configure SSL settings
            configure_ssl_client(client.get());
#else
            throw kythira::ssl_configuration_error("HTTPS support not available (OpenSSL not enabled)");
#endif
        } else {
            client = std::make_unique<httplib::Client>(base_url);
        }
        
        // Configure timeouts
        client->set_connection_timeout(_config.connection_timeout.count() / 1000, 
                                       (_config.connection_timeout.count() % 1000) * 1000);
        client->set_read_timeout(_config.request_timeout.count() / 1000,
                                (_config.request_timeout.count() % 1000) * 1000);
        client->set_write_timeout(_config.request_timeout.count() / 1000,
                                 (_config.request_timeout.count() % 1000) * 1000);
        
        // Enable keep-alive
        client->set_keep_alive(true);
        
    } catch (const std::exception& e) {
        throw kythira::ssl_configuration_error(
            std::format("Failed to create HTTP client for node {}: {}", node_id, e.what()));
    }
    
    // Store and return
    auto* client_ptr = client.get();
    _http_clients[node_id] = std::move(client);
    
    // Emit connection created metric
    auto metric = _metrics;
    metric.set_metric_name("http.client.connection.created");
    metric.add_dimension("target_node_id", std::to_string(node_id));
    metric.add_one();
    metric.emit();
    
    // Update pool size metric
    metric = _metrics;
    metric.set_metric_name("http.client.connection.pool_size");
    metric.add_dimension("target_node_id", std::to_string(node_id));
    metric.add_value(static_cast<double>(_http_clients.size()));
    metric.emit();
    
    return client_ptr;
}

// Generic RPC send implementation
template<typename Types>
requires kythira::transport_types<Types>
template<typename Request, typename Response>
auto cpp_httplib_client<Types>::send_rpc(
    std::uint64_t target,
    const std::string& endpoint,
    const Request& request,
    std::chrono::milliseconds timeout
) -> typename Types::template future_template<Response> {
    try {
        // Get or create HTTP client
        auto* client = this->get_or_create_client(target);
        
        // Serialize request
        auto serialized_data = _serializer.serialize(request);
        
        // Convert bytes to string for HTTP body
        std::string body;
        body.reserve(serialized_data.size());
        for (auto b : serialized_data) {
            body.push_back(static_cast<char>(b));
        }
        
        // Set headers
        httplib::Headers headers;
        headers.emplace(header_content_type, content_type_json);
        // Let cpp-httplib handle Content-Length automatically
        headers.emplace(header_user_agent, _config.user_agent);
        
        // Determine RPC type for metrics
        std::string rpc_type;
        if (endpoint == endpoint_request_vote) {
            rpc_type = "request_vote";
        } else if (endpoint == endpoint_append_entries) {
            rpc_type = "append_entries";
        } else if (endpoint == endpoint_install_snapshot) {
            rpc_type = "install_snapshot";
        }
        
        // Record request metrics
        auto start_time = std::chrono::steady_clock::now();
        auto metric = _metrics;
        metric.set_metric_name("http.client.request.sent");
        metric.add_dimension("rpc_type", rpc_type);
        metric.add_dimension("target_node_id", std::to_string(target));
        metric.add_one();
        metric.emit();
        
        metric = _metrics;
        metric.set_metric_name("http.client.request.size");
        metric.add_dimension("rpc_type", rpc_type);
        metric.add_dimension("target_node_id", std::to_string(target));
        metric.add_value(static_cast<double>(body.size()));
        metric.emit();
        
        // Send POST request
        auto result = client->Post(endpoint.c_str(), headers, body, content_type_json);
        
        // Record latency
        auto end_time = std::chrono::steady_clock::now();
        auto latency = std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time);
        
        if (!result) {
            // Connection error or timeout
            std::string error_type = "connection_failed";
            if (result.error() == httplib::Error::ConnectionTimeout ||
                result.error() == httplib::Error::Read) {
                error_type = "timeout";
            }
            
            auto error_metric = _metrics;
            error_metric.set_metric_name("http.client.error");
            error_metric.add_dimension("error_type", error_type);
            error_metric.add_dimension("target_node_id", std::to_string(target));
            error_metric.add_one();
            error_metric.emit();
            
            // Record error latency
            auto latency_metric = _metrics;
            latency_metric.set_metric_name("http.client.request.latency");
            latency_metric.add_dimension("rpc_type", rpc_type);
            latency_metric.add_dimension("target_node_id", std::to_string(target));
            latency_metric.add_dimension("status", "error");
            latency_metric.add_duration(latency);
            latency_metric.emit();
            
            if (error_type == "timeout") {
                return make_future_with_exception<Types, Response>(kythira::http_timeout_error(
                    std::format("HTTP request timed out after {}ms", timeout.count())));
            } else {
                return make_future_with_exception<Types, Response>(std::runtime_error(
                    std::format("HTTP request failed: {}", httplib::to_string(result.error()))));
            }
        } else if (result->status == 200) {
            // Success - deserialize response
            try {
                std::vector<std::byte> response_data;
                response_data.reserve(result->body.size());
                for (char c : result->body) {
                    response_data.push_back(static_cast<std::byte>(c));
                }
                
                Response response;
                if constexpr (std::is_same_v<Response, kythira::request_vote_response<>>) {
                    response = _serializer.deserialize_request_vote_response(response_data);
                } else if constexpr (std::is_same_v<Response, kythira::append_entries_response<>>) {
                    response = _serializer.deserialize_append_entries_response(response_data);
                } else if constexpr (std::is_same_v<Response, kythira::install_snapshot_response<>>) {
                    response = _serializer.deserialize_install_snapshot_response(response_data);
                }
                
                // Record response size
                auto size_metric = _metrics;
                size_metric.set_metric_name("http.client.response.size");
                size_metric.add_dimension("rpc_type", rpc_type);
                size_metric.add_dimension("target_node_id", std::to_string(target));
                size_metric.add_value(static_cast<double>(result->body.size()));
                size_metric.emit();
                
                // Record success latency
                auto latency_metric = _metrics;
                latency_metric.set_metric_name("http.client.request.latency");
                latency_metric.add_dimension("rpc_type", rpc_type);
                latency_metric.add_dimension("target_node_id", std::to_string(target));
                latency_metric.add_dimension("status", "success");
                latency_metric.add_duration(latency);
                latency_metric.emit();
                
                return make_future_with_value<Types, Response>(std::move(response));
            } catch (const std::exception& e) {
                // Deserialization error
                auto error_metric = _metrics;
                error_metric.set_metric_name("http.client.error");
                error_metric.add_dimension("error_type", "deserialization_failed");
                error_metric.add_dimension("target_node_id", std::to_string(target));
                error_metric.add_one();
                error_metric.emit();
                
                return make_future_with_exception<Types, Response>(kythira::serialization_error(
                    std::format("Failed to deserialize response: {}", e.what())));
            }
        } else if (result->status >= 400 && result->status < 500) {
            // Client error (4xx)
            auto error_metric = _metrics;
            error_metric.set_metric_name("http.client.error");
            error_metric.add_dimension("error_type", "4xx");
            error_metric.add_dimension("target_node_id", std::to_string(target));
            error_metric.add_one();
            error_metric.emit();
            
            // Record error latency
            auto latency_metric = _metrics;
            latency_metric.set_metric_name("http.client.request.latency");
            latency_metric.add_dimension("rpc_type", rpc_type);
            latency_metric.add_dimension("target_node_id", std::to_string(target));
            latency_metric.add_dimension("status", "error");
            latency_metric.add_duration(latency);
            latency_metric.emit();
            
            return make_future_with_exception<Types, Response>(kythira::http_client_error(
                result->status,
                std::format("HTTP client error {}: {}", result->status, result->body)));
        } else if (result->status >= 500) {
            // Server error (5xx)
            auto error_metric = _metrics;
            error_metric.set_metric_name("http.client.error");
            error_metric.add_dimension("error_type", "5xx");
            error_metric.add_dimension("target_node_id", std::to_string(target));
            error_metric.add_one();
            error_metric.emit();
            
            // Record error latency
            auto latency_metric = _metrics;
            latency_metric.set_metric_name("http.client.request.latency");
            latency_metric.add_dimension("rpc_type", rpc_type);
            latency_metric.add_dimension("target_node_id", std::to_string(target));
            latency_metric.add_dimension("status", "error");
            latency_metric.add_duration(latency);
            latency_metric.emit();
            
            return make_future_with_exception<Types, Response>(kythira::http_server_error(
                result->status,
                std::format("HTTP server error {}: {}", result->status, result->body)));
        } else {
            // Unexpected status code
            return make_future_with_exception<Types, Response>(std::runtime_error(
                std::format("Unexpected HTTP status code: {}", result->status)));
        }
    } catch (const std::exception& e) {
        return make_future_with_exception<Types, Response>(e);
    }
}

// send_request_vote implementation
template<typename Types>
requires kythira::transport_types<Types>
auto cpp_httplib_client<Types>::send_request_vote(
    std::uint64_t target,
    const kythira::request_vote_request<>& request,
    std::chrono::milliseconds timeout
) -> typename Types::template future_template<kythira::request_vote_response<>> {
    return send_rpc<kythira::request_vote_request<>, kythira::request_vote_response<>>(
        target, endpoint_request_vote, request, timeout);
}

// send_append_entries implementation
template<typename Types>
requires kythira::transport_types<Types>
auto cpp_httplib_client<Types>::send_append_entries(
    std::uint64_t target,
    const kythira::append_entries_request<>& request,
    std::chrono::milliseconds timeout
) -> typename Types::template future_template<kythira::append_entries_response<>> {
    return send_rpc<kythira::append_entries_request<>, kythira::append_entries_response<>>(
        target, endpoint_append_entries, request, timeout);
}

// send_install_snapshot implementation
template<typename Types>
requires kythira::transport_types<Types>
auto cpp_httplib_client<Types>::send_install_snapshot(
    std::uint64_t target,
    const kythira::install_snapshot_request<>& request,
    std::chrono::milliseconds timeout
) -> typename Types::template future_template<kythira::install_snapshot_response<>> {
    return send_rpc<kythira::install_snapshot_request<>, kythira::install_snapshot_response<>>(
        target, endpoint_install_snapshot, request, timeout);
}

// Server constructor implementation
template<typename Types>
requires kythira::transport_types<Types>
cpp_httplib_server<Types>::cpp_httplib_server(
    std::string bind_address,
    std::uint16_t bind_port,
    cpp_httplib_server_config config,
    typename Types::metrics_type metrics
)
    : _serializer{}
    , _http_server{std::make_unique<httplib::Server>()}
    , _request_vote_handler{}
    , _append_entries_handler{}
    , _install_snapshot_handler{}
    , _bind_address{std::move(bind_address)}
    , _bind_port{bind_port}
    , _config{std::move(config)}
    , _metrics{std::move(metrics)}
    , _running{false}
    , _mutex{}
{
    // Validate SSL certificate configuration if SSL is enabled
    try {
        validate_certificate_files();
        if (_config.enable_ssl) {
            load_server_certificates();
        }
    } catch (const std::exception& e) {
        throw kythira::ssl_configuration_error(
            std::format("SSL configuration error during server construction: {}", e.what()));
    }
    
    // Configure server settings
    _http_server->set_payload_max_length(_config.max_request_body_size);
    _http_server->set_read_timeout(_config.request_timeout.count());
    _http_server->set_write_timeout(_config.request_timeout.count());
}

// Server destructor implementation
template<typename Types>
requires kythira::transport_types<Types>
cpp_httplib_server<Types>::~cpp_httplib_server() {
    if (_running.load()) {
        stop();
    }
}

// Validate certificate files for server
template<typename Types>
requires kythira::transport_types<Types>
auto cpp_httplib_server<Types>::validate_certificate_files() const -> void {
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
    if (_config.enable_ssl) {
        // Validate cipher suites configuration
        validate_cipher_suites(_config.cipher_suites);
        
        // Validate TLS version range
        validate_tls_version_range(_config.min_tls_version, _config.max_tls_version);
        
        // Server certificate and key are required for SSL
        if (_config.ssl_cert_path.empty()) {
            throw kythira::ssl_configuration_error(
                "SSL enabled but no server certificate path provided");
        }
        
        if (_config.ssl_key_path.empty()) {
            throw kythira::ssl_configuration_error(
                "SSL enabled but no server private key path provided");
        }
        
        // Validate server certificate and key
        validate_certificate_file(_config.ssl_cert_path);
        check_certificate_expiration(_config.ssl_cert_path);
        validate_private_key_file(_config.ssl_key_path);
        validate_certificate_key_pair(_config.ssl_cert_path, _config.ssl_key_path);
        
        // Validate server certificate chain
        if (!_config.ca_cert_path.empty()) {
            // Validate against provided CA certificate
            validate_certificate_chain(_config.ssl_cert_path, _config.ca_cert_path);
        } else {
            // Validate against system CA certificates
            validate_certificate_chain(_config.ssl_cert_path, "");
        }
        
        // Validate CA certificate if client certificate authentication is required
        if (_config.require_client_cert) {
            if (_config.ca_cert_path.empty()) {
                throw kythira::ssl_configuration_error(
                    "Client certificate authentication enabled but no CA certificate path provided");
            }
            validate_certificate_file(_config.ca_cert_path);
            check_certificate_expiration(_config.ca_cert_path);
        }
    } else {
        // If SSL is disabled, check if SSL configuration is provided
        if (!_config.ssl_cert_path.empty() || !_config.ssl_key_path.empty() || 
            !_config.ca_cert_path.empty() || _config.require_client_cert ||
            !_config.cipher_suites.empty()) {
            throw kythira::ssl_configuration_error(
                "SSL configuration provided but SSL is disabled");
        }
    }
#else
    // If SSL support is not available, check if SSL is enabled
    if (_config.enable_ssl) {
        throw kythira::ssl_configuration_error(
            "SSL enabled but OpenSSL support not available");
    }
    
    // Check if any SSL configuration is provided
    if (!_config.ssl_cert_path.empty() || !_config.ssl_key_path.empty() || 
        !_config.ca_cert_path.empty() || _config.require_client_cert ||
        !_config.cipher_suites.empty()) {
        throw kythira::ssl_configuration_error(
            "SSL configuration provided but OpenSSL support not available");
    }
#endif
}

// Load server certificates
template<typename Types>
requires kythira::transport_types<Types>
auto cpp_httplib_server<Types>::load_server_certificates() -> void {
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
    if (!_config.enable_ssl) {
        return; // SSL not enabled
    }
    
    // Validate that we can load the server certificates
    validate_certificate_key_pair(_config.ssl_cert_path, _config.ssl_key_path);
    
    // If client certificate authentication is required, validate CA certificate
    if (_config.require_client_cert && !_config.ca_cert_path.empty()) {
        validate_certificate_file(_config.ca_cert_path);
    }
#endif
}

// Configure SSL for server
template<typename Types>
requires kythira::transport_types<Types>
auto cpp_httplib_server<Types>::configure_ssl_server() -> void {
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
    if (!_config.enable_ssl) {
        return; // SSL not enabled
    }
    
    // Note: cpp-httplib SSL server configuration varies significantly by version
    // and may not support all advanced SSL context parameters directly.
    // 
    // For full SSL context control, this implementation would need to:
    // 1. Create a custom SSL context using OpenSSL directly
    // 2. Configure all SSL parameters (certificates, cipher suites, TLS versions)
    // 3. Use cpp-httplib's SSL context callback mechanism (if available)
    // 
    // The current implementation validates all SSL configuration parameters
    // but notes that some advanced features may require library modifications
    // or a different HTTP library with better SSL context control.
    
    // Validate that all required certificates are available and valid
    if (_config.ssl_cert_path.empty() || _config.ssl_key_path.empty()) {
        throw kythira::ssl_configuration_error(
            "SSL server requires both certificate and private key paths");
    }
    
    // Create and configure SSL context for validation
    SSL_CTX* ctx = SSL_CTX_new(TLS_server_method());
    if (!ctx) {
        throw kythira::ssl_context_error("Failed to create SSL context for server");
    }
    
    try {
        // Configure SSL context with all parameters
        configure_ssl_context(ctx, _config.cipher_suites, 
                             _config.min_tls_version, _config.max_tls_version);
        
        // Load server certificate and key
        if (SSL_CTX_use_certificate_file(ctx, _config.ssl_cert_path.c_str(), SSL_FILETYPE_PEM) != 1) {
            throw kythira::ssl_configuration_error(
                std::format("Failed to load server certificate: {}", _config.ssl_cert_path));
        }
        
        if (SSL_CTX_use_PrivateKey_file(ctx, _config.ssl_key_path.c_str(), SSL_FILETYPE_PEM) != 1) {
            throw kythira::ssl_configuration_error(
                std::format("Failed to load server private key: {}", _config.ssl_key_path));
        }
        
        // Verify that the private key matches the certificate
        if (SSL_CTX_check_private_key(ctx) != 1) {
            throw kythira::certificate_validation_error(
                "Server private key does not match certificate");
        }
        
        // Configure client certificate authentication if required
        if (_config.require_client_cert) {
            if (_config.ca_cert_path.empty()) {
                throw kythira::ssl_configuration_error(
                    "Client certificate authentication requires CA certificate path");
            }
            
            // Load CA certificate for client verification
            if (SSL_CTX_load_verify_locations(ctx, _config.ca_cert_path.c_str(), nullptr) != 1) {
                throw kythira::ssl_configuration_error(
                    std::format("Failed to load CA certificate: {}", _config.ca_cert_path));
            }
            
            // Set verification mode to require client certificates
            SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, nullptr);
        }
        
        SSL_CTX_free(ctx);
        
        // At this point, all SSL configuration has been validated
        // The actual integration with cpp-httplib would depend on the library version
        // and available SSL context configuration mechanisms
        
        throw kythira::ssl_configuration_error(
            "SSL server configuration validated successfully, but cpp-httplib SSL server "
            "integration is not fully implemented. Server certificate: " + _config.ssl_cert_path + 
            ", Server key: " + _config.ssl_key_path +
            (_config.require_client_cert ? ", Client cert required" : "") +
            (!_config.cipher_suites.empty() ? ", Cipher suites: " + _config.cipher_suites : "") +
            ", TLS versions: " + _config.min_tls_version + " to " + _config.max_tls_version);
            
    } catch (...) {
        SSL_CTX_free(ctx);
        throw;
    }
#else
    throw kythira::ssl_configuration_error("SSL support not available (OpenSSL not enabled)");
#endif
}

// Register RequestVote handler
template<typename Types>
requires kythira::transport_types<Types>
auto cpp_httplib_server<Types>::register_request_vote_handler(
    std::function<kythira::request_vote_response<>(const kythira::request_vote_request<>&)> handler
) -> void {
    std::lock_guard<std::mutex> lock(_mutex);
    _request_vote_handler = std::move(handler);
}

// Register AppendEntries handler
template<typename Types>
requires kythira::transport_types<Types>
auto cpp_httplib_server<Types>::register_append_entries_handler(
    std::function<kythira::append_entries_response<>(const kythira::append_entries_request<>&)> handler
) -> void {
    std::lock_guard<std::mutex> lock(_mutex);
    _append_entries_handler = std::move(handler);
}

// Register InstallSnapshot handler
template<typename Types>
requires kythira::transport_types<Types>
auto cpp_httplib_server<Types>::register_install_snapshot_handler(
    std::function<kythira::install_snapshot_response<>(const kythira::install_snapshot_request<>&)> handler
) -> void {
    std::lock_guard<std::mutex> lock(_mutex);
    _install_snapshot_handler = std::move(handler);
}

// Generic RPC endpoint handler
template<typename Types>
requires kythira::transport_types<Types>
template<typename Request, typename Response>
auto cpp_httplib_server<Types>::handle_rpc_endpoint(
    const httplib::Request& http_req,
    httplib::Response& http_resp,
    std::function<Response(const Request&)> handler
) -> void {
    auto start_time = std::chrono::steady_clock::now();
    
    // Determine RPC type for metrics
    std::string rpc_type;
    std::string endpoint = http_req.path;
    if (endpoint == endpoint_request_vote) {
        rpc_type = "request_vote";
    } else if (endpoint == endpoint_append_entries) {
        rpc_type = "append_entries";
    } else if (endpoint == endpoint_install_snapshot) {
        rpc_type = "install_snapshot";
    }
    
    try {
        // Check if handler is registered
        if (!handler) {
            auto error_metric = _metrics;
            error_metric.set_metric_name("http.server.error");
            error_metric.add_dimension("error_type", "handler_not_registered");
            error_metric.add_dimension("endpoint", endpoint);
            error_metric.add_one();
            error_metric.emit();
            
            http_resp.status = 500;
            http_resp.body = "Handler not registered";
            http_resp.set_header(header_content_type, "text/plain");
            // Let cpp-httplib handle Content-Length automatically
            return;
        }
        
        // Record request received metric
        auto metric = _metrics;
        metric.set_metric_name("http.server.request.received");
        metric.add_dimension("rpc_type", rpc_type);
        metric.add_dimension("endpoint", endpoint);
        metric.add_one();
        metric.emit();
        
        // Record request size
        metric = _metrics;
        metric.set_metric_name("http.server.request.size");
        metric.add_dimension("rpc_type", rpc_type);
        metric.add_dimension("endpoint", endpoint);
        metric.add_value(static_cast<double>(http_req.body.size()));
        metric.emit();
        
        // Convert request body to bytes
        std::vector<std::byte> request_data;
        request_data.reserve(http_req.body.size());
        for (char c : http_req.body) {
            request_data.push_back(static_cast<std::byte>(c));
        }
        
        // Deserialize request
        Request request;
        if constexpr (std::is_same_v<Request, kythira::request_vote_request<>>) {
            request = _serializer.deserialize_request_vote_request(request_data);
        } else if constexpr (std::is_same_v<Request, kythira::append_entries_request<>>) {
            request = _serializer.deserialize_append_entries_request(request_data);
        } else if constexpr (std::is_same_v<Request, kythira::install_snapshot_request<>>) {
            request = _serializer.deserialize_install_snapshot_request(request_data);
        }
        
        // Invoke handler
        Response response = handler(request);
        
        // Serialize response
        auto serialized_response = _serializer.serialize(response);
        
        // Convert bytes to string for HTTP body
        std::string response_body;
        response_body.reserve(serialized_response.size());
        for (auto b : serialized_response) {
            response_body.push_back(static_cast<char>(b));
        }
        
        // Set response
        http_resp.status = 200;
        http_resp.body = std::move(response_body);
        http_resp.set_header(header_content_type, content_type_json);
        // Let cpp-httplib handle Content-Length automatically
        
        // Record response size
        metric = _metrics;
        metric.set_metric_name("http.server.response.size");
        metric.add_dimension("rpc_type", rpc_type);
        metric.add_dimension("endpoint", endpoint);
        metric.add_dimension("status_code", "200");
        metric.add_value(static_cast<double>(http_resp.body.size()));
        metric.emit();
        
        // Record success latency
        auto end_time = std::chrono::steady_clock::now();
        auto latency = std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time);
        
        auto latency_metric = _metrics;
        latency_metric.set_metric_name("http.server.request.latency");
        latency_metric.add_dimension("rpc_type", rpc_type);
        latency_metric.add_dimension("endpoint", endpoint);
        latency_metric.add_dimension("status_code", "200");
        latency_metric.add_duration(latency);
        latency_metric.emit();
        
    } catch (const std::exception& e) {
        // Handle errors
        std::string error_type = "deserialization_failed";
        int status_code = 400;
        std::string error_message = std::format("Bad Request: {}", e.what());
        
        // Check if it's a handler exception
        if (http_req.body.empty() == false) {
            try {
                // Try to deserialize to see if it's a deserialization error
                std::vector<std::byte> test_data;
                test_data.reserve(http_req.body.size());
                for (char c : http_req.body) {
                    test_data.push_back(static_cast<std::byte>(c));
                }
                
                if constexpr (std::is_same_v<Request, kythira::request_vote_request<>>) {
                    _serializer.deserialize_request_vote_request(test_data);
                } else if constexpr (std::is_same_v<Request, kythira::append_entries_request<>>) {
                    _serializer.deserialize_append_entries_request(test_data);
                } else if constexpr (std::is_same_v<Request, kythira::install_snapshot_request<>>) {
                    _serializer.deserialize_install_snapshot_request(test_data);
                }
                
                // If we get here, deserialization worked, so it's a handler exception
                error_type = "handler_exception";
                status_code = 500;
                error_message = "Internal Server Error";
            } catch (...) {
                // Deserialization failed, keep original error type
            }
        }
        
        // Record error metric
        auto error_metric = _metrics;
        error_metric.set_metric_name("http.server.error");
        error_metric.add_dimension("error_type", error_type);
        error_metric.add_dimension("endpoint", endpoint);
        error_metric.add_one();
        error_metric.emit();
        
        // Set error response
        http_resp.status = status_code;
        http_resp.body = error_message;
        http_resp.set_header(header_content_type, "text/plain");
        // Let cpp-httplib handle Content-Length automatically
        
        // Record error latency
        auto end_time = std::chrono::steady_clock::now();
        auto latency = std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time);
        
        auto latency_metric = _metrics;
        latency_metric.set_metric_name("http.server.request.latency");
        latency_metric.add_dimension("rpc_type", rpc_type);
        latency_metric.add_dimension("endpoint", endpoint);
        latency_metric.add_dimension("status_code", std::to_string(status_code));
        latency_metric.add_duration(latency);
        latency_metric.emit();
    }
}

// Setup endpoints
template<typename Types>
requires kythira::transport_types<Types>
auto cpp_httplib_server<Types>::setup_endpoints() -> void {
    // RequestVote endpoint
    _http_server->Post(endpoint_request_vote, [this](const httplib::Request& req, httplib::Response& resp) {
        this->handle_rpc_endpoint<kythira::request_vote_request<>, kythira::request_vote_response<>>(
            req, resp, _request_vote_handler);
    });
    
    // AppendEntries endpoint
    _http_server->Post(endpoint_append_entries, [this](const httplib::Request& req, httplib::Response& resp) {
        this->handle_rpc_endpoint<kythira::append_entries_request<>, kythira::append_entries_response<>>(
            req, resp, _append_entries_handler);
    });
    
    // InstallSnapshot endpoint
    _http_server->Post(endpoint_install_snapshot, [this](const httplib::Request& req, httplib::Response& resp) {
        this->handle_rpc_endpoint<kythira::install_snapshot_request<>, kythira::install_snapshot_response<>>(
            req, resp, _install_snapshot_handler);
    });
}

// Start server
template<typename Types>
requires kythira::transport_types<Types>
auto cpp_httplib_server<Types>::start() -> void {
    std::lock_guard<std::mutex> lock(_mutex);
    
    if (_running.load()) {
        return; // Already running
    }
    
    // Setup endpoints
    setup_endpoints();
    
    // Configure SSL if enabled
    if (_config.enable_ssl) {
        try {
            configure_ssl_server();
        } catch (const std::exception& e) {
            throw kythira::ssl_configuration_error(
                std::format("Failed to configure SSL server: {}", e.what()));
        }
    }
    
    // Start server in a separate thread
    _running.store(true);
    
    // Emit server started metric
    auto metric = _metrics;
    metric.set_metric_name("http.server.started");
    metric.add_one();
    metric.emit();
    
    // Start the server in a separate thread
    _server_thread = std::thread([this]() {
        try {
            if (!_http_server->listen(_bind_address.c_str(), _bind_port)) {
                _running.store(false);
            }
        } catch (const std::exception& e) {
            _running.store(false);
        }
    });
    
    // Give the server a moment to start up
    std::this_thread::sleep_for(std::chrono::milliseconds{200});
}

// Stop server
template<typename Types>
requires kythira::transport_types<Types>
auto cpp_httplib_server<Types>::stop() -> void {
    std::lock_guard<std::mutex> lock(_mutex);
    
    if (!_running.load()) {
        return; // Already stopped
    }
    
    // Stop the server
    _http_server->stop();
    _running.store(false);
    
    // Wait for server thread to finish
    if (_server_thread.joinable()) {
        _server_thread.join();
    }
    
    // Emit server stopped metric
    auto metric = _metrics;
    metric.set_metric_name("http.server.stopped");
    metric.add_one();
    metric.emit();
}

// Check if server is running
template<typename Types>
requires kythira::transport_types<Types>
auto cpp_httplib_server<Types>::is_running() const -> bool {
    return _running.load();
}

} // namespace kythira
