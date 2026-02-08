#pragma once

#include <cstdint>

namespace kythira {

// Block option parsing structure for CoAP block-wise transfer
// Based on RFC 7959 - Block-Wise Transfers in the Constrained Application Protocol (CoAP)
//
// RFC 7959 Section 2.2: Block Option Format
//  0                   1                   2                   3
//  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
// |          NUM (variable)               |M|     SZX (3 bits)    |
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//
// NUM: Block number (4-20 bits, depending on value)
// M: More flag (1 bit) - indicates if more blocks follow
// SZX: Size exponent (3 bits) - block size = 2^(SZX + 4) bytes
struct block_option {
    std::uint32_t block_number{0};  // Block number (0-based)
    bool more_blocks{false};        // More blocks flag (M bit)
    std::uint32_t block_size{16};   // Block size (must be power of 2: 16, 32, 64, 128, 256, 512, 1024)
    
    // Parse Block1/Block2 option value according to RFC 7959
    static auto parse(std::uint32_t option_value) -> block_option {
        block_option result;
        
        // RFC 7959: Extract SZX from lower 3 bits (bits 0-2)
        std::uint32_t szx = option_value & 0x7;
        
        // RFC 7959: Extract More flag from bit 3
        result.more_blocks = ((option_value >> 3) & 0x1) != 0;
        
        // RFC 7959: Extract block number from upper bits (bit 4 and above)
        result.block_number = option_value >> 4;
        
        // Convert SZX to block size: block_size = 2^(SZX + 4)
        result.block_size = 16 << szx;
        
        return result;
    }
    
    // Encode Block1/Block2 option value according to RFC 7959
    auto encode() const -> std::uint32_t {
        // Convert block size to SZX (Size Exponent)
        // SZX = log2(block_size) - 4
        std::uint32_t szx = 0;
        std::uint32_t size = block_size;
        while (size > 16 && szx < 7) {
            size >>= 1;
            szx++;
        }
        
        // RFC 7959: Encode as NUM (upper bits) | M (bit 3) | SZX (bits 0-2)
        std::uint32_t result = 0;
        result |= (szx & 0x7);                    // SZX (bits 0-2)
        result |= (more_blocks ? 1 : 0) << 3;     // More flag (bit 3)
        result |= (block_number << 4);            // Block number (bit 4 and above)
        
        return result;
    }
};

} // namespace kythira