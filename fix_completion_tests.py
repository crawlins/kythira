#!/usr/bin/env python3

import re
import glob

def fix_completion_tests():
    """Fix template parameter issues in completion component tests"""
    
    # Find all completion test files
    test_files = glob.glob('tests/raft_*_completion_property_test.cpp') + \
                 glob.glob('tests/raft_*_collection_property_test.cpp') + \
                 glob.glob('tests/raft_*_retry_*_property_test.cpp') + \
                 glob.glob('tests/raft_*_linearizability_*_property_test.cpp') + \
                 glob.glob('tests/raft_*_read_*_property_test.cpp') + \
                 glob.glob('tests/raft_*_concurrent_*_property_test.cpp')
    
    for test_file in test_files:
        print(f"Fixing {test_file}")
        
        with open(test_file, 'r') as f:
            content = f.read()
        
        # Fix NetworkSimulator template parameters
        content = re.sub(
            r'network_simulator::NetworkSimulator<std::uint64_t, std::uint16_t>',
            r'kythira::NetworkSimulator<std::uint64_t, std::uint16_t, kythira::Future<network_simulator::Message<std::uint64_t, std::uint16_t>>>',
            content
        )
        
        # Fix simulator_network_client template parameters
        content = re.sub(
            r'kythira::simulator_network_client<\s*raft::json_rpc_serializer<std::vector<std::byte>>,\s*std::vector<std::byte>\s*>',
            r'kythira::simulator_network_client<kythira::Future<std::vector<std::byte>>, raft::json_rpc_serializer<std::vector<std::byte>>, std::vector<std::byte>>',
            content,
            flags=re.MULTILINE | re.DOTALL
        )
        
        # Fix simulator_network_server template parameters
        content = re.sub(
            r'kythira::simulator_network_server<\s*raft::json_rpc_serializer<std::vector<std::byte>>,\s*std::vector<std::byte>\s*>',
            r'kythira::simulator_network_server<kythira::Future<std::vector<std::byte>>, raft::json_rpc_serializer<std::vector<std::byte>>, std::vector<std::byte>>',
            content,
            flags=re.MULTILINE | re.DOTALL
        )
        
        with open(test_file, 'w') as f:
            f.write(content)
    
    print(f"Fixed {len(test_files)} completion test files")

if __name__ == '__main__':
    fix_completion_tests()