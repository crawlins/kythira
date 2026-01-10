#!/usr/bin/env python3

import re

def fix_all_raft_methods():
    """Fix all method template declarations in raft.hpp to include FutureType"""
    
    with open('include/raft/raft.hpp', 'r') as f:
        content = f.read()
    
    # Pattern to match method implementations
    # Look for template declarations followed by auto node<...>::method_name
    pattern = r'template<([^>]+)>\s*requires[^{]*?auto\s+node<([^>]+)>::([^(]+)\('
    
    def fix_template_and_node(match):
        template_params = match.group(1).strip()
        node_params = match.group(2).strip()
        method_name = match.group(3).strip()
        full_match = match.group(0)
        
        # Skip if FutureType is already present
        if 'FutureType' in template_params:
            return full_match
        
        # Add FutureType as first template parameter
        new_template_params = f'typename FutureType,\n    {template_params}'
        
        # Add FutureType as first node parameter
        new_node_params = f'FutureType, {node_params}'
        
        # Replace both template and node parameters
        result = full_match.replace(f'template<{template_params}>', f'template<\n    {new_template_params}\n>')
        result = result.replace(f'node<{node_params}>', f'node<{new_node_params}>')
        
        return result
    
    # Apply the fix
    content = re.sub(pattern, fix_template_and_node, content, flags=re.MULTILINE | re.DOTALL)
    
    with open('include/raft/raft.hpp', 'w') as f:
        f.write(content)
    
    print("Fixed all method template declarations in raft.hpp")

if __name__ == '__main__':
    fix_all_raft_methods()