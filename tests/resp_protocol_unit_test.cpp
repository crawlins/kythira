// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file resp_protocol_unit_test.cpp
/// @brief RESP codec tests (.kiro/specs/redis-compatible-kv/ task 1,
///        Requirements 1.2-1.7).

#define BOOST_TEST_MODULE resp_protocol_unit_test
#include <boost/test/unit_test.hpp>

#include <raft/resp_protocol.hpp>

#include <string>
#include <vector>

using namespace kythira;

namespace {

auto argv_of(const resp_command& c) -> std::vector<std::string> {
    return c._argv;
}

}  // namespace

BOOST_AUTO_TEST_CASE(parses_single_multibulk_command, *boost::unit_test::timeout(10)) {
    resp_parser p;
    auto cmds = p.consume("*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n");
    BOOST_REQUIRE_EQUAL(cmds.size(), 1u);
    BOOST_CHECK(argv_of(cmds[0]) == (std::vector<std::string>{"SET", "foo", "bar"}));
    BOOST_CHECK_EQUAL(p.buffered_bytes(), 0u);
}

// Requirement 1.3: redis-rs writes AUTH, SELECT and two CLIENT SETINFO before
// reading, so one read must yield all four, in order.
BOOST_AUTO_TEST_CASE(parses_pipelined_handshake, *boost::unit_test::timeout(10)) {
    resp_parser p;
    std::string wire =
        "*3\r\n$4\r\nAUTH\r\n$7\r\nsccache\r\n$6\r\nsecret\r\n"
        "*2\r\n$6\r\nSELECT\r\n$1\r\n0\r\n"
        "*4\r\n$6\r\nCLIENT\r\n$7\r\nSETINFO\r\n$8\r\nLIB-NAME\r\n$8\r\nredis-rs\r\n"
        "*4\r\n$6\r\nCLIENT\r\n$7\r\nSETINFO\r\n$7\r\nLIB-VER\r\n$5\r\n1.2.0\r\n";
    auto cmds = p.consume(wire);
    BOOST_REQUIRE_EQUAL(cmds.size(), 4u);
    BOOST_CHECK_EQUAL(cmds[0]._argv[0], "AUTH");
    BOOST_CHECK_EQUAL(cmds[1]._argv[0], "SELECT");
    BOOST_CHECK_EQUAL(cmds[2]._argv[2], "LIB-NAME");
    BOOST_CHECK_EQUAL(cmds[3]._argv[3], "1.2.0");
}

BOOST_AUTO_TEST_CASE(reassembles_one_byte_at_a_time, *boost::unit_test::timeout(10)) {
    resp_parser p;
    std::string wire = "*2\r\n$3\r\nGET\r\n$5\r\nhello\r\n*1\r\n$4\r\nPING\r\n";
    std::vector<resp_command> all;
    for (char ch : wire) {
        auto cmds = p.consume(std::string_view(&ch, 1));
        all.insert(all.end(), cmds.begin(), cmds.end());
    }
    BOOST_REQUIRE_EQUAL(all.size(), 2u);
    BOOST_CHECK(argv_of(all[0]) == (std::vector<std::string>{"GET", "hello"}));
    BOOST_CHECK(argv_of(all[1]) == (std::vector<std::string>{"PING"}));
    BOOST_CHECK_EQUAL(p.buffered_bytes(), 0u);
}

BOOST_AUTO_TEST_CASE(partial_command_is_retained, *boost::unit_test::timeout(10)) {
    resp_parser p;
    auto cmds = p.consume("*2\r\n$3\r\nGET\r\n$5\r\nhel");
    BOOST_CHECK(cmds.empty());
    BOOST_CHECK_EQUAL(p.buffered_bytes(), std::string("*2\r\n$3\r\nGET\r\n$5\r\nhel").size());
    cmds = p.consume("lo\r\n");
    BOOST_REQUIRE_EQUAL(cmds.size(), 1u);
    BOOST_CHECK_EQUAL(cmds[0]._argv[1], "hello");
}

BOOST_AUTO_TEST_CASE(bulk_strings_carry_binary_bytes, *boost::unit_test::timeout(10)) {
    resp_parser p;
    std::string value("\x00\x01\r\n\xff", 5);
    std::string wire = "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$5\r\n" + value + "\r\n";
    auto cmds = p.consume(wire);
    BOOST_REQUIRE_EQUAL(cmds.size(), 1u);
    BOOST_CHECK(cmds[0]._argv[2] == value);
}

BOOST_AUTO_TEST_CASE(empty_bulk_string, *boost::unit_test::timeout(10)) {
    resp_parser p;
    auto cmds = p.consume("*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$0\r\n\r\n");
    BOOST_REQUIRE_EQUAL(cmds.size(), 1u);
    BOOST_CHECK_EQUAL(cmds[0]._argv[2], "");
}

BOOST_AUTO_TEST_CASE(inline_command_accepted, *boost::unit_test::timeout(10)) {
    resp_parser p;
    auto cmds = p.consume("PING\r\nGET  foo\r\n");
    BOOST_REQUIRE_EQUAL(cmds.size(), 2u);
    BOOST_CHECK(argv_of(cmds[0]) == (std::vector<std::string>{"PING"}));
    BOOST_CHECK(argv_of(cmds[1]) == (std::vector<std::string>{"GET", "foo"}));
}

// Requirement 1.6: each limit is a protocol error, not a crash or a hang.
BOOST_AUTO_TEST_CASE(rejects_oversized_bulk, *boost::unit_test::timeout(10)) {
    resp_parser_limits limits;
    limits._max_bulk_len = 8;
    resp_parser p(limits);
    BOOST_CHECK_THROW(p.consume("*2\r\n$3\r\nSET\r\n$9\r\n"), resp_protocol_error);
}

BOOST_AUTO_TEST_CASE(rejects_too_many_elements, *boost::unit_test::timeout(10)) {
    resp_parser_limits limits;
    limits._max_multibulk_elements = 3;
    resp_parser p(limits);
    BOOST_CHECK_THROW(p.consume("*4\r\n"), resp_protocol_error);
}

BOOST_AUTO_TEST_CASE(rejects_buffer_growth, *boost::unit_test::timeout(10)) {
    resp_parser_limits limits;
    limits._max_buffered_bytes = 16;
    resp_parser p(limits);
    BOOST_CHECK_NO_THROW(p.consume("*2\r\n$3\r\nGET\r\n"));
    BOOST_CHECK_THROW(p.consume("$100\r\naaaaaaaaaaaaaaaaaaaaaa"), resp_protocol_error);
}

BOOST_AUTO_TEST_CASE(rejects_malformed_length, *boost::unit_test::timeout(10)) {
    resp_parser p;
    BOOST_CHECK_THROW(p.consume("*x\r\n"), resp_protocol_error);
    resp_parser q;
    BOOST_CHECK_THROW(q.consume("*1\r\n$abc\r\n"), resp_protocol_error);
    resp_parser r;
    BOOST_CHECK_THROW(r.consume("*1\r\n:5\r\n"), resp_protocol_error);
    resp_parser s;
    BOOST_CHECK_THROW(s.consume("*1\r\n$2\r\nabc\r\n"), resp_protocol_error);
}

BOOST_AUTO_TEST_CASE(rejects_bare_lf, *boost::unit_test::timeout(10)) {
    resp_parser p;
    BOOST_CHECK_THROW(p.consume("*1\n"), resp_protocol_error);
}

// Audit H3: a request trickled in small reads must cost time linear in its
// size. The first parser restarted from the command's first byte on every
// read and re-copied each finished argument, so a 32 MiB argument followed by
// a 30 MiB one arriving 4 KiB at a time (the audit's PoC shape) cost ~7700 x
// 32 MiB of copying; this one finishes in well under the timeout.
BOOST_AUTO_TEST_CASE(trickled_large_request_is_linear, *boost::unit_test::timeout(10)) {
    resp_parser p;
    const std::size_t big = 32u * 1024u * 1024u;
    const std::size_t second = 30u * 1024u * 1024u;
    const std::string chunk(4096, 'x');
    BOOST_CHECK(p.consume("*3\r\n$3\r\nSET\r\n$" + std::to_string(big) + "\r\n").empty());
    for (std::size_t sent = 0; sent < big; sent += chunk.size()) {
        BOOST_REQUIRE(p.consume(chunk).empty());
    }
    BOOST_CHECK(p.consume("\r\n$" + std::to_string(second) + "\r\n").empty());
    std::vector<resp_command> cmds;
    for (std::size_t sent = 0; sent < second; sent += chunk.size()) {
        cmds = p.consume(chunk);
        BOOST_REQUIRE(cmds.empty());
    }
    cmds = p.consume("\r\n");
    BOOST_REQUIRE_EQUAL(cmds.size(), 1u);
    BOOST_CHECK_EQUAL(cmds[0]._argv[1].size(), big);
    BOOST_CHECK_EQUAL(cmds[0]._argv[2].size(), second);
    BOOST_CHECK_EQUAL(p.buffered_bytes(), 0u);
}

// The decoded part of a split command still counts against the buffer cap,
// so finishing arguments early does not let a client exceed it.
BOOST_AUTO_TEST_CASE(buffer_cap_counts_decoded_arguments, *boost::unit_test::timeout(10)) {
    resp_parser_limits limits;
    limits._max_buffered_bytes = 64;
    resp_parser p(limits);
    BOOST_CHECK(p.consume("*4\r\n$20\r\n" + std::string(20, 'a') + "\r\n").empty());
    BOOST_CHECK(p.consume("$20\r\n" + std::string(20, 'b') + "\r\n").empty());
    BOOST_CHECK_GE(p.buffered_bytes(), 50u);
    BOOST_CHECK_THROW(p.consume("$20\r\n" + std::string(20, 'c') + "\r\n"), resp_protocol_error);
}

// An unterminated header line is bounded like an inline one; without the
// bound every read rescanned the whole growing line for its CRLF.
BOOST_AUTO_TEST_CASE(rejects_unterminated_header_line, *boost::unit_test::timeout(10)) {
    resp_parser p;
    BOOST_CHECK(p.consume("*" + std::string(60000, '1')).empty());
    BOOST_CHECK_THROW(p.consume(std::string(10000, '1')), resp_protocol_error);
    resp_parser q;
    BOOST_CHECK(q.consume("*1\r\n$" + std::string(60000, '1')).empty());
    BOOST_CHECK_THROW(q.consume(std::string(10000, '1')), resp_protocol_error);
    resp_parser r;
    BOOST_CHECK_THROW(r.consume("PING" + std::string(70000, ' ')), resp_protocol_error);
}

BOOST_AUTO_TEST_CASE(rejects_length_overflow, *boost::unit_test::timeout(10)) {
    resp_parser p;
    BOOST_CHECK_THROW(p.consume("*99999999999999999999\r\n"), resp_protocol_error);
    resp_parser q;
    BOOST_CHECK_THROW(q.consume("*1\r\n$9223372036854775808\r\n"), resp_protocol_error);
}

BOOST_AUTO_TEST_CASE(pre_auth_limits_match_redis, *boost::unit_test::timeout(10)) {
    resp_parser_limits configured;
    auto pre = resp_pre_auth_limits(configured);
    BOOST_CHECK_EQUAL(pre._max_multibulk_elements, 10u);
    BOOST_CHECK_EQUAL(pre._max_bulk_len, 16384u);
    BOOST_CHECK_LE(pre._max_buffered_bytes, 512u * 1024u);
    // Never looser than what the operator configured.
    configured._max_bulk_len = 1024;
    BOOST_CHECK_EQUAL(resp_pre_auth_limits(configured)._max_bulk_len, 1024u);

    resp_parser p(pre);
    // HELLO 3 AUTH user pass SETNAME name still fits.
    BOOST_CHECK_EQUAL(
        p.consume(
             "*7\r\n$5\r\nHELLO\r\n$1\r\n3\r\n$4\r\nAUTH\r\n$1\r\nu\r\n$1\r\np\r\n$7\r\nSETNAME\r\n"
             "$1\r\nn\r\n")
            .size(),
        1u);
    BOOST_CHECK_THROW(p.consume("*11\r\n"), resp_protocol_error);
    resp_parser q(pre);
    BOOST_CHECK_THROW(q.consume("*2\r\n$4\r\nAUTH\r\n$16385\r\n"), resp_protocol_error);
}

// The gateway parses one command at a time before AUTH and relaxes the
// limits once it succeeds, so a pipelined AUTH + large SET is accepted.
BOOST_AUTO_TEST_CASE(limits_change_between_commands, *boost::unit_test::timeout(10)) {
    resp_parser_limits full;
    resp_parser p(resp_pre_auth_limits(full));
    std::string value(100000, 'v');
    p.feed("*3\r\n$4\r\nAUTH\r\n$1\r\nu\r\n$1\r\np\r\n*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$" +
           std::to_string(value.size()) + "\r\n");
    auto auth = p.next();
    BOOST_REQUIRE(auth.has_value());
    BOOST_CHECK_EQUAL(auth->_argv[0], "AUTH");
    p.set_limits(full);
    BOOST_CHECK(!p.next().has_value());
    p.feed(value + "\r\n");
    auto set = p.next();
    BOOST_REQUIRE(set.has_value());
    BOOST_CHECK(set->_argv[2] == value);
    BOOST_CHECK(!p.next().has_value());

    // Under the pre-auth limits the same SET is a protocol error.
    resp_parser q(resp_pre_auth_limits(full));
    q.feed("*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$100000\r\n");
    BOOST_CHECK_THROW(q.next(), resp_protocol_error);
}

// Audit M7: the forwarding client's reply scanner is bounded.
BOOST_AUTO_TEST_CASE(reply_length_measures_and_waits, *boost::unit_test::timeout(10)) {
    BOOST_CHECK_EQUAL(resp_reply_length("+OK\r\n"), 5u);
    BOOST_CHECK_EQUAL(resp_reply_length("+OK\r\nextra"), 5u);
    BOOST_CHECK_EQUAL(resp_reply_length("$3\r\nab"), 0u);
    BOOST_CHECK_EQUAL(resp_reply_length("$3\r\nabc\r\n"), 9u);
    BOOST_CHECK_EQUAL(resp_reply_length("$-1\r\n"), 5u);
    BOOST_CHECK_EQUAL(resp_reply_length("*2\r\n:1\r\n$1\r\na\r\n"), 15u);
    BOOST_CHECK_EQUAL(resp_reply_length("*2\r\n:1\r\n"), 0u);
    BOOST_CHECK_EQUAL(resp_reply_length("%1\r\n+a\r\n:1\r\n"), 12u);
}

BOOST_AUTO_TEST_CASE(reply_length_enforces_limits, *boost::unit_test::timeout(10)) {
    // Nesting beyond the depth limit (the scanner used to recurse freely).
    std::string deep;
    for (int i = 0; i < 64; ++i) {
        deep += "*1\r\n";
    }
    deep += ":1\r\n";
    BOOST_CHECK_THROW((void)resp_reply_length(deep), resp_protocol_error);
    // A length that overflowed into a short bogus total.
    BOOST_CHECK_THROW((void)resp_reply_length("$18446744073709551615\r\nab\r\n"),
                      resp_protocol_error);
    BOOST_CHECK_THROW((void)resp_reply_length("*99999999999\r\n"), resp_protocol_error);
    resp_reply_limits small;
    small._max_bulk_len = 4;
    BOOST_CHECK_THROW((void)resp_reply_length("$5\r\nabcde\r\n", small), resp_protocol_error);
    // An unterminated line is refused once longer than any legal one,
    // instead of being rescanned on every read.
    BOOST_CHECK_EQUAL(resp_reply_length("+" + std::string(1000, 'a')), 0u);
    BOOST_CHECK_THROW((void)resp_reply_length("+" + std::string(70000, 'a')), resp_protocol_error);
    BOOST_CHECK_THROW((void)resp_reply_length("$-5\r\n"), resp_protocol_error);
}

// Requirement 1.4/1.5: reply encodings, including the RESP3 null switch.
BOOST_AUTO_TEST_CASE(writer_resp2_encodings, *boost::unit_test::timeout(10)) {
    resp_writer w;
    BOOST_CHECK_EQUAL(w.simple_string("PONG"), "+PONG\r\n");
    BOOST_CHECK_EQUAL(w.error("ERR boom"), "-ERR boom\r\n");
    BOOST_CHECK_EQUAL(w.integer(-2), ":-2\r\n");
    BOOST_CHECK_EQUAL(w.bulk("hello"), "$5\r\nhello\r\n");
    BOOST_CHECK_EQUAL(w.bulk(""), "$0\r\n\r\n");
    BOOST_CHECK_EQUAL(w.null(), "$-1\r\n");
    BOOST_CHECK_EQUAL(w.array({w.bulk("a"), w.integer(1)}), "*2\r\n$1\r\na\r\n:1\r\n");
    BOOST_CHECK_EQUAL(w.map({{"server", w.bulk("kythira")}}),
                      "*2\r\n$6\r\nserver\r\n$7\r\nkythira\r\n");
}

BOOST_AUTO_TEST_CASE(writer_resp3_encodings, *boost::unit_test::timeout(10)) {
    resp_writer w;
    w.set_version(3);
    BOOST_CHECK_EQUAL(w.null(), "_\r\n");
    BOOST_CHECK_EQUAL(w.map({{"proto", w.integer(3)}}), "%1\r\n$5\r\nproto\r\n:3\r\n");
    // Bulk and simple replies are the same on both versions.
    BOOST_CHECK_EQUAL(w.bulk("x"), "$1\r\nx\r\n");
    BOOST_CHECK_EQUAL(w.simple_string("OK"), "+OK\r\n");
}

BOOST_AUTO_TEST_CASE(case_insensitive_command_match, *boost::unit_test::timeout(10)) {
    BOOST_CHECK(resp_iequals("get", "GET"));
    BOOST_CHECK(resp_iequals("SeTeX", "SETEX"));
    BOOST_CHECK(!resp_iequals("GET", "GETRANGE"));
    BOOST_CHECK_EQUAL(resp_to_upper("client"), "CLIENT");
}
