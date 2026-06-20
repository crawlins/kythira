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

BOOST_AUTO_TEST_SUITE_END()
