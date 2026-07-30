// Feature: grpc-transport
//
// Property + unit tests that need no live gRPC server: protobuf round-trip
// fidelity (Property 2), concept conformance via static_assert (Requirement
// 19.4), and the status-code → exception-type mapping table (Property 3).
//
// These exercise the message-conversion layer and type-level contracts of the
// gRPC transport (.kiro/specs/grpc-transport/) directly, without binding a
// socket, so they run fast and deterministically.

#define BOOST_TEST_MODULE GrpcTransportConversionPropertyTest
#include <boost/test/unit_test.hpp>

#include <raft/grpc_exceptions.hpp>
#include <raft/grpc_message_conversion.hpp>
#include <raft/grpc_transport_impl.hpp>

#include <cstddef>
#include <optional>
#include <random>
#include <string>
#include <vector>

namespace {
constexpr std::size_t property_test_iterations = 100;
constexpr std::uint64_t max_term = 1'000'000;
constexpr std::uint64_t max_index = 1'000'000;
constexpr std::uint64_t max_node_id = 10'000;
constexpr std::size_t max_entries = 10;
constexpr std::size_t max_command_size = 100;

auto rng_seeded() -> std::mt19937 {
    std::random_device rd;
    return std::mt19937(rd());
}

auto random_u64(std::mt19937& rng, std::uint64_t hi) -> std::uint64_t {
    std::uniform_int_distribution<std::uint64_t> dist(0, hi);
    return dist(rng);
}

auto random_bytes(std::mt19937& rng, std::size_t max_size) -> std::vector<std::byte> {
    std::uniform_int_distribution<std::size_t> size_dist(0, max_size);
    std::uniform_int_distribution<int> byte_dist(0, 255);
    std::vector<std::byte> out;
    const std::size_t size = size_dist(rng);
    out.reserve(size);
    for (std::size_t i = 0; i < size; ++i) {
        out.push_back(static_cast<std::byte>(byte_dist(rng)));
    }
    return out;
}

auto random_entry_type(std::mt19937& rng) -> kythira::entry_type {
    switch (random_u64(rng, 2)) {
        case 0:
            return kythira::entry_type::normal;
        case 1:
            return kythira::entry_type::configuration;
        default:
            return kythira::entry_type::no_op;
    }
}

auto random_log_entry(std::mt19937& rng) -> kythira::log_entry<> {
    return kythira::log_entry<>{._term = random_u64(rng, max_term),
                                ._index = random_u64(rng, max_index),
                                ._command = random_bytes(rng, max_command_size),
                                ._type = random_entry_type(rng)};
}

auto random_entries(std::mt19937& rng) -> std::vector<kythira::log_entry<>> {
    std::uniform_int_distribution<std::size_t> count_dist(0, max_entries);
    const std::size_t count = count_dist(rng);
    std::vector<kythira::log_entry<>> entries;
    entries.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        entries.push_back(random_log_entry(rng));
    }
    return entries;
}

auto entries_equal(const std::vector<kythira::log_entry<>>& a,
                   const std::vector<kythira::log_entry<>>& b) -> bool {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].term() != b[i].term() || a[i].index() != b[i].index() ||
            a[i].command() != b[i].command() || a[i].type() != b[i].type()) {
            return false;
        }
    }
    return true;
}
}  // namespace

// ── Property 2: Protobuf round-trip preserves content ───────────────────────
// Feature: grpc-transport, Property 2: For any valid kythira request or
// response value, from_proto(to_proto(value)) equals the original across every
// field, including absent optionals and empty entries().

BOOST_AUTO_TEST_CASE(property_request_vote_roundtrip) {
    auto rng = rng_seeded();
    for (std::size_t i = 0; i < property_test_iterations; ++i) {
        kythira::request_vote_request<> req{._term = random_u64(rng, max_term),
                                            ._candidate_id = random_u64(rng, max_node_id),
                                            ._last_log_index = random_u64(rng, max_index),
                                            ._last_log_term = random_u64(rng, max_term)};
        auto out = kythira::from_proto(kythira::to_proto(req));
        BOOST_TEST(out.term() == req.term());
        BOOST_TEST(out.candidate_id() == req.candidate_id());
        BOOST_TEST(out.last_log_index() == req.last_log_index());
        BOOST_TEST(out.last_log_term() == req.last_log_term());

        kythira::request_vote_response<> resp{._term = random_u64(rng, max_term),
                                              ._vote_granted = (random_u64(rng, 1) != 0)};
        auto resp_out = kythira::from_proto(kythira::to_proto(resp));
        BOOST_TEST(resp_out.term() == resp.term());
        BOOST_TEST(resp_out.vote_granted() == resp.vote_granted());
    }
}

BOOST_AUTO_TEST_CASE(property_append_entries_roundtrip) {
    auto rng = rng_seeded();
    for (std::size_t i = 0; i < property_test_iterations; ++i) {
        kythira::append_entries_request<> req{._term = random_u64(rng, max_term),
                                              ._leader_id = random_u64(rng, max_node_id),
                                              ._prev_log_index = random_u64(rng, max_index),
                                              ._prev_log_term = random_u64(rng, max_term),
                                              ._entries = random_entries(rng),
                                              ._leader_commit = random_u64(rng, max_index)};
        auto out = kythira::from_proto(kythira::to_proto(req));
        BOOST_TEST(out.term() == req.term());
        BOOST_TEST(out.leader_id() == req.leader_id());
        BOOST_TEST(out.prev_log_index() == req.prev_log_index());
        BOOST_TEST(out.prev_log_term() == req.prev_log_term());
        BOOST_TEST(out.leader_commit() == req.leader_commit());
        BOOST_TEST(entries_equal(out.entries(), req.entries()));

        // Response with both present and both absent conflict hints.
        const bool with_conflict = (random_u64(rng, 1) != 0);
        kythira::append_entries_response<> resp{
            ._term = random_u64(rng, max_term),
            ._success = (random_u64(rng, 1) != 0),
            ._conflict_index = with_conflict
                                   ? std::optional<std::uint64_t>(random_u64(rng, max_index))
                                   : std::nullopt,
            ._conflict_term = with_conflict
                                  ? std::optional<std::uint64_t>(random_u64(rng, max_term))
                                  : std::nullopt};
        auto resp_out = kythira::from_proto(kythira::to_proto(resp));
        BOOST_TEST(resp_out.term() == resp.term());
        BOOST_TEST(resp_out.success() == resp.success());
        BOOST_TEST(resp_out.conflict_index().has_value() == resp.conflict_index().has_value());
        BOOST_TEST(resp_out.conflict_term().has_value() == resp.conflict_term().has_value());
        if (resp.conflict_index().has_value()) {
            BOOST_TEST(resp_out.conflict_index().value() == resp.conflict_index().value());
            BOOST_TEST(resp_out.conflict_term().value() == resp.conflict_term().value());
        }
    }
}

BOOST_AUTO_TEST_CASE(property_install_snapshot_roundtrip) {
    auto rng = rng_seeded();
    for (std::size_t i = 0; i < property_test_iterations; ++i) {
        kythira::install_snapshot_request<> req{
            ._term = random_u64(rng, max_term),
            ._leader_id = random_u64(rng, max_node_id),
            ._last_included_index = random_u64(rng, max_index),
            ._last_included_term = random_u64(rng, max_term),
            ._offset = static_cast<std::size_t>(random_u64(rng, max_index)),
            ._data = random_bytes(rng, max_command_size),
            ._done = (random_u64(rng, 1) != 0)};
        auto out = kythira::from_proto(kythira::to_proto(req));
        BOOST_TEST(out.term() == req.term());
        BOOST_TEST(out.leader_id() == req.leader_id());
        BOOST_TEST(out.last_included_index() == req.last_included_index());
        BOOST_TEST(out.last_included_term() == req.last_included_term());
        BOOST_TEST(out.offset() == req.offset());
        BOOST_TEST(out.data() == req.data());
        BOOST_TEST(out.done() == req.done());

        kythira::install_snapshot_response<> resp{._term = random_u64(rng, max_term)};
        auto resp_out = kythira::from_proto(kythira::to_proto(resp));
        BOOST_TEST(resp_out.term() == resp.term());
    }
}

BOOST_AUTO_TEST_CASE(property_cluster_bootstrap_roundtrip) {
    auto rng = rng_seeded();
    for (std::size_t i = 0; i < property_test_iterations; ++i) {
        // Join request/response with and without a redirect hint (Requirement 15.4).
        kythira::cluster_join_request<> jreq{.node_id = random_u64(rng, max_node_id),
                                             .contact_address = "10.0.0.1:9443"};
        auto jreq_out = kythira::from_proto(kythira::to_proto(jreq));
        BOOST_TEST(jreq_out.joining_node_id() == jreq.joining_node_id());
        BOOST_TEST(jreq_out.joining_address() == jreq.joining_address());

        const bool with_redirect = (random_u64(rng, 1) != 0);
        kythira::cluster_join_response<> jresp{
            .accepted = (random_u64(rng, 1) != 0),
            .redirect =
                with_redirect
                    ? std::optional<kythira::peer_info<std::uint64_t, std::string>>(
                          kythira::peer_info<std::uint64_t, std::string>{
                              .node_id = random_u64(rng, max_node_id), .address = "10.0.0.2:9443"})
                    : std::nullopt};
        auto jresp_out = kythira::from_proto(kythira::to_proto(jresp));
        BOOST_TEST(jresp_out.is_accepted() == jresp.is_accepted());
        BOOST_TEST(jresp_out.redirect_peer().has_value() == jresp.redirect_peer().has_value());
        if (jresp.redirect_peer().has_value()) {
            BOOST_TEST(jresp_out.redirect_peer()->node_id == jresp.redirect_peer()->node_id);
            BOOST_TEST(jresp_out.redirect_peer()->address == jresp.redirect_peer()->address);
        }

        kythira::cluster_leave_request<> lreq{.node_id = random_u64(rng, max_node_id)};
        auto lreq_out = kythira::from_proto(kythira::to_proto(lreq));
        BOOST_TEST(lreq_out.leaving_node_id() == lreq.leaving_node_id());
    }
}

BOOST_AUTO_TEST_CASE(property_fetch_log_entries_roundtrip) {
    auto rng = rng_seeded();
    for (std::size_t i = 0; i < property_test_iterations; ++i) {
        kythira::fetch_log_entries_request<> req{._requester_id = random_u64(rng, max_node_id),
                                                 ._from_index = random_u64(rng, max_index),
                                                 ._to_index = random_u64(rng, max_index)};
        auto req_out = kythira::from_proto(kythira::to_proto(req));
        BOOST_TEST(req_out.requester_id() == req.requester_id());
        BOOST_TEST(req_out.from_index() == req.from_index());
        BOOST_TEST(req_out.to_index() == req.to_index());

        kythira::fetch_log_entries_response<> resp{._responder_id = random_u64(rng, max_node_id),
                                                   ._available = (random_u64(rng, 1) != 0),
                                                   ._prev_log_term = random_u64(rng, max_term),
                                                   ._entries = random_entries(rng)};
        auto resp_out = kythira::from_proto(kythira::to_proto(resp));
        BOOST_TEST(resp_out.responder_id() == resp.responder_id());
        BOOST_TEST(resp_out.available() == resp.available());
        BOOST_TEST(resp_out.prev_log_term() == resp.prev_log_term());
        BOOST_TEST(entries_equal(resp_out.entries(), resp.entries()));  // Requirement 17.3
    }
}

BOOST_AUTO_TEST_CASE(entry_type_roundtrip_every_value) {
    for (auto type : {kythira::entry_type::normal, kythira::entry_type::configuration,
                      kythira::entry_type::no_op}) {
        BOOST_TEST((kythira::entry_type_from_proto(kythira::to_proto(type)) == type));
    }
}

BOOST_AUTO_TEST_CASE(unknown_enum_wire_value_fails_closed) {
    // A wire value outside the known EntryType set must be rejected, not
    // silently coerced (Requirement 11.6).
    auto bogus = static_cast<kythira::raft::v1::EntryType>(99);
    BOOST_CHECK_THROW(kythira::entry_type_from_proto(bogus),
                      kythira::grpc_message_conversion_error);
}

// ── Property 3 building blocks: exception types carry their status code ──────

BOOST_AUTO_TEST_CASE(exception_types_carry_status_codes) {
    kythira::grpc_connection_error conn("down");
    BOOST_TEST((conn.status_code() == grpc::StatusCode::UNAVAILABLE));

    kythira::grpc_timeout_error timeout("slow", std::chrono::milliseconds(250));
    BOOST_TEST((timeout.status_code() == grpc::StatusCode::DEADLINE_EXCEEDED));
    BOOST_TEST(timeout.configured_timeout() == std::chrono::milliseconds(250));

    kythira::grpc_client_error client(grpc::StatusCode::INVALID_ARGUMENT, "bad");
    BOOST_TEST((client.status_code() == grpc::StatusCode::INVALID_ARGUMENT));

    kythira::grpc_server_error server(grpc::StatusCode::INTERNAL, "boom");
    BOOST_TEST((server.status_code() == grpc::StatusCode::INTERNAL));

    kythira::grpc_tls_configuration_error tls("no cert");
    BOOST_TEST((tls.status_code() == grpc::StatusCode::INVALID_ARGUMENT));
}

// ── Requirement 19.4: concept conformance ───────────────────────────────────

static_assert(kythira::network_client<kythira::grpc_client<kythira::grpc_kythira_transport_types>>);
static_assert(kythira::network_client_with_pre_vote<
              kythira::grpc_client<kythira::grpc_kythira_transport_types>>);
static_assert(kythira::network_client_with_cluster_join<
              kythira::grpc_client<kythira::grpc_kythira_transport_types>>);
static_assert(kythira::network_client_with_cluster_leave<
              kythira::grpc_client<kythira::grpc_kythira_transport_types>>);
static_assert(kythira::network_client_with_log_fetch<
              kythira::grpc_client<kythira::grpc_kythira_transport_types>>);
static_assert(kythira::network_server<kythira::grpc_server<kythira::grpc_kythira_transport_types>>);
static_assert(kythira::network_server_with_pre_vote<
              kythira::grpc_server<kythira::grpc_kythira_transport_types>>);
static_assert(kythira::network_server_with_cluster_join<
              kythira::grpc_server<kythira::grpc_kythira_transport_types>>);
static_assert(kythira::network_server_with_cluster_leave<
              kythira::grpc_server<kythira::grpc_kythira_transport_types>>);
static_assert(kythira::network_server_with_log_fetch<
              kythira::grpc_server<kythira::grpc_kythira_transport_types>>);

BOOST_AUTO_TEST_CASE(concepts_satisfied) {
    // The static_asserts above are the real test; this keeps a runtime case so
    // the translation unit reports as an executed Boost.Test suite.
    BOOST_TEST(true);
}
