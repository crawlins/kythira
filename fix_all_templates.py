#!/usr/bin/env python3

import re

def fix_all_template_declarations():
    with open('include/raft/raft.hpp', 'r') as f:
        content = f.read()
    
    # Pattern to match problematic template declarations
    # This matches templates that start with "typename NetworkClient" and use FutureType in requires
    pattern = r'(template<\s*\n\s*typename NetworkClient,\s*\n.*?\n.*?\n.*?\n.*?\n.*?\n.*?\n.*?\n.*?\n>\s*\nrequires\s*\n\s*kythira::network_client<NetworkClient, FutureType>.*?\n.*?auto node<)(NetworkClient,)'
    
    def replacement(match):
        template_part = match.group(1)
        node_part = match.group(2)
        
        # Add FutureType to template parameters
        new_template = template_part.replace(
            'template<\n    typename NetworkClient,',
            'template<\n    typename FutureType,\n    typename NetworkClient,'
        )
        
        # Add future requirements and fix namespace prefixes
        new_template = re.sub(
            r'requires \n    kythira::network_client<NetworkClient, FutureType>',
            'requires \n    future<FutureType, std::vector<std::byte>> &&\n    future<FutureType, bool> &&\n    kythira::network_client<NetworkClient, FutureType>',
            new_template
        )
        
        # Add raft:: prefixes
        new_template = new_template.replace('persistence_engine<', 'raft::persistence_engine<')
        new_template = new_template.replace('diagnostic_logger<', 'raft::diagnostic_logger<')
        new_template = new_template.replace('metrics<', 'raft::metrics<')
        new_template = new_template.replace('membership_manager<', 'raft::membership_manager<')
        new_template = new_template.replace('node_id<', 'raft::node_id<')
        new_template = new_template.replace('term_id<', 'raft::term_id<')
        new_template = new_template.replace('log_index<', 'raft::log_index<')
        
        # Add FutureType to node template arguments
        new_node_part = 'FutureType, ' + node_part
        
        return new_template + new_node_part
    
    # Apply the replacement multiple times to catch all instances
    fixed_content = content
    for i in range(5):  # Multiple passes to catch all instances
        new_content = re.sub(pattern, replacement, fixed_content, flags=re.MULTILINE | re.DOTALL)
        if new_content == fixed_content:
            break  # No more changes
        fixed_content = new_content
    
    # Write back the fixed content
    with open('include/raft/raft.hpp', 'w') as f:
        f.write(fixed_content)
    
    print("All template declarations fixed")

if __name__ == "__main__":
    fix_all_template_declarations()