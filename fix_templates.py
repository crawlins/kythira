#!/usr/bin/env python3

import re

def fix_template_declarations():
    with open('include/raft/raft.hpp', 'r') as f:
        content = f.read()
    
    # Find all problematic template declarations
    lines = content.split('\n')
    fixed_lines = []
    i = 0
    
    while i < len(lines):
        line = lines[i]
        
        # Check if this is a template declaration that needs fixing
        if line.strip().startswith('template<') and i + 15 < len(lines):
            # Look ahead to see if this template uses FutureType but doesn't have it
            template_block = '\n'.join(lines[i:i+20])
            
            if ('typename NetworkClient,' in template_block and 
                'FutureType' in template_block and 
                'typename FutureType,' not in template_block):
                
                # This template needs fixing
                print(f"Fixing template at line {i+1}")
                
                # Replace the template declaration
                fixed_lines.append('template<')
                fixed_lines.append('    typename FutureType,')
                
                # Skip the original template< line and add the rest with FutureType
                i += 1
                while i < len(lines) and not lines[i].strip().startswith('auto node<'):
                    if 'typename NetworkClient,' in lines[i]:
                        fixed_lines.append('    typename NetworkClient,')
                    elif 'requires' in lines[i]:
                        fixed_lines.append('requires')
                        fixed_lines.append('    future<FutureType, std::vector<std::byte>> &&')
                        fixed_lines.append('    future<FutureType, bool> &&')
                    elif 'kythira::network_client<NetworkClient, FutureType>' in lines[i]:
                        fixed_lines.append('    kythira::network_client<NetworkClient, FutureType> &&')
                    elif 'kythira::network_server<NetworkServer, FutureType>' in lines[i]:
                        fixed_lines.append('    kythira::network_server<NetworkServer, FutureType> &&')
                    elif 'persistence_engine<' in lines[i]:
                        fixed_lines.append(lines[i].replace('persistence_engine<', 'raft::persistence_engine<'))
                    elif 'diagnostic_logger<' in lines[i]:
                        fixed_lines.append(lines[i].replace('diagnostic_logger<', 'raft::diagnostic_logger<'))
                    elif 'metrics<' in lines[i]:
                        fixed_lines.append(lines[i].replace('metrics<', 'raft::metrics<'))
                    elif 'membership_manager<' in lines[i]:
                        fixed_lines.append(lines[i].replace('membership_manager<', 'raft::membership_manager<'))
                    elif 'node_id<' in lines[i]:
                        fixed_lines.append(lines[i].replace('node_id<', 'raft::node_id<'))
                    elif 'term_id<' in lines[i]:
                        fixed_lines.append(lines[i].replace('term_id<', 'raft::term_id<'))
                    elif 'log_index<' in lines[i]:
                        fixed_lines.append(lines[i].replace('log_index<', 'raft::log_index<'))
                    else:
                        fixed_lines.append(lines[i])
                    i += 1
                
                # Fix the auto node< line
                if i < len(lines) and 'auto node<' in lines[i]:
                    fixed_lines.append(lines[i].replace('auto node<NetworkClient,', 'auto node<FutureType, NetworkClient,'))
                    i += 1
            else:
                fixed_lines.append(line)
                i += 1
        else:
            fixed_lines.append(line)
            i += 1
    
    # Write the fixed content back
    with open('include/raft/raft.hpp', 'w') as f:
        f.write('\n'.join(fixed_lines))
    
    print("Template declarations fixed")

if __name__ == "__main__":
    fix_template_declarations()