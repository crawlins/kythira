#!/usr/bin/env python3

import re

def fix_raft_template_issues():
    """Fix template issues in raft.hpp"""
    
    with open('include/raft/raft.hpp', 'r') as f:
        content = f.read()
    
    # Fix network_client and network_server template arguments
    # Pattern 1: network_client<NetworkClient> && -> kythira::network_client<NetworkClient, FutureType> &&
    content = re.sub(
        r'(\s+)network_client<NetworkClient>\s*&&',
        r'\1kythira::network_client<NetworkClient, FutureType> &&',
        content
    )
    
    # Pattern 2: network_server<NetworkServer> && -> kythira::network_server<NetworkServer, FutureType> &&
    content = re.sub(
        r'(\s+)network_server<NetworkServer>\s*&&',
        r'\1kythira::network_server<NetworkServer, FutureType> &&',
        content
    )
    
    # Fix persistence_engine references that are missing raft:: namespace
    content = re.sub(
        r'(\s+)persistence_engine<PersistenceEngine,',
        r'\1raft::persistence_engine<PersistenceEngine,',
        content
    )
    
    # Fix log_entry references that are missing raft:: namespace
    content = re.sub(
        r'log_entry<TermId, LogIndex>',
        r'raft::log_entry<TermId, LogIndex>',
        content
    )
    
    # Fix snapshot references that are missing raft:: namespace
    content = re.sub(
        r'snapshot<NodeId, TermId, LogIndex>',
        r'raft::snapshot<NodeId, TermId, LogIndex>',
        content
    )
    
    # Fix any remaining network_client without kythira:: namespace
    content = re.sub(
        r'(\s+)network_client<NetworkClient, FutureType>',
        r'\1kythira::network_client<NetworkClient, FutureType>',
        content
    )
    
    # Fix any remaining network_server without kythira:: namespace  
    content = re.sub(
        r'(\s+)network_server<NetworkServer, FutureType>',
        r'\1kythira::network_server<NetworkServer, FutureType>',
        content
    )
    
    # Fix raft::network_server -> kythira::network_server
    content = re.sub(
        r'raft::network_server<NetworkServer>',
        r'kythira::network_server<NetworkServer, FutureType>',
        content
    )
    
    with open('include/raft/raft.hpp', 'w') as f:
        f.write(content)
    
    print("Fixed template issues in raft.hpp")

if __name__ == '__main__':
    fix_raft_template_issues()