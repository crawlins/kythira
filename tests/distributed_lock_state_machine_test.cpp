#define BOOST_TEST_MODULE distributed_lock_state_machine_test
#include <boost/test/unit_test.hpp>
#include <raft/examples/distributed_lock_state_machine.hpp>
#include <raft/types.hpp>

using namespace kythira::examples;

static_assert(kythira::state_machine<distributed_lock_state_machine, std::uint64_t>,
              "distributed_lock_state_machine must satisfy state_machine concept");

namespace {
auto make_command(const std::string& cmd) -> std::vector<std::byte> {
    return {reinterpret_cast<const std::byte*>(cmd.data()),
            reinterpret_cast<const std::byte*>(cmd.data() + cmd.size())};
}

auto parse_result(const std::vector<std::byte>& result) -> std::string {
    return {reinterpret_cast<const char*>(result.data()), result.size()};
}
}

BOOST_AUTO_TEST_CASE(test_distributed_lock_acquire_free_lock, *boost::unit_test::timeout(10)) {
    distributed_lock_state_machine sm;

    auto result = sm.apply(make_command("ACQUIRE resource client1 100"), 1);
    BOOST_CHECK_EQUAL(parse_result(result), "OK");
}

BOOST_AUTO_TEST_CASE(test_distributed_lock_acquire_contended, *boost::unit_test::timeout(10)) {
    distributed_lock_state_machine sm;

    sm.apply(make_command("ACQUIRE resource client1 100"), 1);

    auto result = sm.apply(make_command("ACQUIRE resource client2 100"), 2);
    BOOST_CHECK_EQUAL(parse_result(result), "LOCKED");
}

BOOST_AUTO_TEST_CASE(test_distributed_lock_release_by_owner, *boost::unit_test::timeout(10)) {
    distributed_lock_state_machine sm;

    sm.apply(make_command("ACQUIRE resource client1 100"), 1);

    auto result = sm.apply(make_command("RELEASE resource client1"), 2);
    BOOST_CHECK_EQUAL(parse_result(result), "OK");

    // Freed, so a different client can now acquire it
    result = sm.apply(make_command("ACQUIRE resource client2 100"), 3);
    BOOST_CHECK_EQUAL(parse_result(result), "OK");
}

BOOST_AUTO_TEST_CASE(test_distributed_lock_release_by_non_owner, *boost::unit_test::timeout(10)) {
    distributed_lock_state_machine sm;

    sm.apply(make_command("ACQUIRE resource client1 100"), 1);

    auto result = sm.apply(make_command("RELEASE resource client2"), 2);
    BOOST_CHECK_EQUAL(parse_result(result), "NOT_OWNER");

    // Still held by client1
    result = sm.apply(make_command("QUERY resource"), 3);
    BOOST_CHECK_EQUAL(parse_result(result), "LOCKED:client1");
}

BOOST_AUTO_TEST_CASE(test_distributed_lock_query_free, *boost::unit_test::timeout(10)) {
    distributed_lock_state_machine sm;

    auto result = sm.apply(make_command("QUERY nonexistent"), 1);
    BOOST_CHECK_EQUAL(parse_result(result), "FREE");
}

BOOST_AUTO_TEST_CASE(test_distributed_lock_query_held, *boost::unit_test::timeout(10)) {
    distributed_lock_state_machine sm;

    sm.apply(make_command("ACQUIRE resource client1 100"), 1);

    auto result = sm.apply(make_command("QUERY resource"), 2);
    BOOST_CHECK_EQUAL(parse_result(result), "LOCKED:client1");
}

BOOST_AUTO_TEST_CASE(test_distributed_lock_acquire_after_expiry, *boost::unit_test::timeout(10)) {
    distributed_lock_state_machine sm;

    // Acquire at index 10 with a 5-entry timeout -> expires at index 15
    sm.apply(make_command("ACQUIRE resource client1 5"), 10);

    // Still held just before expiry
    auto result = sm.apply(make_command("QUERY resource"), 14);
    BOOST_CHECK_EQUAL(parse_result(result), "LOCKED:client1");

    // A different owner can acquire once index reaches the expiry index,
    // without any intervening QUERY to trigger cleanup
    result = sm.apply(make_command("ACQUIRE resource client2 5"), 15);
    BOOST_CHECK_EQUAL(parse_result(result), "OK");

    result = sm.apply(make_command("QUERY resource"), 16);
    BOOST_CHECK_EQUAL(parse_result(result), "LOCKED:client2");
}

BOOST_AUTO_TEST_CASE(test_distributed_lock_query_triggers_expiry, *boost::unit_test::timeout(10)) {
    distributed_lock_state_machine sm;

    // Acquire at index 1 with a 3-entry timeout -> expires at index 4
    sm.apply(make_command("ACQUIRE resource client1 3"), 1);

    auto result = sm.apply(make_command("QUERY resource"), 3);
    BOOST_CHECK_EQUAL(parse_result(result), "LOCKED:client1");

    result = sm.apply(make_command("QUERY resource"), 4);
    BOOST_CHECK_EQUAL(parse_result(result), "FREE");

    // Still free afterwards
    result = sm.apply(make_command("QUERY resource"), 100);
    BOOST_CHECK_EQUAL(parse_result(result), "FREE");
}

BOOST_AUTO_TEST_CASE(test_distributed_lock_malformed_acquire, *boost::unit_test::timeout(10)) {
    distributed_lock_state_machine sm;

    BOOST_CHECK_THROW(sm.apply(make_command("ACQUIRE resource"), 1), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(test_distributed_lock_malformed_release, *boost::unit_test::timeout(10)) {
    distributed_lock_state_machine sm;

    BOOST_CHECK_THROW(sm.apply(make_command("RELEASE resource"), 1), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(test_distributed_lock_unknown_command, *boost::unit_test::timeout(10)) {
    distributed_lock_state_machine sm;

    BOOST_CHECK_THROW(sm.apply(make_command("FROB resource"), 1), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(test_distributed_lock_snapshot_round_trip, *boost::unit_test::timeout(10)) {
    distributed_lock_state_machine sm1;

    sm1.apply(make_command("ACQUIRE lockA client1 100"), 1);
    sm1.apply(make_command("ACQUIRE lockB client2 100"), 2);

    auto snapshot = sm1.get_state();

    distributed_lock_state_machine sm2;
    sm2.restore_from_snapshot(snapshot, 2);

    BOOST_CHECK_EQUAL(parse_result(sm2.apply(make_command("QUERY lockA"), 3)), "LOCKED:client1");
    BOOST_CHECK_EQUAL(parse_result(sm2.apply(make_command("QUERY lockB"), 3)), "LOCKED:client2");
}

BOOST_AUTO_TEST_CASE(test_distributed_lock_empty_snapshot, *boost::unit_test::timeout(10)) {
    distributed_lock_state_machine sm;

    std::vector<std::byte> empty_snapshot;
    sm.restore_from_snapshot(empty_snapshot, 0);

    auto result = sm.apply(make_command("QUERY anything"), 1);
    BOOST_CHECK_EQUAL(parse_result(result), "FREE");
}

// Determinism: the same command sequence applied to two independent
// instances (including a lock expiring and being re-acquired) must produce
// byte-identical get_state() after every command. This is the property
// that std::chrono::steady_clock::now() previously violated.
BOOST_AUTO_TEST_CASE(test_distributed_lock_determinism, *boost::unit_test::timeout(10)) {
    distributed_lock_state_machine sm1;
    distributed_lock_state_machine sm2;

    struct step {
        std::string command;
        std::uint64_t index;
    };
    const std::vector<step> steps = {
        {"ACQUIRE lockA client1 5", 1},
        {"ACQUIRE lockB client2 10", 2},
        {"QUERY lockA", 3},
        {"ACQUIRE lockA client3 5", 6},  // lockA expired (expiry_index=6), re-acquired
        {"RELEASE lockB client2", 7},
        {"QUERY lockB", 8},
    };

    for (const auto& s : steps) {
        auto r1 = sm1.apply(make_command(s.command), s.index);
        auto r2 = sm2.apply(make_command(s.command), s.index);
        BOOST_CHECK(r1 == r2);
        BOOST_CHECK(sm1.get_state() == sm2.get_state());
    }
}
