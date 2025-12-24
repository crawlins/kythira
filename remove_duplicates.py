#!/usr/bin/env python3

import re

# Read the file
with open('include/raft/coap_transport_impl.hpp', 'r') as f:
    lines = f.readlines()

# Define the duplicate line ranges to remove (second occurrences)
# Format: (start_line, method_name) - we'll find the end by looking for the next method or end of file
duplicates_to_remove = [
    (1846, 'should_use_block_transfer'),  # server duplicate
    (1852, 'split_payload_into_blocks'),  # server duplicate  
    (1917, 'cleanup_expired_block_transfers'),  # server duplicate
    (1934, 'detect_malformed_message'),  # server duplicate
    (2005, 'handle_resource_exhaustion'),  # server duplicate
    (2094, 'get_or_create_session'),  # client duplicate
    (2171, 'return_session_to_pool'),  # client duplicate
    (2202, 'cleanup_expired_sessions'),  # client duplicate
    (2266, 'allocate_from_pool'),  # client duplicate
    (2282, 'get_cached_serialization'),  # client duplicate
    (2310, 'cache_serialization'),  # client duplicate
    (2334, 'cleanup_serialization_cache'),  # client duplicate
    (2668, 'setup_multicast_listener'),  # server duplicate
]

# Sort by line number in reverse order so we can remove from end to beginning
duplicates_to_remove.sort(key=lambda x: x[0], reverse=True)

# Remove each duplicate method
for start_line, method_name in duplicates_to_remove:
    # Convert to 0-based indexing
    start_idx = start_line - 1
    
    # Find the end of this method by looking for the next template or end of file
    end_idx = len(lines)
    for i in range(start_idx + 1, len(lines)):
        line = lines[i].strip()
        if line.startswith('template<') or line.startswith('} // namespace'):
            end_idx = i
            break
    
    print(f"Removing duplicate {method_name} from lines {start_line} to {end_idx}")
    
    # Remove the lines
    del lines[start_idx:end_idx]

# Write the cleaned file
with open('include/raft/coap_transport_impl.hpp', 'w') as f:
    f.writelines(lines)

print("Duplicate removal complete")
