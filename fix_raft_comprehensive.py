#!/usr/bin/env python3

import re

def fix_raft_comprehensive():
    """Comprehensive fix for raft.hpp template and namespace issues"""
    
    with open('include/raft/raft.hpp', 'r') as f:
        content = f.read()
    
    # Fix double raft::raft:: namespace qualifiers
    content = re.sub(r'raft::raft::', r'raft::', content)
    
    # Fix method definitions missing FutureType template parameter
    # Pattern: auto node<NetworkClient, NetworkServer, ...>::method_name
    # Should be: auto node<FutureType, NetworkClient, NetworkServer, ...>::method_name
    content = re.sub(
        r'auto node<(NetworkClient, NetworkServer, PersistenceEngine, Logger, Metrics, MembershipManager, NodeId, TermId, LogIndex)>::',
        r'auto node<FutureType, \1>::',
        content
    )
    
    # Fix requires clauses that are missing FutureType
    # Pattern: requires \n    network_client<NetworkClient> &&
    # Should be: requires \n    kythira::network_client<NetworkClient, FutureType> &&
    content = re.sub(
        r'(requires\s*\n\s+)network_client<NetworkClient>\s*&&',
        r'\1kythira::network_client<NetworkClient, FutureType> &&',
        content,
        flags=re.MULTILINE
    )
    
    content = re.sub(
        r'(requires\s*\n\s+)network_server<NetworkServer>\s*&&',
        r'\1kythira::network_server<NetworkServer, FutureType> &&',
        content,
        flags=re.MULTILINE
    )
    
    # Fix persistence_engine references
    content = re.sub(
        r'(\s+)persistence_engine<PersistenceEngine,',
        r'\1raft::persistence_engine<PersistenceEngine,',
        content
    )
    
    # Fix any remaining template constraint issues
    # Look for patterns like: FutureType> && where FutureType is not defined
    # This happens in method definitions that are missing the template parameter
    
    # Fix lambda capture issues by ensuring proper template context
    # This is more complex and may need manual intervention
    
    with open('include/raft/raft.hpp', 'w') as f:
        f.write(content)
    
    print("Applied comprehensive fixes to raft.hpp")

if __name__ == '__main__':
    fix_raft_comprehensive()