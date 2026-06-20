#define BOOST_TEST_MODULE ChaosSmokeTest
#include <boost/test/unit_test.hpp>

#include <fiu.h>
#include <fiu-control.h>

#include "chaos_test_types.hpp"

using ChaosFixture = kythira::chaos::chaos_test_fixture;
BOOST_GLOBAL_FIXTURE(ChaosFixture);

BOOST_AUTO_TEST_SUITE(chaos_smoke)

// Verify that libfiu is correctly linked and the basic enable/check/disable
// cycle works.  This test exercises no Raft logic — it only confirms the
// fault injection machinery itself is operational.
BOOST_AUTO_TEST_CASE(libfiu_enable_check_disable, *boost::unit_test::timeout(30)) {
    kythira::chaos::clear_all_faults();

    // Enable a synthetic fault point not used by any real operation.
    BOOST_CHECK_EQUAL(fiu_fail("raft/smoke"), 0);

    fiu_enable("raft/smoke", 1, nullptr, 0);
    BOOST_CHECK_NE(fiu_fail("raft/smoke"), 0);

    kythira::chaos::clear_all_faults();
    BOOST_CHECK_EQUAL(fiu_fail("raft/smoke"), 0);
}

// Verify that the persistence fault point fires when enabled and is silent
// when disabled.
BOOST_AUTO_TEST_CASE(persistence_fault_point_fires, *boost::unit_test::timeout(30)) {
    kythira::chaos::clear_all_faults();

    kythira::memory_persistence_engine<> engine;

    // Without fault: save_current_term must succeed.
    BOOST_CHECK_NO_THROW(engine.save_current_term(1u));
    BOOST_CHECK_EQUAL(engine.load_current_term(), 1u);

    // With fault enabled: save_current_term must throw.
    fiu_enable("raft/persistence/save_current_term", 1, nullptr, 0);
    BOOST_CHECK_THROW(engine.save_current_term(2u), std::runtime_error);
    BOOST_CHECK_EQUAL(engine.load_current_term(), 1u);  // unchanged

    kythira::chaos::clear_all_faults();

    // Fault cleared: save must succeed again.
    BOOST_CHECK_NO_THROW(engine.save_current_term(3u));
    BOOST_CHECK_EQUAL(engine.load_current_term(), 3u);
}

// Verify the state machine fault point.
BOOST_AUTO_TEST_CASE(state_machine_fault_point_fires, *boost::unit_test::timeout(30)) {
    kythira::chaos::clear_all_faults();

    kythira::test_key_value_state_machine<> sm;
    auto cmd = kythira::test_key_value_state_machine<>::make_put_command("k", "v");

    BOOST_CHECK_NO_THROW(sm.apply(cmd, 1u));

    fiu_enable("raft/state_machine/apply", 1, nullptr, 0);
    BOOST_CHECK_THROW(sm.apply(cmd, 2u), std::runtime_error);

    kythira::chaos::clear_all_faults();
    BOOST_CHECK_NO_THROW(sm.apply(cmd, 2u));
}

BOOST_AUTO_TEST_SUITE_END()
