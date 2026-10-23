// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#define BOOST_TEST_MODULE fault_control_unit_test
#include <boost/test/unit_test.hpp>

#include "fault_control.hpp"
#include "harness.hpp"

// ── fiu-ctrl command string construction ─────────────────────────────────────

BOOST_AUTO_TEST_SUITE(fiu_cmd_builders)

BOOST_AUTO_TEST_CASE(enable_always_format) {
    auto cmd = docker_chaos::fiu::build_enable_always_cmd("raft/network/send_request_vote");
    BOOST_TEST(cmd == "enable name=raft/network/send_request_vote failnum=-1 failinfo=0");
}

BOOST_AUTO_TEST_CASE(enable_random_format) {
    auto cmd = docker_chaos::fiu::build_enable_random_cmd("raft/persistence/append_log_entry", 0.3);
    BOOST_TEST(cmd.substr(0, 13) == "enable_random");
    BOOST_TEST(cmd.find("name=raft/persistence/append_log_entry") != std::string::npos);
    BOOST_TEST(cmd.find("probability=0.300000") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(enable_once_format) {
    auto cmd = docker_chaos::fiu::build_enable_once_cmd("raft/persistence/save_current_term");
    BOOST_TEST(cmd.find("failnum=1") != std::string::npos);
    BOOST_TEST(cmd.find("one") != std::string::npos);
    BOOST_TEST(cmd.find("name=raft/persistence/save_current_term") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(disable_format) {
    auto cmd = docker_chaos::fiu::build_disable_cmd("raft/network/send_append_entries");
    BOOST_TEST(cmd == "disable name=raft/network/send_append_entries");
}

BOOST_AUTO_TEST_CASE(disable_all_format) {
    BOOST_TEST(docker_chaos::fiu::build_disable_all_cmd() == "disable_all");
}

// All fault point constants must be non-empty and start with "raft/"
BOOST_AUTO_TEST_CASE(fault_point_constants_well_formed) {
    using namespace docker_chaos::fiu;
    for (auto name :
         {SEND_REQUEST_VOTE, SEND_APPEND_ENTRIES, SEND_INSTALL_SNAPSHOT, SAVE_CURRENT_TERM,
          SAVE_VOTED_FOR, APPEND_LOG_ENTRY, TRUNCATE_LOG, SAVE_SNAPSHOT, STATE_MACHINE_APPLY}) {
        BOOST_TEST(!name.empty());
        BOOST_TEST(name.substr(0, 5) == "raft/");
    }
}

BOOST_AUTO_TEST_SUITE_END()

// ── HTTP response parsers ─────────────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(http_parsers)

BOOST_AUTO_TEST_CASE(parse_health_200_true) {
    docker_chaos::HttpResult r{200, R"({"status":"running"})"};
    BOOST_TEST(docker_chaos::parse_health(r) == true);
}

BOOST_AUTO_TEST_CASE(parse_health_503_false) {
    docker_chaos::HttpResult r{503, R"({"status":"stopped"})"};
    BOOST_TEST(docker_chaos::parse_health(r) == false);
}

BOOST_AUTO_TEST_CASE(parse_health_connection_failed_false) {
    docker_chaos::HttpResult r{0, "connection failed"};
    BOOST_TEST(docker_chaos::parse_health(r) == false);
}

BOOST_AUTO_TEST_CASE(parse_status_extracts_fields) {
    docker_chaos::HttpResult r{200, R"({"node_id":1,"role":"leader","term":4,
                                        "commit_index":12,"last_applied":12})"};
    auto obj = docker_chaos::parse_status(r);
    BOOST_TEST(obj["role"].as_string() == "leader");
    BOOST_TEST(obj["term"].as_int64() == 4);
    BOOST_TEST(obj["commit_index"].as_int64() == 12);
}

BOOST_AUTO_TEST_CASE(parse_status_non_200_throws) {
    docker_chaos::HttpResult r{503, ""};
    BOOST_CHECK_THROW(docker_chaos::parse_status(r), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(parse_command_response_success) {
    docker_chaos::HttpResult r{200, R"({"success":true,"commit_index":5})"};
    auto obj = docker_chaos::parse_command_response(r);
    BOOST_TEST(obj["success"].as_bool() == true);
    BOOST_TEST(obj["commit_index"].as_int64() == 5);
}

BOOST_AUTO_TEST_CASE(parse_command_response_not_leader) {
    docker_chaos::HttpResult r{503, R"({"error":"not_leader","leader_id":2})"};
    auto obj = docker_chaos::parse_command_response(r);
    BOOST_TEST(obj["error"].as_string() == "not_leader");
    BOOST_TEST(obj["leader_id"].as_int64() == 2);
}

BOOST_AUTO_TEST_CASE(parse_log_entry_extracts_fields) {
    docker_chaos::HttpResult r{200, R"({"index":3,"term":2,"command":""})"};
    auto obj = docker_chaos::parse_log_entry(r);
    BOOST_TEST(obj["index"].as_int64() == 3);
    BOOST_TEST(obj["term"].as_int64() == 2);
}

BOOST_AUTO_TEST_CASE(parse_log_entry_404_throws_out_of_range) {
    docker_chaos::HttpResult r{404, ""};
    BOOST_CHECK_THROW(docker_chaos::parse_log_entry(r), std::out_of_range);
}

BOOST_AUTO_TEST_CASE(parse_log_entry_non_200_throws_runtime_error) {
    docker_chaos::HttpResult r{500, ""};
    BOOST_CHECK_THROW(docker_chaos::parse_log_entry(r), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(parse_log_entry_404_names_the_reason) {
    docker_chaos::HttpResult r{404, R"({"error":"compacted"})"};
    try {
        (void)docker_chaos::parse_log_entry(r);
        BOOST_FAIL("expected out_of_range");
    } catch (const std::out_of_range& e) {
        BOOST_TEST(std::string(e.what()).find("compacted") != std::string::npos);
    }
}

BOOST_AUTO_TEST_CASE(parse_log_range_extracts_entries) {
    docker_chaos::HttpResult r{
        200, R"({"from":1,"to":2,"snapshot_index":0,"last_log_index":2,"entries":[)"
             R"({"index":1,"term":1,"type":"no_op","command_b64":""},)"
             R"({"index":2,"term":1,"type":"normal","command_b64":"AQ=="}]})"};
    auto obj = docker_chaos::parse_log_range(r);
    BOOST_TEST(obj["entries"].as_array().size() == 2U);
    BOOST_TEST(obj["entries"].as_array()[1].as_object().at("type").as_string() == "normal");
}

BOOST_AUTO_TEST_CASE(parse_log_range_400_throws) {
    docker_chaos::HttpResult r{400, R"({"error":"bad_range","detail":"missing from"})"};
    BOOST_CHECK_THROW(docker_chaos::parse_log_range(r), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(parse_elections_extracts_terms) {
    docker_chaos::HttpResult r{200, R"({"node_id":2,"elections":[{"term":3,"at_ms":10},)"
                                    R"({"term":5,"at_ms":99}]})"};
    auto obj = docker_chaos::parse_elections(r);
    auto& list = obj["elections"].as_array();
    BOOST_REQUIRE(list.size() == 2U);
    BOOST_TEST(list[0].as_object().at("term").as_int64() == 3);
    BOOST_TEST(list[1].as_object().at("term").as_int64() == 5);
}

BOOST_AUTO_TEST_CASE(parse_elections_non_200_throws) {
    BOOST_CHECK_THROW(docker_chaos::parse_elections({0, "connection failed"}), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(parse_kv_read_found_and_absent) {
    auto found = docker_chaos::parse_kv_read(
        {200, R"({"key":"k","mode":"local","found":true,"value":"v","commit_index":4,)"
              R"("last_applied":4})"});
    BOOST_TEST(found["value"].as_string() == "v");
    auto absent = docker_chaos::parse_kv_read(
        {200, R"({"key":"k","mode":"linearizable","found":false,"value":null,)"
              R"("commit_index":4,"last_applied":4})"});
    BOOST_TEST(absent["value"].is_null());
    BOOST_CHECK_THROW(docker_chaos::parse_kv_read({503, R"({"error":"read_failed"})"}),
                      std::runtime_error);
}

BOOST_AUTO_TEST_CASE(parse_membership_response_keeps_refusals) {
    auto ok =
        docker_chaos::parse_membership_response({200, R"({"success":true,"commit_index":9})"});
    BOOST_TEST(ok["success"].as_bool());
    BOOST_TEST(ok["http_status"].as_int64() == 200);
    auto busy = docker_chaos::parse_membership_response(
        {409, R"({"error":"change_in_progress","detail":"x"})"});
    BOOST_TEST(busy["error"].as_string() == "change_in_progress");
    BOOST_TEST(busy["http_status"].as_int64() == 409);
    BOOST_CHECK_THROW(docker_chaos::parse_membership_response({0, ""}), std::runtime_error);
}

// The CAS body is built with boost::json, so a key or value with a quote in it
// still reaches chaos_node as the same string.
BOOST_AUTO_TEST_CASE(submit_cas_body_round_trips) {
    std::string posted_url;
    std::string posted_body;
    docker_chaos::ChaosNode node(
        1, [](const std::vector<std::string>&) { return docker_chaos::os::CmdResult{}; },
        [](const std::string&) { return docker_chaos::HttpResult{200, "{}"}; },
        [&](const std::string& url, const std::string& body) {
            posted_url = url;
            posted_body = body;
            return docker_chaos::HttpResult{200, R"({"success":true,"applied":false})"};
        });
    auto reply = node.submit_cas("k\"1", std::nullopt, "v");
    BOOST_TEST(!reply["applied"].as_bool());
    BOOST_TEST(posted_url.find("/command") != std::string::npos);
    auto body = boost::json::parse(posted_body).as_object();
    BOOST_TEST(body.at("op").as_string() == "cas");
    BOOST_TEST(body.at("key").as_string() == "k\"1");
    BOOST_TEST(body.at("expected").is_null());

    (void)node.submit_cas("k", std::string("old"), "new");
    BOOST_TEST(boost::json::parse(posted_body).as_object().at("expected").as_string() == "old");
}

BOOST_AUTO_TEST_SUITE_END()
