#define BOOST_TEST_MODULE replicated_log_state_machine_test
#include <boost/test/unit_test.hpp>
#include <raft/examples/replicated_log_state_machine.hpp>
#include <raft/types.hpp>

using namespace kythira::examples;

static_assert(kythira::state_machine<replicated_log_state_machine, std::uint64_t>,
              "replicated_log_state_machine must satisfy state_machine concept");

namespace {
auto make_command(const std::string& cmd) -> std::vector<std::byte> {
    return {reinterpret_cast<const std::byte*>(cmd.data()),
            reinterpret_cast<const std::byte*>(cmd.data() + cmd.size())};
}
}

BOOST_AUTO_TEST_CASE(test_replicated_log_single_append, *boost::unit_test::timeout(10)) {
    replicated_log_state_machine sm;

    auto cmd = make_command("APPEND first entry");
    auto result = sm.apply(cmd, 1);

    BOOST_CHECK(result.empty());
    BOOST_CHECK_EQUAL(sm.entry_count(), 1U);
}

BOOST_AUTO_TEST_CASE(test_replicated_log_multiple_appends, *boost::unit_test::timeout(10)) {
    replicated_log_state_machine sm;

    sm.apply(make_command("APPEND entry one"), 1);
    sm.apply(make_command("APPEND entry two"), 2);
    sm.apply(make_command("APPEND entry three"), 3);

    BOOST_CHECK_EQUAL(sm.entry_count(), 3U);
}

BOOST_AUTO_TEST_CASE(test_replicated_log_malformed_command, *boost::unit_test::timeout(10)) {
    replicated_log_state_machine sm;

    // Missing "APPEND " prefix, but long enough to pass the length check
    auto bad_cmd = make_command("NOTAPPEND data");
    BOOST_CHECK_THROW(sm.apply(bad_cmd, 1), std::invalid_argument);
    BOOST_CHECK_EQUAL(sm.entry_count(), 0U);
}

BOOST_AUTO_TEST_CASE(test_replicated_log_empty_command, *boost::unit_test::timeout(10)) {
    replicated_log_state_machine sm;

    std::vector<std::byte> empty_cmd;
    BOOST_CHECK_THROW(sm.apply(empty_cmd, 1), std::invalid_argument);
    BOOST_CHECK_EQUAL(sm.entry_count(), 0U);
}

BOOST_AUTO_TEST_CASE(test_replicated_log_snapshot_round_trip, *boost::unit_test::timeout(10)) {
    replicated_log_state_machine sm1;

    sm1.apply(make_command("APPEND alpha"), 1);
    sm1.apply(make_command("APPEND beta"), 2);
    sm1.apply(make_command("APPEND gamma"), 3);

    auto snapshot = sm1.get_state();

    replicated_log_state_machine sm2;
    sm2.restore_from_snapshot(snapshot, 3);

    BOOST_CHECK_EQUAL(sm2.entry_count(), 3U);
    BOOST_CHECK(sm2.get_state() == snapshot);
}

BOOST_AUTO_TEST_CASE(test_replicated_log_empty_snapshot, *boost::unit_test::timeout(10)) {
    replicated_log_state_machine sm;

    std::vector<std::byte> empty_snapshot;
    sm.restore_from_snapshot(empty_snapshot, 0);

    BOOST_CHECK_EQUAL(sm.entry_count(), 0U);
}

BOOST_AUTO_TEST_CASE(test_replicated_log_embedded_null_bytes, *boost::unit_test::timeout(10)) {
    replicated_log_state_machine sm1;

    // "APPEND " prefix followed by data containing embedded null and
    // non-ASCII bytes, to stress the length-prefixed encoding rather than
    // relying on null-terminated string handling anywhere in the pipeline.
    std::vector<std::byte> cmd = make_command("APPEND ");
    const unsigned char payload[] = {0x00, 0xFF, 'a', 0x00, 0x7F, 0x80, 'z'};
    for (unsigned char b : payload) {
        cmd.push_back(static_cast<std::byte>(b));
    }

    sm1.apply(cmd, 1);
    BOOST_CHECK_EQUAL(sm1.entry_count(), 1U);

    auto snapshot = sm1.get_state();

    replicated_log_state_machine sm2;
    sm2.restore_from_snapshot(snapshot, 1);

    BOOST_CHECK_EQUAL(sm2.entry_count(), 1U);
    BOOST_CHECK(sm2.get_state() == snapshot);
}
