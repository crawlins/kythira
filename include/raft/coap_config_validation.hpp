#pragma once

#include <raft/coap_transport.hpp>
#include <raft/coap_exceptions.hpp>
#include <raft/coap_utils.hpp>

namespace kythira {
namespace coap_utils {

// Client configuration validation
inline auto validate_client_config(const coap_client_config& config) -> void {
    // Validate timeout values
    if (config.ack_timeout.count() <= 0) {
        throw coap_transport_error("ack_timeout must be positive");
    }
    
    if (config.session_timeout.count() <= 0) {
        throw coap_transport_error("session_timeout must be positive");
    }
    
    if (config.retransmission_timeout.count() <= 0) {
        throw coap_transport_error("retransmission_timeout must be positive");
    }
    
    // Validate retransmission parameters
    if (config.max_retransmit == 0) {
        throw coap_transport_error("max_retransmit must be greater than 0");
    }
    
    if (config.max_retransmit > 20) {
        throw coap_transport_error("max_retransmit must not exceed 20");
    }
    
    if (config.max_retransmissions > 20) {
        throw coap_transport_error("max_retransmissions must not exceed 20");
    }
    
    // Validate session limits
    if (config.max_sessions == 0) {
        throw coap_transport_error("max_sessions must be greater than 0");
    }
    
    // Validate exponential backoff factor
    if (config.exponential_backoff_factor < 1.0) {
        throw coap_transport_error("exponential_backoff_factor must be >= 1.0");
    }
    
    if (config.exponential_backoff_factor > 10.0) {
        throw coap_transport_error("exponential_backoff_factor must be <= 10.0");
    }
    
    // Validate block transfer settings
    if (config.enable_block_transfer) {
        if (config.max_block_size < 16) {
            throw coap_transport_error("max_block_size must be at least 16 bytes");
        }
        
        if (config.max_block_size > 1024) {
            throw coap_transport_error("max_block_size must not exceed 1024 bytes");
        }
        
        if (!is_valid_block_size(config.max_block_size)) {
            throw coap_transport_error("max_block_size must be a power of 2 between 16 and 1024");
        }
    }
    
    // Validate DTLS configuration
    if (config.enable_dtls) {
        bool has_cert_auth = !config.cert_file.empty() && !config.key_file.empty();
        bool has_psk_auth = !config.psk_identity.empty() && !config.psk_key.empty();
        
        if (!has_cert_auth && !has_psk_auth) {
            throw coap_security_error("DTLS enabled but no authentication method configured (need certificate or PSK)");
        }
        
        if (has_cert_auth && has_psk_auth) {
            throw coap_security_error("Cannot use both certificate and PSK authentication simultaneously");
        }
        
        if (has_psk_auth) {
            if (config.psk_key.size() < 4) {
                throw coap_security_error("PSK key must be at least 4 bytes");
            }
            
            if (config.psk_key.size() > 64) {
                throw coap_security_error("PSK key must not exceed 64 bytes");
            }
            
            if (config.psk_identity.size() > 128) {
                throw coap_security_error("PSK identity must not exceed 128 characters");
            }
        }
    }
    
    // Validate multicast configuration
    if (config.enable_multicast) {
        if (config.multicast_address.empty()) {
            throw coap_transport_error("multicast_address cannot be empty when multicast is enabled");
        }
        
        if (config.multicast_port == 0) {
            throw coap_transport_error("multicast_port must be greater than 0");
        }
    }
    
    // Validate performance optimization settings
    if (config.enable_memory_optimization) {
        if (config.memory_pool_size == 0) {
            throw coap_transport_error("memory_pool_size must be greater than 0 when memory optimization is enabled");
        }
        
        if (config.memory_pool_block_size == 0) {
            throw coap_transport_error("memory_pool_block_size must be greater than 0 when memory optimization is enabled");
        }
    }
    
    if (config.enable_serialization_caching) {
        if (config.serialization_cache_size == 0) {
            throw coap_transport_error("serialization_cache_size must be greater than 0 when caching is enabled");
        }
    }
}

// Server configuration validation
inline auto validate_server_config(const coap_server_config& config) -> void {
    // Validate session limits
    if (config.max_concurrent_sessions == 0) {
        throw coap_transport_error("max_concurrent_sessions must be greater than 0");
    }
    
    // Validate request size limits
    if (config.max_request_size == 0) {
        throw coap_transport_error("max_request_size must be greater than 0");
    }
    
    if (config.max_request_size > 100 * 1024 * 1024) { // 100 MB
        throw coap_transport_error("max_request_size must not exceed 100 MB");
    }
    
    // Validate timeout values
    if (config.session_timeout.count() <= 0) {
        throw coap_transport_error("session_timeout must be positive");
    }
    
    // Validate block transfer settings
    if (config.enable_block_transfer) {
        if (config.max_block_size < 16) {
            throw coap_transport_error("max_block_size must be at least 16 bytes");
        }
        
        if (config.max_block_size > 1024) {
            throw coap_transport_error("max_block_size must not exceed 1024 bytes");
        }
        
        if (!is_valid_block_size(config.max_block_size)) {
            throw coap_transport_error("max_block_size must be a power of 2 between 16 and 1024");
        }
    }
    
    // Validate DTLS configuration
    if (config.enable_dtls) {
        bool has_cert_auth = !config.cert_file.empty() && !config.key_file.empty();
        bool has_psk_auth = !config.psk_identity.empty() && !config.psk_key.empty();
        
        if (!has_cert_auth && !has_psk_auth) {
            throw coap_security_error("DTLS enabled but no authentication method configured (need certificate or PSK)");
        }
        
        if (has_cert_auth && has_psk_auth) {
            throw coap_security_error("Cannot use both certificate and PSK authentication simultaneously");
        }
        
        if (has_psk_auth) {
            if (config.psk_key.size() < 4) {
                throw coap_security_error("PSK key must be at least 4 bytes");
            }
            
            if (config.psk_key.size() > 64) {
                throw coap_security_error("PSK key must not exceed 64 bytes");
            }
            
            if (config.psk_identity.size() > 128) {
                throw coap_security_error("PSK identity must not exceed 128 characters");
            }
        }
    }
    
    // Validate multicast configuration
    if (config.enable_multicast) {
        if (config.multicast_address.empty()) {
            throw coap_transport_error("multicast_address cannot be empty when multicast is enabled");
        }
        
        if (config.multicast_port == 0) {
            throw coap_transport_error("multicast_port must be greater than 0");
        }
        
        // Validate multicast address format
        const auto& addr = config.multicast_address;
        
        // Check for IPv4 multicast (224.0.0.0 - 239.255.255.255)
        bool is_ipv4_multicast = addr.find("224.") == 0 || addr.find("225.") == 0 || 
                                 addr.find("226.") == 0 || addr.find("227.") == 0 ||
                                 addr.find("228.") == 0 || addr.find("229.") == 0 ||
                                 addr.find("230.") == 0 || addr.find("231.") == 0 ||
                                 addr.find("232.") == 0 || addr.find("233.") == 0 ||
                                 addr.find("234.") == 0 || addr.find("235.") == 0 ||
                                 addr.find("236.") == 0 || addr.find("237.") == 0 ||
                                 addr.find("238.") == 0 || addr.find("239.") == 0;
        
        // Check for IPv6 multicast (ff00::/8)
        bool is_ipv6_multicast = addr.find("ff") == 0 || addr.find("FF") == 0;
        
        if (!is_ipv4_multicast && !is_ipv6_multicast) {
            throw coap_transport_error("multicast_address must be a valid multicast address (IPv4: 224.0.0.0-239.255.255.255, IPv6: ff00::/8)");
        }
    }
    
    // Validate performance optimization settings
    if (config.enable_memory_optimization) {
        if (config.memory_pool_size == 0) {
            throw coap_transport_error("memory_pool_size must be greater than 0 when memory optimization is enabled");
        }
        
        if (config.memory_pool_block_size == 0) {
            throw coap_transport_error("memory_pool_block_size must be greater than 0 when memory optimization is enabled");
        }
    }
    
    if (config.enable_serialization_caching) {
        if (config.serialization_cache_size == 0) {
            throw coap_transport_error("serialization_cache_size must be greater than 0 when caching is enabled");
        }
    }
}

} // namespace coap_utils
} // namespace kythira
