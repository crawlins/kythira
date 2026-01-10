#!/usr/bin/env python3

import re

def fix_raft_namespaces():
    """Fix missing raft:: namespace prefixes in template constraints"""
    
    with open('include/raft/raft.hpp', 'r') as f:
        content = f.read()
    
    # Fix persistence_engine constraints
    content = re.sub(
        r'^(\s+)persistence_engine<([^>]+), log_entry<([^>]+)>, snapshot<([^>]+)>>',
        r'\1raft::persistence_engine<\2, raft::log_entry<\3>, raft::snapshot<\4>>',
        content,
        flags=re.MULTILINE
    )
    
    # Fix diagnostic_logger constraints
    content = re.sub(
        r'^(\s+)diagnostic_logger<',
        r'\1raft::diagnostic_logger<',
        content,
        flags=re.MULTILINE
    )
    
    # Fix metrics constraints
    content = re.sub(
        r'^(\s+)metrics<',
        r'\1raft::metrics<',
        content,
        flags=re.MULTILINE
    )
    
    # Fix membership_manager constraints
    content = re.sub(
        r'^(\s+)membership_manager<([^>]+), cluster_configuration<([^>]+)>>',
        r'\1raft::membership_manager<\2, raft::cluster_configuration<\3>>',
        content,
        flags=re.MULTILINE
    )
    
    # Fix node_id constraints
    content = re.sub(
        r'^(\s+)node_id<',
        r'\1raft::node_id<',
        content,
        flags=re.MULTILINE
    )
    
    # Fix term_id constraints
    content = re.sub(
        r'^(\s+)term_id<',
        r'\1raft::term_id<',
        content,
        flags=re.MULTILINE
    )
    
    # Fix log_index constraints
    content = re.sub(
        r'^(\s+)log_index<',
        r'\1raft::log_index<',
        content,
        flags=re.MULTILINE
    )
    
    with open('include/raft/raft.hpp', 'w') as f:
        f.write(content)
    
    print("Fixed namespace issues in raft.hpp")

if __name__ == '__main__':
    fix_raft_namespaces()