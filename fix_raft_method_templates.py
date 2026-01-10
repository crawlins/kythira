#!/usr/bin/env python3

import re

def fix_raft_method_templates():
    """Fix missing FutureType template parameter in raft.hpp method implementations"""
    
    with open('include/raft/raft.hpp', 'r') as f:
        content = f.read()
    
    # Pattern to match method template declarations that are missing FutureType
    # Look for template declarations followed by requires clauses that have network_client/server constraints
    pattern = r'(template<\s*(?:typename\s+)?(?!.*FutureType)([^>]+)>\s*requires[^{]*?kythira::network_client<[^>]+, FutureType>[^{]*?auto\s+node<[^>]+>::[^(]+\([^)]*\)[^{]*?{)'
    
    def add_future_type(match):
        template_params = match.group(2).strip()
        full_match = match.group(1)
        
        # Add FutureType as the first template parameter
        if template_params:
            new_template = f'template<\n    typename FutureType,\n    {template_params}\n>'
        else:
            new_template = 'template<typename FutureType>'
        
        # Replace the template declaration
        return full_match.replace(f'template<{template_params}>', new_template)
    
    # This is a complex fix, so let's do it more systematically
    # Find all method implementations that need FutureType added
    
    # Pattern to find template method declarations
    method_pattern = r'template<([^>]+)>\s*requires[^{]*?auto\s+node<([^>]+)>::([^(]+)\([^)]*\)[^{]*?{'
    
    def fix_method_template(match):
        template_params = match.group(1).strip()
        node_params = match.group(2).strip()
        method_name = match.group(3).strip()
        full_match = match.group(0)
        
        # Check if FutureType is already in template parameters
        if 'FutureType' in template_params:
            return full_match
        
        # Check if this method needs FutureType (has network constraints)
        if 'kythira::network_client' in full_match or 'kythira::network_server' in full_match:
            # Add FutureType as first parameter
            if template_params:
                new_template_params = f'typename FutureType,\n    {template_params}'
            else:
                new_template_params = 'typename FutureType'
            
            return full_match.replace(f'template<{template_params}>', f'template<\n    {new_template_params}\n>')
        
        return full_match
    
    # Apply the fix
    content = re.sub(method_pattern, fix_method_template, content, flags=re.MULTILINE | re.DOTALL)
    
    with open('include/raft/raft.hpp', 'w') as f:
        f.write(content)
    
    print("Fixed method template parameters in raft.hpp")

if __name__ == '__main__':
    fix_raft_method_templates()