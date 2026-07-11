#define BOOST_TEST_MODULE peer2peer_replication_unit_test
#include <boost/test/unit_test.hpp>

#include <raft/peer2peer_replication.hpp>

#include <folly/init/Init.h>

#include <chrono>
#include <memory>
#include <string>

// ── Folly global fixture ───────────────────────────────────────────────────

struct FollyInitFixture {
    FollyInitFixture() {
        int argc = 1;
        char* argv0[] = {const_cast<char*>("peer2peer_replication_unit_test"), nullptr};
        char** argv = argv0;
        _init = std::make_unique<folly::Init>(&argc, &argv);
    }
    std::unique_ptr<folly::Init> _init;
};

BOOST_GLOBAL_FIXTURE(FollyInitFixture);

namespace {
using node_id_t = std::uint64_t;
using address_t = std::string;
using log_index_t = std::uint64_t;
using replicator_t = kythira::static_peer2peer_replicator<node_id_t, address_t, log_index_t>;
}  // namespace

// ── Requirement 1.3: no_op_peer2peer_replicator ────────────────────────────

BOOST_AUTO_TEST_CASE(no_op_replicator_always_succeeds_and_returns_nullopt) {
    kythira::no_op_peer2peer_replicator<node_id_t, address_t, log_index_t> replicator;

    auto advertise_fut = replicator.advertise_progress(1, "addr1", 3, 100);
    BOOST_CHECK_NO_THROW(std::move(advertise_fut).get());

    auto membership_fut = replicator.update_membership({1, 2, 3});
    BOOST_CHECK_NO_THROW(std::move(membership_fut).get());

    auto source_fut = replicator.find_catch_up_source(1, 100, std::chrono::milliseconds{100});
    auto source = std::move(source_fut).get();
    BOOST_CHECK(!source.has_value());
}

// ── Requirement 9.1: static_peer2peer_replicator ───────────────────────────

BOOST_AUTO_TEST_CASE(static_replicator_visible_across_instances_sharing_table) {
    auto table = std::make_shared<replicator_t::table_type>();
    replicator_t node1_view(table);
    replicator_t node2_view(table);

    std::move(node1_view.update_membership({1, 2})).get();
    std::move(node2_view.update_membership({1, 2})).get();

    // node1 advertises itself far ahead
    std::move(node1_view.advertise_progress(1, "addr1", 5, 500)).get();

    // node2 looks for a source covering index 100 — should see node1
    auto source =
        std::move(node2_view.find_catch_up_source(100, 200, std::chrono::milliseconds{100})).get();
    BOOST_REQUIRE(source.has_value());
    BOOST_CHECK_EQUAL(source->node_id, 1u);
    BOOST_CHECK_EQUAL(source->address, "addr1");
}

BOOST_AUTO_TEST_CASE(static_replicator_no_candidate_below_from_index) {
    auto table = std::make_shared<replicator_t::table_type>();
    replicator_t node1_view(table);
    replicator_t node2_view(table);

    std::move(node1_view.update_membership({1, 2})).get();
    std::move(node2_view.update_membership({1, 2})).get();

    std::move(node1_view.advertise_progress(1, "addr1", 1, 10)).get();

    // node2 wants index 100 but node1 only has 10 — no candidate.
    auto source =
        std::move(node2_view.find_catch_up_source(100, 200, std::chrono::milliseconds{100})).get();
    BOOST_CHECK(!source.has_value());
}

// ── Requirement 11.4 / design.md Property 4: membership filtering is
// per-instance, not derived from the shared digest table ──────────────────

BOOST_AUTO_TEST_CASE(static_replicator_excludes_non_members_even_if_digest_lingers) {
    auto table = std::make_shared<replicator_t::table_type>();
    replicator_t node1_view(table);
    replicator_t node2_view(table);

    // node1's digest is in the shared table...
    std::move(node1_view.advertise_progress(1, "addr1", 1, 500)).get();

    // ...but node2 has never been told node1 is a current member.
    std::move(node2_view.update_membership({2})).get();
    auto source =
        std::move(node2_view.find_catch_up_source(1, 200, std::chrono::milliseconds{100})).get();
    BOOST_CHECK(!source.has_value());

    // Once node2 is told node1 is a member, the same lingering digest becomes usable.
    std::move(node2_view.update_membership({1, 2})).get();
    source =
        std::move(node2_view.find_catch_up_source(1, 200, std::chrono::milliseconds{100})).get();
    BOOST_CHECK(source.has_value());

    // Removing node1 from membership again makes it unavailable, even though its
    // digest still lingers in the shared table.
    std::move(node2_view.update_membership({2})).get();
    source =
        std::move(node2_view.find_catch_up_source(1, 200, std::chrono::milliseconds{100})).get();
    BOOST_CHECK(!source.has_value());
}

BOOST_AUTO_TEST_CASE(static_replicator_empty_before_first_update_membership) {
    auto table = std::make_shared<replicator_t::table_type>();
    replicator_t node1_view(table);
    replicator_t node2_view(table);

    std::move(node1_view.advertise_progress(1, "addr1", 1, 500)).get();

    // node2 has never called update_membership — its member set is empty
    // (Requirement 2.2 for the transport spec's sibling concept applies
    // identically here: "no update_membership call yet" means no source ever
    // offered).
    auto source =
        std::move(node2_view.find_catch_up_source(1, 200, std::chrono::milliseconds{100})).get();
    BOOST_CHECK(!source.has_value());
}
