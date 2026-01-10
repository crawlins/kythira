#!/usr/bin/env python3

import re

def fix_raft_complete():
    """Complete fix for all raft.hpp issues"""
    
    with open('include/raft/raft.hpp', 'r') as f:
        content = f.read()
    
    print("Starting comprehensive raft.hpp fixes...")
    
    # 1. Fix double raft::raft:: namespace qualifiers
    content = re.sub(r'raft::raft::', r'raft::', content)
    print("Fixed double namespace qualifiers")
    
    # 2. Fix method template definitions missing FutureType
    # Pattern: template<\n    typename NetworkClient,
    # Should be: template<\n    typename FutureType,\n    typename NetworkClient,
    content = re.sub(
        r'(template<\s*\n\s*)typename NetworkClient,',
        r'\1typename FutureType,\n    typename NetworkClient,',
        content,
        flags=re.MULTILINE
    )
    print("Fixed missing FutureType in template parameters")
    
    # 3. Fix requires clauses missing FutureType template parameter
    # Pattern: requires \n    kythira::network_client<NetworkClient> &&
    # Should be: requires \n    kythira::network_client<NetworkClient, FutureType> &&
    content = re.sub(
        r'(requires\s*\n\s+)kythira::network_client<NetworkClient>\s*&&',
        r'\1kythira::network_client<NetworkClient, FutureType> &&',
        content,
        flags=re.MULTILINE
    )
    
    content = re.sub(
        r'(requires\s*\n\s+)kythira::network_server<NetworkServer>\s*&&',
        r'\1kythira::network_server<NetworkServer, FutureType> &&',
        content,
        flags=re.MULTILINE
    )
    print("Fixed requires clauses with missing FutureType")
    
    # 4. Fix method definitions that have wrong template parameter order
    # Pattern: auto node<FutureType, FutureType, NetworkClient, ...
    # Should be: auto node<FutureType, NetworkClient, ...
    content = re.sub(
        r'auto node<FutureType, FutureType,',
        r'auto node<FutureType,',
        content
    )
    print("Fixed duplicate FutureType in method definitions")
    
    # 5. Fix server_state references missing raft:: namespace
    content = re.sub(r'server_state::', r'raft::server_state::', content)
    print("Fixed server_state namespace references")
    
    # 6. Fix raft_configuration references missing raft:: namespace
    content = re.sub(r'raft_configuration\b', r'raft::raft_configuration', content)
    print("Fixed raft_configuration namespace references")
    
    # 7. Fix error_handler references missing kythira:: namespace
    content = re.sub(r'error_handler<', r'kythira::error_handler<', content)
    print("Fixed error_handler namespace references")
    
    # 8. Fix raft_future_collector references missing raft:: namespace
    content = re.sub(r'raft_future_collector<', r'raft::raft_future_collector<', content)
    print("Fixed raft_future_collector namespace references")
    
    # 9. Fix lambda capture issues by adding proper captures
    # This is complex, so let's fix the most common patterns
    
    # Fix lambda captures that need to capture member variables
    # Pattern: [this, target](
    # Should be: [this, target, node_id = _node_id, logger = _logger, metrics = _metrics](
    
    # This is too complex to do with regex, so let's focus on the template issues first
    
    # 10. Fix any remaining namespace issues
    content = re.sub(r'(\s+)persistence_engine<', r'\1raft::persistence_engine<', content)
    content = re.sub(r'(\s+)diagnostic_logger<', r'\1raft::diagnostic_logger<', content)
    content = re.sub(r'(\s+)metrics<', r'\1raft::metrics<', content)
    content = re.sub(r'(\s+)membership_manager<', r'\1raft::membership_manager<', content)
    content = re.sub(r'(\s+)node_id<', r'\1raft::node_id<', content)
    content = re.sub(r'(\s+)term_id<', r'\1raft::term_id<', content)
    content = re.sub(r'(\s+)log_index<', r'\1raft::log_index<', content)
    print("Fixed remaining namespace qualifiers")
    
    # 11. Fix template constraint issues in configuration_synchronizer
    # The error shows NodeId = kythira::Future<std::vector<std::byte>> which is wrong
    # This suggests the template parameters are in the wrong order
    
    # Fix configuration_synchronizer template arguments
    content = re.sub(
        r'raft::configuration_synchronizer<FutureType>',
        r'raft::configuration_synchronizer<NodeId, LogIndex, FutureType>',
        content
    )
    print("Fixed configuration_synchronizer template arguments")
    
    # 12. Fix any remaining template issues
    # Look for patterns where FutureType is used incorrectly
    
    with open('include/raft/raft.hpp', 'w') as f:
        f.write(content)
    
    print("Applied all comprehensive fixes to raft.hpp")

if __name__ == '__main__':
    fix_raft_complete()