/**
 * @file http_transport_ssl_example.cpp
 * @brief Example demonstrating SSL/TLS configuration for HTTP transport
 * 
 * This example shows how to configure SSL/TLS for both client and server
 * components of the HTTP transport, including mutual TLS authentication.
 */

#include <raft/http_transport.hpp>
#include <raft/http_transport_impl.hpp>
#include <raft/json_serializer.hpp>
#include <raft/metrics.hpp>
#include <raft/future.hpp>
#include <folly/executors/CPUThreadPoolExecutor.h>
#include <iostream>
#include <thread>
#include <chrono>

namespace {
    // Example SSL configuration paths
    constexpr const char* server_cert_path = "/path/to/server.crt";
    constexpr const char* server_key_path = "/path/to/server.key";
    constexpr const char* client_cert_path = "/path/to/client.crt";
    constexpr const char* client_key_path = "/path/to/client.key";
    constexpr const char* ca_cert_path = "/path/to/ca.crt";
    
    // Network configuration
    constexpr const char* bind_address = "127.0.0.1";
    constexpr std::uint16_t bind_port = 8443;
    constexpr std::uint64_t node_id = 1;
    constexpr const char* node_url = "https://localhost:8443";
}

using transport_types = kythira::http_transport_types<
    kythira::json_rpc_serializer<std::vector<std::byte>>,
    kythira::noop_metrics,
    folly::CPUThreadPoolExecutor
>;

/**
 * @brief Configure SSL server with comprehensive security settings
 */
auto create_ssl_server_config() -> kythira::cpp_httplib_server_config {
    kythira::cpp_httplib_server_config config;
    
    // Enable SSL/TLS
    config.enable_ssl = true;
    
    // Server certificate and private key
    config.ssl_cert_path = server_cert_path;
    config.ssl_key_path = server_key_path;
    
    // Certificate Authority for client certificate verification
    config.ca_cert_path = ca_cert_path;
    
    // Require client certificates for mutual TLS
    config.require_client_cert = true;
    
    // Restrict cipher suites to secure options only
    config.cipher_suites = "ECDHE-RSA-AES256-GCM-SHA384:ECDHE-RSA-AES128-GCM-SHA256:ECDHE-ECDSA-AES256-GCM-SHA384:ECDHE-ECDSA-AES128-GCM-SHA256";
    
    // Enforce minimum TLS version (TLS 1.2 or higher)
    config.min_tls_version = "TLSv1.2";
    config.max_tls_version = "TLSv1.3";
    
    // Additional security settings
    config.connection_timeout_ms = 30000;
    config.read_timeout_ms = 30000;
    config.write_timeout_ms = 30000;
    
    return config;
}

/**
 * @brief Configure SSL client with comprehensive security settings
 */
auto create_ssl_client_config() -> kythira::cpp_httplib_client_config {
    kythira::cpp_httplib_client_config config;
    
    // Client certificate and private key for mutual TLS
    config.client_cert_path = client_cert_path;
    config.client_key_path = client_key_path;
    
    // Certificate Authority for server certificate verification
    config.ca_cert_path = ca_cert_path;
    
    // Enable SSL certificate verification
    config.enable_ssl_verification = true;
    
    // Restrict cipher suites to secure options only
    config.cipher_suites = "ECDHE-RSA-AES256-GCM-SHA384:ECDHE-RSA-AES128-GCM-SHA256:ECDHE-ECDSA-AES256-GCM-SHA384:ECDHE-ECDSA-AES128-GCM-SHA256";
    
    // Enforce minimum TLS version (TLS 1.2 or higher)
    config.min_tls_version = "TLSv1.2";
    config.max_tls_version = "TLSv1.3";
    
    // Connection timeouts
    config.connection_timeout_ms = 30000;
    config.read_timeout_ms = 30000;
    config.write_timeout_ms = 30000;
    
    return config;
}

/**
 * @brief Example SSL server setup
 */
auto run_ssl_server_example() -> void {
    std::cout << "=== SSL Server Example ===\n";
    
    try {
        // Create server configuration
        auto server_config = create_ssl_server_config();
        
        // Create metrics instance
        transport_types::metrics_type metrics;
        
        // Create SSL-enabled server
        kythira::cpp_httplib_server<transport_types> server(
            bind_address, bind_port, server_config, metrics);
        
        // Register request handlers
        server.register_request_vote_handler(
            [](const kythira::request_vote_request<>& req) {
                std::cout << "Received request_vote from term " << req.term() << std::endl;
                
                kythira::request_vote_response<> response;
                response._term = req.term();
                response._vote_granted = true;
                return response;
            });
        
        server.register_append_entries_handler(
            [](const kythira::append_entries_request<>& req) {
                std::cout << "Received append_entries from term " << req.term() << std::endl;
                
                kythira::append_entries_response<> response;
                response._term = req.term();
                response._success = true;
                return response;
            });
        
        std::cout << "SSL server configured successfully\n";
        std::cout << "Server certificate: " << server_config.ssl_cert_path << "\n";
        std::cout << "CA certificate: " << server_config.ca_cert_path << "\n";
        std::cout << "Client certificates required: " << (server_config.require_client_cert ? "Yes" : "No") << "\n";
        std::cout << "TLS version range: " << server_config.min_tls_version << " - " << server_config.max_tls_version << "\n";
        std::cout << "Cipher suites: " << server_config.cipher_suites << "\n";
        
        // Note: In a real application, you would call server.start() here
        // For this example, we just validate the configuration
        
    } catch (const kythira::ssl_configuration_error& e) {
        std::cerr << "SSL configuration error: " << e.what() << std::endl;
        std::cerr << "\nTroubleshooting tips:\n";
        std::cerr << "1. Verify certificate files exist and are readable\n";
        std::cerr << "2. Check certificate format (PEM expected)\n";
        std::cerr << "3. Ensure OpenSSL is available and properly linked\n";
        std::cerr << "4. Verify certificate and key match\n";
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
    }
}
/**
 * @brief Example SSL client setup
 */
auto run_ssl_client_example() -> void {
    std::cout << "\n=== SSL Client Example ===\n";
    
    try {
        // Create client configuration
        auto client_config = create_ssl_client_config();
        
        // Create node map
        std::unordered_map<std::uint64_t, std::string> node_map;
        node_map[node_id] = node_url;
        
        // Create metrics instance
        transport_types::metrics_type metrics;
        
        // Create SSL-enabled client
        kythira::cpp_httplib_client<transport_types> client(
            std::move(node_map), client_config, metrics);
        
        std::cout << "SSL client configured successfully\n";
        std::cout << "Client certificate: " << client_config.client_cert_path << "\n";
        std::cout << "CA certificate: " << client_config.ca_cert_path << "\n";
        std::cout << "SSL verification enabled: " << (client_config.enable_ssl_verification ? "Yes" : "No") << "\n";
        std::cout << "TLS version range: " << client_config.min_tls_version << " - " << client_config.max_tls_version << "\n";
        std::cout << "Cipher suites: " << client_config.cipher_suites << "\n";
        
        // Example: Send a request_vote message
        kythira::request_vote_request<> request;
        request._term = 1;
        request._candidate_id = node_id;
        request._last_log_index = 0;
        request._last_log_term = 0;
        
        std::cout << "\nExample request_vote call (would be sent over SSL):\n";
        std::cout << "Term: " << request.term() << "\n";
        std::cout << "Candidate ID: " << request.candidate_id() << "\n";
        
        // Note: In a real application, you would call:
        // auto future = client.request_vote(node_id, request);
        // auto response = future.get();
        
    } catch (const kythira::ssl_configuration_error& e) {
        std::cerr << "SSL configuration error: " << e.what() << std::endl;
        std::cerr << "\nTroubleshooting tips:\n";
        std::cerr << "1. Verify certificate files exist and are readable\n";
        std::cerr << "2. Check certificate format (PEM expected)\n";
        std::cerr << "3. Ensure OpenSSL is available and properly linked\n";
        std::cerr << "4. Verify certificate and key match\n";
        std::cerr << "5. Check that CA certificate can validate server certificate\n";
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
    }
}
/**
 * @brief Example of SSL configuration without mutual TLS
 */
auto run_ssl_server_only_example() -> void {
    std::cout << "\n=== SSL Server-Only Example (No Client Certificates) ===\n";
    
    try {
        // Create server configuration without client certificate requirement
        kythira::cpp_httplib_server_config config;
        config.enable_ssl = true;
        config.ssl_cert_path = server_cert_path;
        config.ssl_key_path = server_key_path;
        config.require_client_cert = false; // No mutual TLS
        config.cipher_suites = "ECDHE-RSA-AES256-GCM-SHA384:ECDHE-RSA-AES128-GCM-SHA256";
        config.min_tls_version = "TLSv1.2";
        config.max_tls_version = "TLSv1.3";
        
        transport_types::metrics_type metrics;
        
        kythira::cpp_httplib_server<transport_types> server(
            bind_address, bind_port, config, metrics);
        
        std::cout << "SSL server (no client certs) configured successfully\n";
        std::cout << "Mutual TLS: Disabled\n";
        
        // Corresponding client configuration
        kythira::cpp_httplib_client_config client_config;
        client_config.ca_cert_path = ca_cert_path;
        client_config.enable_ssl_verification = true;
        // No client certificate needed
        client_config.cipher_suites = "ECDHE-RSA-AES256-GCM-SHA384:ECDHE-RSA-AES128-GCM-SHA256";
        client_config.min_tls_version = "TLSv1.2";
        client_config.max_tls_version = "TLSv1.3";
        
        std::unordered_map<std::uint64_t, std::string> node_map;
        node_map[node_id] = node_url;
        
        kythira::cpp_httplib_client<transport_types> client(
            std::move(node_map), client_config, metrics);
        
        std::cout << "SSL client (no client cert) configured successfully\n";
        
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
    }
}
/**
 * @brief Display SSL configuration best practices
 */
auto display_ssl_best_practices() -> void {
    std::cout << "\n=== SSL/TLS Configuration Best Practices ===\n";
    std::cout << "\n1. Certificate Management:\n";
    std::cout << "   - Use certificates from a trusted CA\n";
    std::cout << "   - Regularly rotate certificates before expiration\n";
    std::cout << "   - Store private keys securely with restricted permissions\n";
    std::cout << "   - Use separate certificates for different environments\n";
    
    std::cout << "\n2. Cipher Suite Selection:\n";
    std::cout << "   - Prefer ECDHE for forward secrecy\n";
    std::cout << "   - Use AES-GCM for authenticated encryption\n";
    std::cout << "   - Avoid deprecated ciphers (RC4, DES, MD5)\n";
    std::cout << "   - Order cipher suites by preference\n";
    
    std::cout << "\n3. TLS Version Policy:\n";
    std::cout << "   - Minimum TLS 1.2 for production\n";
    std::cout << "   - Prefer TLS 1.3 when available\n";
    std::cout << "   - Disable older protocols (SSLv3, TLS 1.0, TLS 1.1)\n";
    
    std::cout << "\n4. Mutual TLS (mTLS):\n";
    std::cout << "   - Use for high-security environments\n";
    std::cout << "   - Implement proper certificate validation\n";
    std::cout << "   - Consider certificate revocation checking\n";
    std::cout << "   - Plan for certificate lifecycle management\n";
    
    std::cout << "\n5. Monitoring and Logging:\n";
    std::cout << "   - Log SSL handshake failures\n";
    std::cout << "   - Monitor certificate expiration dates\n";
    std::cout << "   - Track cipher suite usage\n";
    std::cout << "   - Alert on security policy violations\n";
}

int main() {
    std::cout << "HTTP Transport SSL/TLS Configuration Example\n";
    std::cout << "==========================================\n";
    
    // Display configuration paths (update these for your environment)
    std::cout << "\nCertificate paths (update these for your environment):\n";
    std::cout << "Server certificate: " << server_cert_path << "\n";
    std::cout << "Server private key: " << server_key_path << "\n";
    std::cout << "Client certificate: " << client_cert_path << "\n";
    std::cout << "Client private key: " << client_key_path << "\n";
    std::cout << "CA certificate: " << ca_cert_path << "\n";
    
    // Run examples
    run_ssl_server_example();
    run_ssl_client_example();
    run_ssl_server_only_example();
    display_ssl_best_practices();
    
    std::cout << "\n=== Example Complete ===\n";
    std::cout << "Note: This example validates SSL configuration only.\n";
    std::cout << "To run actual SSL communication, ensure certificate files exist\n";
    std::cout << "and call server.start() and client methods as needed.\n";
    
    return 0;
}