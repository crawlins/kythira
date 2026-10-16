// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// **Feature: cloud-composite-node-ids, tasks 6.2, 6.4 and 6.5 (gRPC)**
// proto/raft.proto carries every node id twice: the original uint64 field
// and a NodeIdValue at a new tag. These cases pin the rolling-upgrade
// contract against the schema as it stood before the NodeIdValue fields --
// rebuilt at run time from raft.proto's own descriptor with those fields
// removed -- in both directions, then run grpc_client/grpc_server
// instantiated for std::string and aws_ec2_node_id through every RPC kind.
// **Validates: Requirements 9.2, 9.5, 11**

#define BOOST_TEST_MODULE grpc_textual_node_id_test
#include <boost/test/unit_test.hpp>

#include <raft/composite_node_id.hpp>
#include <raft/grpc_exceptions.hpp>
#include <raft/grpc_message_conversion.hpp>
#include <raft/grpc_transport_impl.hpp>

#include <folly/executors/CPUThreadPoolExecutor.h>

#include <google/protobuf/descriptor.h>
#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/dynamic_message.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

using namespace std::chrono_literals;
namespace pb = google::protobuf;
namespace v1 = kythira::raft::v1;
using types = kythira::grpc_kythira_transport_types;

// ── The pre-NodeIdValue schema ─────────────────────────────────────────────

// raft.proto as an older peer compiled it: the same file with every
// NodeIdValue-typed field dropped. Those fields are the only change the
// composite-id work made, so this is that schema exactly, and it cannot
// drift from the real one.
class legacy_schema {
public:
    legacy_schema() {
        pb::FileDescriptorProto file;
        v1::RequestVoteRequest::descriptor()->file()->CopyTo(&file);
        for (auto& message : *file.mutable_message_type()) {
            auto* fields = message.mutable_field();
            for (int i = fields->size() - 1; i >= 0; --i) {
                if (fields->Get(i).type_name().ends_with("NodeIdValue")) {
                    fields->DeleteSubrange(i, 1);
                }
            }
        }
        file.clear_service();
        _file = _pool.BuildFile(file);
        BOOST_REQUIRE(_file != nullptr);
    }

    // A message of the legacy type named like `M`, holding `bytes`.
    template<typename M> auto parse(const std::string& bytes) -> std::unique_ptr<pb::Message> {
        std::unique_ptr<pb::Message> message(prototype<M>()->New());
        BOOST_REQUIRE(message->ParseFromString(bytes));
        return message;
    }

    template<typename M> auto make() -> std::unique_ptr<pb::Message> {
        return std::unique_ptr<pb::Message>(prototype<M>()->New());
    }

    template<typename M> [[nodiscard]] auto descriptor() const -> const pb::Descriptor* {
        return _file->pool()->FindMessageTypeByName(M::descriptor()->full_name());
    }

private:
    template<typename M> auto prototype() -> const pb::Message* {
        const auto* type = descriptor<M>();
        BOOST_REQUIRE(type != nullptr);
        // None of its fields is a NodeIdValue.
        for (int i = 0; i < type->field_count(); ++i) {
            BOOST_REQUIRE(type->field(i)->message_type() == nullptr ||
                          !type->field(i)->message_type()->name().ends_with("NodeIdValue"));
        }
        return _factory.GetPrototype(type);
    }

    pb::DescriptorPool _pool;
    const pb::FileDescriptor* _file{nullptr};
    pb::DynamicMessageFactory _factory;
};

auto legacy_u64(const pb::Message& message, const char* field) -> std::uint64_t {
    const auto* descriptor = message.GetDescriptor()->FindFieldByName(field);
    BOOST_REQUIRE(descriptor != nullptr);
    return message.GetReflection()->GetUInt64(message, descriptor);
}

auto set_legacy_u64(pb::Message& message, const char* field, std::uint64_t value) -> void {
    const auto* descriptor = message.GetDescriptor()->FindFieldByName(field);
    BOOST_REQUIRE(descriptor != nullptr);
    message.GetReflection()->SetUInt64(&message, descriptor, value);
}

auto legacy_message(pb::Message& message, const char* field) -> pb::Message& {
    const auto* descriptor = message.GetDescriptor()->FindFieldByName(field);
    BOOST_REQUIRE(descriptor != nullptr);
    return *message.GetReflection()->MutableMessage(&message, descriptor);
}

// A node id larger than 2^63, so a signed or narrowing step anywhere shows.
constexpr std::uint64_t big_id = 0xF000'0000'0000'0007ULL;

// The ids the end-to-end cases name their peers by.
template<typename NodeId> struct ids;
template<> struct ids<std::string> {
    static auto server() -> std::string { return "docker:kythira:2"; }
    static auto client() -> std::string { return "node 1/50%:x"; }
};
template<> struct ids<kythira::aws_ec2_node_id> {
    static auto server() -> kythira::aws_ec2_node_id {
        return kythira::aws_ec2_node_id::parse("aws-ec2:us-east-1:i-0123456789abcdef0").value();
    }
    static auto client() -> kythira::aws_ec2_node_id {
        return kythira::aws_ec2_node_id::parse("aws-ec2:eu-west-1:i-0fedcba9876543210").value();
    }
};

}  // namespace

// ── New numeric sender → old receiver ──────────────────────────────────────

BOOST_AUTO_TEST_SUITE(rolling_upgrade)

// A numeric sender still fills every uint64 id field, so a peer built before
// the NodeIdValue fields reads the id it always read; it skips the new tags
// as unknown fields.
BOOST_AUTO_TEST_CASE(numeric_sender_keeps_the_legacy_fields_an_old_peer_reads) {
    legacy_schema old;
    using kythira::to_proto;

    auto rv = old.parse<v1::RequestVoteRequest>(
        to_proto(kythira::request_vote_request<>{._term = 3, ._candidate_id = big_id})
            .SerializeAsString());
    BOOST_TEST(legacy_u64(*rv, "candidate_id") == big_id);
    BOOST_TEST(legacy_u64(*rv, "term") == 3U);

    auto pv = old.parse<v1::RequestPreVoteRequest>(
        to_proto(kythira::request_pre_vote_request<>{._term = 3, ._candidate_id = 5})
            .SerializeAsString());
    BOOST_TEST(legacy_u64(*pv, "candidate_id") == 5U);

    auto ae = old.parse<v1::AppendEntriesRequest>(
        to_proto(kythira::append_entries_request<>{._term = 4, ._leader_id = big_id})
            .SerializeAsString());
    BOOST_TEST(legacy_u64(*ae, "leader_id") == big_id);

    auto is = old.parse<v1::InstallSnapshotRequest>(
        to_proto(kythira::install_snapshot_request<>{._term = 4, ._leader_id = 6})
            .SerializeAsString());
    BOOST_TEST(legacy_u64(*is, "leader_id") == 6U);

    auto tn = old.parse<v1::TimeoutNowRequest>(
        to_proto(kythira::timeout_now_request<>{._term = 4, ._leader_id = 8}).SerializeAsString());
    BOOST_TEST(legacy_u64(*tn, "leader_id") == 8U);

    auto cj = old.parse<v1::ClusterJoinRequest>(
        to_proto(kythira::cluster_join_request<>{.node_id = 9, .contact_address = "h:1"})
            .SerializeAsString());
    BOOST_TEST(legacy_u64(*cj, "node_id") == 9U);

    auto cl = old.parse<v1::ClusterLeaveRequest>(
        to_proto(kythira::cluster_leave_request<>{.node_id = 10}).SerializeAsString());
    BOOST_TEST(legacy_u64(*cl, "node_id") == 10U);

    kythira::cluster_join_response<> join_resp{
        .accepted = false,
        .redirect =
            kythira::peer_info<std::uint64_t, std::string>{.node_id = 11, .address = "leader:1"}};
    auto cjr = old.parse<v1::ClusterJoinResponse>(to_proto(join_resp).SerializeAsString());
    BOOST_TEST(legacy_u64(legacy_message(*cjr, "redirect"), "node_id") == 11U);

    auto fr = old.parse<v1::FetchLogEntriesRequest>(
        to_proto(kythira::fetch_log_entries_request<>{._requester_id = 12, ._from_index = 1})
            .SerializeAsString());
    BOOST_TEST(legacy_u64(*fr, "requester_id") == 12U);

    kythira::fetch_log_entries_response<> fetch_resp{
        ._responder_id = big_id, ._available = true, ._prev_log_term = 0, ._entries = {}};
    auto frr = old.parse<v1::FetchLogEntriesResponse>(to_proto(fetch_resp).SerializeAsString());
    BOOST_TEST(legacy_u64(*frr, "responder_id") == big_id);
}

// ── Old sender → new numeric receiver ──────────────────────────────────────

// A message from a peer that never heard of NodeIdValue carries the id only in
// the uint64 field, and a new numeric receiver falls back to it.
BOOST_AUTO_TEST_CASE(old_peer_messages_decode_from_the_legacy_fields) {
    legacy_schema old;

    auto decode = [&]<typename M>(std::type_identity<M>, const char* field, std::uint64_t id) {
        auto message = old.make<M>();
        set_legacy_u64(*message, field, id);
        M current;
        BOOST_REQUIRE(current.ParseFromString(message->SerializeAsString()));
        return kythira::from_proto(current);
    };

    BOOST_TEST(decode(std::type_identity<v1::RequestVoteRequest>{}, "candidate_id", big_id)
                   .candidate_id() == big_id);
    BOOST_TEST(
        decode(std::type_identity<v1::RequestPreVoteRequest>{}, "candidate_id", 5).candidate_id() ==
        5U);
    BOOST_TEST(
        decode(std::type_identity<v1::AppendEntriesRequest>{}, "leader_id", big_id).leader_id() ==
        big_id);
    BOOST_TEST(
        decode(std::type_identity<v1::InstallSnapshotRequest>{}, "leader_id", 6).leader_id() == 6U);
    BOOST_TEST(decode(std::type_identity<v1::TimeoutNowRequest>{}, "leader_id", 8).leader_id() ==
               8U);
    BOOST_TEST(
        decode(std::type_identity<v1::ClusterJoinRequest>{}, "node_id", 9).joining_node_id() == 9U);
    BOOST_TEST(
        decode(std::type_identity<v1::ClusterLeaveRequest>{}, "node_id", 10).leaving_node_id() ==
        10U);
    BOOST_TEST(decode(std::type_identity<v1::FetchLogEntriesRequest>{}, "requester_id", 12)
                   .requester_id() == 12U);
    BOOST_TEST(decode(std::type_identity<v1::FetchLogEntriesResponse>{}, "responder_id", big_id)
                   .responder_id() == big_id);

    // The redirect hint nests the id one level down.
    auto join = old.make<v1::ClusterJoinResponse>();
    set_legacy_u64(legacy_message(*join, "redirect"), "node_id", 11);
    v1::ClusterJoinResponse current;
    BOOST_REQUIRE(current.ParseFromString(join->SerializeAsString()));
    const auto decoded = kythira::from_proto(current);
    BOOST_REQUIRE(decoded.redirect_peer().has_value());
    BOOST_TEST(decoded.redirect_peer()->node_id == 11U);
}

// ── Field selection ────────────────────────────────────────────────────────

// A numeric id goes in both fields, a textual one only in NodeIdValue; the
// receiver prefers NodeIdValue when both are present.
BOOST_AUTO_TEST_CASE(numeric_fills_both_textual_fills_only_the_new_field) {
    const auto numeric = kythira::to_proto(kythira::request_vote_request<>{._candidate_id = 7});
    BOOST_TEST(numeric.candidate_id() == 7U);
    BOOST_REQUIRE(numeric.has_candidate());
    BOOST_TEST(numeric.candidate().numeric() == 7U);

    const auto id = ids<kythira::aws_ec2_node_id>::client();
    const auto textual = kythira::to_proto(
        kythira::request_vote_request<kythira::aws_ec2_node_id>{._candidate_id = id});
    BOOST_TEST(textual.candidate_id() == 0U);
    BOOST_REQUIRE(textual.has_candidate());
    BOOST_TEST(textual.candidate().text() == id.to_string());
    BOOST_TEST((kythira::from_proto<kythira::aws_ec2_node_id>(textual).candidate_id() == id));

    v1::RequestVoteRequest both;
    both.set_candidate_id(1);
    both.mutable_candidate()->set_numeric(2);
    BOOST_TEST(kythira::from_proto(both).candidate_id() == 2U);
}

// A receiver whose id type cannot represent what arrived refuses it rather
// than guessing: no id for a textual receiver, text for a numeric one, a
// number for a textual one, or text that is not a valid id.
BOOST_AUTO_TEST_CASE(unrepresentable_ids_fail_closed) {
    using kythira::grpc_message_conversion_error;
    v1::RequestVoteRequest legacy_only;
    legacy_only.set_candidate_id(7);
    BOOST_CHECK_THROW(kythira::from_proto<std::string>(legacy_only), grpc_message_conversion_error);

    v1::RequestVoteRequest text;
    text.mutable_candidate()->set_text("docker:kythira:3");
    BOOST_CHECK_THROW(kythira::from_proto(text), grpc_message_conversion_error);
    BOOST_TEST(kythira::from_proto<std::string>(text).candidate_id() == "docker:kythira:3");
    BOOST_CHECK_THROW(kythira::from_proto<kythira::aws_ec2_node_id>(text),
                      grpc_message_conversion_error);

    v1::RequestVoteRequest number;
    number.mutable_candidate()->set_numeric(7);
    BOOST_CHECK_THROW(kythira::from_proto<std::string>(number), grpc_message_conversion_error);

    v1::RequestVoteRequest empty_text;
    empty_text.mutable_candidate()->set_text("");
    BOOST_CHECK_THROW(kythira::from_proto<std::string>(empty_text), grpc_message_conversion_error);

    // A numeric id narrower than the wire's uint64 is range-checked.
    v1::RequestVoteRequest wide;
    wide.set_candidate_id(70000);
    BOOST_CHECK_THROW(kythira::from_proto<std::uint16_t>(wide), grpc_message_conversion_error);
}

BOOST_AUTO_TEST_SUITE_END()

// ── End to end ─────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(textual_end_to_end)

using node_id_types = std::tuple<std::string, kythira::aws_ec2_node_id>;

// Every RPC kind, addressed by and carrying textual ids.
BOOST_AUTO_TEST_CASE_TEMPLATE(every_rpc_round_trips, NodeId, node_id_types) {
    folly::CPUThreadPoolExecutor exec(4);
    const NodeId server_id = ids<NodeId>::server();
    const NodeId client_id = ids<NodeId>::client();

    kythira::grpc_server<types, NodeId> server("127.0.0.1", 0, {}, kythira::noop_metrics{}, exec);
    std::mutex seen_mutex;
    std::vector<NodeId> seen;
    auto record = [&](const NodeId& id) {
        const std::lock_guard lock(seen_mutex);
        seen.push_back(id);
    };
    server.register_request_vote_handler([&](const kythira::request_vote_request<NodeId>& r) {
        record(r.candidate_id());
        return kythira::request_vote_response<>{._term = r.term(),
                                                ._vote_granted = r.candidate_id() == client_id};
    });
    server.register_request_pre_vote_handler(
        [&](const kythira::request_pre_vote_request<NodeId>& r) {
            record(r.candidate_id());
            return kythira::request_pre_vote_response<>{
                ._term = r.term(), ._vote_granted = r.candidate_id() == client_id};
        });
    server.register_append_entries_handler([&](const kythira::append_entries_request<NodeId>& r) {
        record(r.leader_id());
        return kythira::append_entries_response<>{._term = r.term(), ._success = true};
    });
    server.register_install_snapshot_handler(
        [&](const kythira::install_snapshot_request<NodeId>& r) {
            record(r.leader_id());
            return kythira::install_snapshot_response<>{._term = r.term()};
        });
    server.register_timeout_now_handler([&](const kythira::timeout_now_request<NodeId>& r) {
        record(r.leader_id());
        return kythira::timeout_now_response<>{._term = r.term(), ._success = true};
    });
    server.register_cluster_join_handler([&](const kythira::cluster_join_request<NodeId>& r) {
        record(r.joining_node_id());
        // Not the leader: redirect to it, by its textual id.
        return kythira::cluster_join_response<NodeId>{
            .accepted = false,
            .redirect = kythira::peer_info<NodeId, std::string>{.node_id = server_id,
                                                                .address = "leader:7000"}};
    });
    server.register_cluster_leave_handler([&](const kythira::cluster_leave_request<NodeId>& r) {
        record(r.leaving_node_id());
        return kythira::cluster_leave_response<NodeId>{.accepted = true, .redirect = std::nullopt};
    });
    server.register_fetch_log_entries_handler(
        [&](const kythira::fetch_log_entries_request<NodeId>& r) {
            record(r.requester_id());
            return kythira::fetch_log_entries_response<NodeId>{
                ._responder_id = server_id,
                ._available = true,
                ._prev_log_term = 1,
                ._entries = {kythira::log_entry<>{._term = 2, ._index = r.from_index()}}};
        });
    server.start();

    const auto target = "127.0.0.1:" + std::to_string(server.bound_port());
    kythira::grpc_client<types, NodeId> client(
        std::unordered_map<NodeId, std::string>{{server_id, target}}, {}, kythira::noop_metrics{},
        exec);

    BOOST_TEST(client
                   .send_request_vote(server_id,
                                      kythira::request_vote_request<NodeId>{
                                          ._term = 3, ._candidate_id = client_id},
                                      2000ms)
                   .get()
                   .vote_granted());
    BOOST_TEST(client
                   .send_request_pre_vote(server_id,
                                          kythira::request_pre_vote_request<NodeId>{
                                              ._term = 3, ._candidate_id = client_id},
                                          2000ms)
                   .get()
                   .vote_granted());
    BOOST_TEST(client
                   .send_append_entries(
                       server_id,
                       kythira::append_entries_request<NodeId>{._term = 4, ._leader_id = client_id},
                       2000ms)
                   .get()
                   .success());
    BOOST_TEST(client
                   .send_install_snapshot(server_id,
                                          kythira::install_snapshot_request<NodeId>{
                                              ._term = 5, ._leader_id = client_id, ._done = true},
                                          2000ms)
                   .get()
                   .term() == 5U);
    BOOST_TEST(client
                   .send_timeout_now(
                       server_id,
                       kythira::timeout_now_request<NodeId>{._term = 6, ._leader_id = client_id},
                       2000ms)
                   .get()
                   .success());
    const auto join =
        client
            .send_cluster_join_request(target,
                                       kythira::cluster_join_request<NodeId>{
                                           .node_id = client_id, .contact_address = "me:7001"},
                                       2000ms)
            .get();
    BOOST_TEST(!join.is_accepted());
    BOOST_REQUIRE(join.redirect_peer().has_value());
    BOOST_TEST((join.redirect_peer()->node_id == server_id));
    BOOST_TEST(client
                   .send_cluster_leave_request(
                       target, kythira::cluster_leave_request<NodeId>{.node_id = client_id}, 2000ms)
                   .get()
                   .is_accepted());
    const auto fetched = client
                             .send_fetch_log_entries(
                                 server_id,
                                 kythira::fetch_log_entries_request<NodeId>{
                                     ._requester_id = client_id, ._from_index = 9, ._to_index = 9},
                                 2000ms)
                             .get();
    BOOST_TEST((fetched.responder_id() == server_id));
    BOOST_REQUIRE(fetched.entries().size() == 1U);
    BOOST_TEST(fetched.entries().front().index() == 9U);

    server.stop();
    const std::lock_guard lock(seen_mutex);
    BOOST_TEST(seen.size() == 8U);
    for (const auto& id : seen) {
        BOOST_TEST((id == client_id));
    }
}

// A textual sender's request reaching a numeric server carries no id that
// server can read, and is refused as INVALID_ARGUMENT before any handler.
BOOST_AUTO_TEST_CASE(textual_request_to_a_numeric_server_is_refused) {
    folly::CPUThreadPoolExecutor exec(4);
    kythira::grpc_server<types> server("127.0.0.1", 0, {}, kythira::noop_metrics{}, exec);
    std::atomic<int> handled{0};
    server.register_request_vote_handler([&](const kythira::request_vote_request<>& r) {
        handled.fetch_add(1);
        return kythira::request_vote_response<>{._term = r.term(), ._vote_granted = true};
    });
    server.start();

    kythira::grpc_client<types, std::string> client(
        std::unordered_map<std::string, std::string>{
            {"docker:kythira:2", "127.0.0.1:" + std::to_string(server.bound_port())}},
        {}, kythira::noop_metrics{}, exec);
    BOOST_CHECK_THROW(client
                          .send_request_vote("docker:kythira:2",
                                             kythira::request_vote_request<std::string>{
                                                 ._term = 1, ._candidate_id = "docker:kythira:1"},
                                             2000ms)
                          .get(),
                      kythira::grpc_client_error);
    BOOST_TEST(handled.load() == 0);
    server.stop();
}

// An unknown textual target fails naming it.
BOOST_AUTO_TEST_CASE(unknown_textual_target_is_named) {
    folly::CPUThreadPoolExecutor exec(2);
    kythira::grpc_client<types, kythira::aws_ec2_node_id> client({}, {}, kythira::noop_metrics{},
                                                                 exec);
    const auto stranger = ids<kythira::aws_ec2_node_id>::server();
    try {
        std::ignore =
            client
                .send_request_vote(stranger,
                                   kythira::request_vote_request<kythira::aws_ec2_node_id>{
                                       ._candidate_id = stranger},
                                   500ms)
                .get();
        BOOST_FAIL("expected an unknown-target error");
    } catch (const kythira::grpc_client_error& e) {
        BOOST_TEST(std::string(e.what()).find(stranger.to_string()) != std::string::npos);
    }
}

BOOST_AUTO_TEST_SUITE_END()
