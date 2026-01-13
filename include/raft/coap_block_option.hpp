#pragma once

#include <cstdint>

namespace kythira {

// Block option parsing structure for CoAP block-wise transfer
// Based on RFC 7959 - Block-Wise Transfers in the Constrained Application Protocol (CoAP)
struct block_option {
    std::uint32_t block_number{0};  // Block number (0-based)
    bool more_blocks{false};        // More blocks flag (M bit)
    std::uint32_t block_size{16};   // Block size (must be power of 2: 16, 32, 64, 128, 256, 512, 1024)
    
    // Parse Block1/Block2 option value
    static auto parse(std::uint32_t option_value) -> block_option {
        block_option result;
        
        // CoAP Block option format (RFC 7959):
        // 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
        // +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
        // |          Block Number         |M|   SZX   |
        // +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
        
        result.block_number = (option_value >> 4) & 0xFFFFF;  // Block number (20 bits)
        result.more_blocks = ((option_value >> 3) & 0x1) != 0; // More flag (1 bit)
        std::uint32_t szx = option_value & 0x7;               // SZX (3 bits)
        
        // Convert SZX to block size: block_size = 16 * 2^szx
        result.block_size = 16 << szx;
        
        return result;
    }
    
    // Encode Block1/Block2 option value
    auto encode() const -> std::uint32_t {
        // Convert block size to SZX (Size Exponent)
        std::uint32_t szx = 0;
        std::uint32_t size = block_size;
        while (size > 16 && szx < 7) {
            size >>= 1;
            szx++;
        }
        
        // Encode: NUM (20 bits) | M (1 bit) | SZX (3 bits)
        std::uint32_t result = 0;
        result |= (block_number & 0xFFFFF) << 4;  // Block number (20 bits)
        result |= (more_blocks ? 1 : 0) << 3;     // More flag (1 bit)
        result |= (szx & 0x7);                    // SZX (3 bits)
        
        return result;
    }
};

} // namespace kythira