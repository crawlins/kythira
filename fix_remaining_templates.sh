#!/bin/bash

# Fix all remaining template declarations that are missing FutureType

# Create a backup
cp include/raft/raft.hpp include/raft/raft.hpp.backup3

# Use sed to fix the template declarations
# This will replace template declarations that start with "typename NetworkClient" 
# and use FutureType in requires clause but don't have FutureType as a parameter

sed -i '
/template</{
    # Read the next several lines to check the pattern
    N;N;N;N;N;N;N;N;N;N;N;N;N;N;N;N;N;N;N;N
    
    # Check if this matches our problematic pattern
    /typename NetworkClient,.*requires.*FutureType.*auto node<NetworkClient/{
        # Replace the template parameter list
        s/template<\n    typename NetworkClient,/template<\n    typename FutureType,\n    typename NetworkClient,/
        
        # Add the future requirements
        s/requires \n    kythira::network_client<NetworkClient, FutureType>/requires \n    future<FutureType, std::vector<std::byte>> \&\&\n    future<FutureType, bool> \&\&\n    kythira::network_client<NetworkClient, FutureType>/
        
        # Add raft:: prefixes
        s/persistence_engine</raft::persistence_engine</g
        s/diagnostic_logger</raft::diagnostic_logger</g
        s/metrics</raft::metrics</g
        s/membership_manager</raft::membership_manager</g
        s/node_id</raft::node_id</g
        s/term_id</raft::term_id</g
        s/log_index</raft::log_index</g
        
        # Fix the node template arguments
        s/auto node<NetworkClient,/auto node<FutureType, NetworkClient,/
    }
}
' include/raft/raft.hpp

echo "Template fixes applied"