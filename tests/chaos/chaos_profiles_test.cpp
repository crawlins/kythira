#define BOOST_TEST_MODULE ChaosProfilesTest
#include <boost/test/unit_test.hpp>

#include <fiu.h>
#include <fiu-control.h>

#include "chaos_test_types.hpp"
#include "fault_profiles.hpp"

using ChaosFixture = kythira::chaos::chaos_test_fixture;
BOOST_GLOBAL_FIXTURE(ChaosFixture);

BOOST_AUTO_TEST_SUITE(chaos_profiles)

// network_partition_profile: sends throw while profile is active, succeed after destruction.
BOOST_AUTO_TEST_CASE(network_partition_profile_raii, *boost::unit_test::timeout(30)) {
    kythira::chaos::clear_all_faults();

    BOOST_CHECK_EQUAL(fiu_fail("raft/network/send_request_vote"), 0);
    BOOST_CHECK_EQUAL(fiu_fail("raft/network/send_append_entries"), 0);

    {
        kythira::chaos::network_partition_profile profile;
        BOOST_CHECK_NE(fiu_fail("raft/network/send_request_vote"), 0);
        BOOST_CHECK_NE(fiu_fail("raft/network/send_append_entries"), 0);
    }

    // Destructor must have disabled all points.
    BOOST_CHECK_EQUAL(fiu_fail("raft/network/send_request_vote"), 0);
    BOOST_CHECK_EQUAL(fiu_fail("raft/network/send_append_entries"), 0);
}

// disk_degradation_profile: persistence writes can throw while active.
BOOST_AUTO_TEST_CASE(disk_degradation_profile_raii, *boost::unit_test::timeout(30)) {
    kythira::chaos::clear_all_faults();

    {
        // 100% failure rate for deterministic testing.
        kythira::chaos::disk_degradation_profile profile{1.0};

        kythira::memory_persistence_engine<> engine;
        BOOST_CHECK_THROW(
            engine.append_log_entry(kythira::log_entry<>{._term = 1, ._index = 1, ._command = {}}),
            std::runtime_error);
        BOOST_CHECK_THROW(engine.save_current_term(1u), std::runtime_error);
    }

    // After destruction fault points are cleared.
    kythira::memory_persistence_engine<> engine;
    BOOST_CHECK_NO_THROW(engine.save_current_term(1u));
    BOOST_CHECK_NO_THROW(
        engine.append_log_entry(kythira::log_entry<>{._term = 1, ._index = 1, ._command = {}}));
}

// leader_crash_profile: fires once then self-disables.
BOOST_AUTO_TEST_CASE(leader_crash_profile_fires_once, *boost::unit_test::timeout(30)) {
    kythira::chaos::clear_all_faults();

    kythira::memory_persistence_engine<> engine;

    {
        kythira::chaos::leader_crash_profile profile;

        // First call throws.
        BOOST_CHECK_THROW(engine.save_current_term(1u), std::runtime_error);
        // Second call must succeed (fault disabled itself after firing once).
        BOOST_CHECK_NO_THROW(engine.save_current_term(2u));
        BOOST_CHECK_EQUAL(engine.load_current_term(), 2u);
    }

    BOOST_CHECK_NO_THROW(engine.save_current_term(3u));
}

// state_machine_fault_profile: apply throws while profile is active, succeeds after.
BOOST_AUTO_TEST_CASE(state_machine_fault_profile_raii, *boost::unit_test::timeout(30)) {
    kythira::chaos::clear_all_faults();

    kythira::test_key_value_state_machine<> sm;
    auto cmd = kythira::test_key_value_state_machine<>::make_put_command("k", "v");

    {
        kythira::chaos::state_machine_fault_profile profile{1.0};  // 100% failure
        BOOST_CHECK_THROW(sm.apply(cmd, 1u), std::runtime_error);
    }

    BOOST_CHECK_NO_THROW(sm.apply(cmd, 1u));
}

// explicit disable(): calling disable() mid-scope releases the fault points.
BOOST_AUTO_TEST_CASE(explicit_disable_releases_points, *boost::unit_test::timeout(30)) {
    kythira::chaos::clear_all_faults();

    kythira::chaos::network_partition_profile profile;
    BOOST_CHECK_NE(fiu_fail("raft/network/send_request_vote"), 0);

    profile.disable();
    BOOST_CHECK_EQUAL(fiu_fail("raft/network/send_request_vote"), 0);

    // Second disable() must be a no-op (not crash).
    BOOST_CHECK_NO_THROW(profile.disable());
}

BOOST_AUTO_TEST_SUITE_END()
