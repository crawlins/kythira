#define BOOST_TEST_MODULE raft_configuration_concept_test
#include <raft/types.hpp>
#include <boost/test/unit_test.hpp>
#include <chrono>

BOOST_AUTO_TEST_SUITE(raft_configuration_concept_tests)

// Test that the default raft_configuration satisfies the concept
BOOST_AUTO_TEST_CASE(test_default_raft_configuration_satisfies_concept) {
    static_assert(kythira::raft_configuration_type<kythira::raft_configuration>,
                  "Default raft_configuration should satisfy raft_configuration_type concept");
    BOOST_CHECK(true);
}

// Test that default values are reasonable
BOOST_AUTO_TEST_CASE(test_default_raft_configuration_values) {
    kythira::raft_configuration config;
    
    // Test election timeout range
    BOOST_CHECK_EQUAL(config.election_timeout_min().count(), 150);
    BOOST_CHECK_EQUAL(config.election_timeout_max().count(), 300);
    BOOST_CHECK(config.election_timeout_min() < config.election_timeout_max());
    
    // Test heartbeat interval
    BOOST_CHECK_EQUAL(config.heartbeat_interval().count(), 50);
    // Heartbeat should be less than minimum election timeout
    BOOST_CHECK(config.heartbeat_interval() < config.election_timeout_min());
    
    // Test RPC timeout
    BOOST_CHECK_EQUAL(config.rpc_timeout().count(), 100);
    
    // Test batch size
    BOOST_CHECK_EQUAL(config.max_entries_per_append(), 100);
    
    // Test snapshot thresholds
    BOOST_CHECK_EQUAL(config.snapshot_threshold_bytes(), 10'000'000);
    BOOST_CHECK_EQUAL(config.snapshot_chunk_size(), 1'000'000);
    BOOST_CHECK(config.snapshot_chunk_size() < config.snapshot_threshold_bytes());
}

// Test that we can create a custom configuration
BOOST_AUTO_TEST_CASE(test_custom_raft_configuration, * boost::unit_test::timeout(30)) {
    struct custom_raft_configuration {
        std::chrono::milliseconds _election_timeout_min{200};
        std::chrono::milliseconds _election_timeout_max{400};
        std::chrono::milliseconds _heartbeat_interval{75};
        std::chrono::milliseconds _rpc_timeout{150};
        std::chrono::milliseconds _append_entries_timeout{5000};
        std::chrono::milliseconds _request_vote_timeout{2000};
        std::chrono::milliseconds _install_snapshot_timeout{30000};
        std::size_t _max_entries_per_append{50};
        std::size_t _snapshot_threshold_bytes{5'000'000};
        std::size_t _snapshot_chunk_size{500'000};
        
        kythira::retry_policy_config _heartbeat_retry_policy{
            .initial_delay = std::chrono::milliseconds{50},
            .max_delay = std::chrono::milliseconds{1000},
            .backoff_multiplier = 1.5,
            .jitter_factor = 0.1,
            .max_attempts = 3
        };
        
        kythira::retry_policy_config _append_entries_retry_policy{
            .initial_delay = std::chrono::milliseconds{100},
            .max_delay = std::chrono::milliseconds{5000},
            .backoff_multiplier = 2.0,
            .jitter_factor = 0.1,
            .max_attempts = 5
        };
        
        kythira::retry_policy_config _request_vote_retry_policy{
            .initial_delay = std::chrono::milliseconds{100},
            .max_delay = std::chrono::milliseconds{2000},
            .backoff_multiplier = 2.0,
            .jitter_factor = 0.1,
            .max_attempts = 3
        };
        
        kythira::retry_policy_config _install_snapshot_retry_policy{
            .initial_delay = std::chrono::milliseconds{500},
            .max_delay = std::chrono::milliseconds{30000},
            .backoff_multiplier = 2.0,
            .jitter_factor = 0.1,
            .max_attempts = 10
        };
        
        kythira::adaptive_timeout_config _adaptive_timeout_config{
            .enabled = false,
            .min_timeout = std::chrono::milliseconds{50},
            .max_timeout = std::chrono::milliseconds{10000},
            .adaptation_factor = 1.2,
            .sample_window_size = 10
        };
        
        auto election_timeout_min() const -> std::chrono::milliseconds { return _election_timeout_min; }
        auto election_timeout_max() const -> std::chrono::milliseconds { return _election_timeout_max; }
        auto heartbeat_interval() const -> std::chrono::milliseconds { return _heartbeat_interval; }
        auto rpc_timeout() const -> std::chrono::milliseconds { return _rpc_timeout; }
        auto append_entries_timeout() const -> std::chrono::milliseconds { return _append_entries_timeout; }
        auto request_vote_timeout() const -> std::chrono::milliseconds { return _request_vote_timeout; }
        auto install_snapshot_timeout() const -> std::chrono::milliseconds { return _install_snapshot_timeout; }
        auto max_entries_per_append() const -> std::size_t { return _max_entries_per_append; }
        auto snapshot_threshold_bytes() const -> std::size_t { return _snapshot_threshold_bytes; }
        auto snapshot_chunk_size() const -> std::size_t { return _snapshot_chunk_size; }
        auto heartbeat_retry_policy() const -> const kythira::retry_policy_config& { return _heartbeat_retry_policy; }
        auto append_entries_retry_policy() const -> const kythira::retry_policy_config& { return _append_entries_retry_policy; }
        auto request_vote_retry_policy() const -> const kythira::retry_policy_config& { return _request_vote_retry_policy; }
        auto install_snapshot_retry_policy() const -> const kythira::retry_policy_config& { return _install_snapshot_retry_policy; }
        auto get_adaptive_timeout_config() const -> const kythira::adaptive_timeout_config& { return _adaptive_timeout_config; }
        
        auto validate() const -> bool {
            return get_validation_errors().empty();
        }
        
        auto get_validation_errors() const -> std::vector<std::string> {
            std::vector<std::string> errors;
            if (_election_timeout_min <= std::chrono::milliseconds{0}) {
                errors.push_back("election_timeout_min must be positive");
            }
            if (_election_timeout_max <= _election_timeout_min) {
                errors.push_back("election_timeout_max must be greater than election_timeout_min");
            }
            return errors;
        }
    };
    
    static_assert(kythira::raft_configuration_type<custom_raft_configuration>,
                  "Custom configuration should satisfy raft_configuration_type concept");
    
    custom_raft_configuration config;
    BOOST_CHECK_EQUAL(config.election_timeout_min().count(), 200);
    BOOST_CHECK_EQUAL(config.election_timeout_max().count(), 400);
    BOOST_CHECK_EQUAL(config.heartbeat_interval().count(), 75);
    BOOST_CHECK_EQUAL(config.rpc_timeout().count(), 150);
    BOOST_CHECK_EQUAL(config.max_entries_per_append(), 50);
    BOOST_CHECK_EQUAL(config.snapshot_threshold_bytes(), 5'000'000);
    BOOST_CHECK_EQUAL(config.snapshot_chunk_size(), 500'000);
}

// Test that configuration values can be modified
BOOST_AUTO_TEST_CASE(test_modifiable_raft_configuration) {
    kythira::raft_configuration config;
    
    // Modify values
    config._election_timeout_min = std::chrono::milliseconds{250};
    config._election_timeout_max = std::chrono::milliseconds{500};
    config._heartbeat_interval = std::chrono::milliseconds{100};
    config._rpc_timeout = std::chrono::milliseconds{200};
    config._max_entries_per_append = 200;
    config._snapshot_threshold_bytes = 20'000'000;
    config._snapshot_chunk_size = 2'000'000;
    
    // Verify modified values
    BOOST_CHECK_EQUAL(config.election_timeout_min().count(), 250);
    BOOST_CHECK_EQUAL(config.election_timeout_max().count(), 500);
    BOOST_CHECK_EQUAL(config.heartbeat_interval().count(), 100);
    BOOST_CHECK_EQUAL(config.rpc_timeout().count(), 200);
    BOOST_CHECK_EQUAL(config.max_entries_per_append(), 200);
    BOOST_CHECK_EQUAL(config.snapshot_threshold_bytes(), 20'000'000);
    BOOST_CHECK_EQUAL(config.snapshot_chunk_size(), 2'000'000);
}

BOOST_AUTO_TEST_SUITE_END()
