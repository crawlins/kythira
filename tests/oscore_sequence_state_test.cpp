// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#include "test_timeout_scale.hpp"
#define BOOST_TEST_MODULE oscore_sequence_state_test
#include <boost/test/unit_test.hpp>

// The two OSCORE counters that must survive a Security Context: the Sender
// Sequence Number, whose reuse under one key reuses an AES-CCM nonce, and the
// replay window, which must not forget what it accepted when the context is
// rebuilt.
//
// A "restart" here is a second security_context built on a fresh
// file_sequence_store over the same directory: nothing in memory survives, only
// what reached the disk.
#include <raft/coap_exceptions.hpp>
#include <raft/coap_security.hpp>
#include <raft/oscore.hpp>
#include <raft/oscore_sequence_store.hpp>

#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

namespace osc = kythira::oscore;

auto bytes(std::initializer_list<unsigned char> values) -> std::vector<std::byte> {
    std::vector<std::byte> out;
    for (const auto value : values) {
        out.push_back(static_cast<std::byte>(value));
    }
    return out;
}

auto client_credentials() -> kythira::oscore_credentials {
    kythira::oscore_credentials credentials;
    credentials.sender_id = bytes({0x00});
    credentials.recipient_id = bytes({0x01});
    credentials.master_secret = bytes({0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xAA,
                                       0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00});
    credentials.master_salt = bytes({0x01, 0x02, 0x03, 0x04});
    return credentials;
}

auto server_credentials() -> kythira::oscore_credentials {
    auto credentials = client_credentials();
    credentials.sender_id = bytes({0x01});
    credentials.recipient_id = bytes({0x00});
    return credentials;
}

auto sample_request() -> osc::coap_message {
    osc::coap_message message;
    message.code = 0x02;  // POST
    message.message_id = 0x0101;
    message.token = bytes({0x0A});
    message.options.push_back({11, bytes({'r', 'a', 'f', 't'})});
    message.payload = bytes({'h', 'i'});
    message.has_payload = true;
    return message;
}

auto partial_iv_of(const osc::coap_message& protected_message) -> std::uint64_t {
    for (const auto& option : protected_message.options) {
        if (option.number == osc::coap_option_oscore) {
            return osc::detail::decode_partial_iv(osc::decode_option(option.value).partial_iv);
        }
    }
    throw std::runtime_error("no OSCORE option");
}

auto fresh_store() -> std::shared_ptr<osc::sequence_store> {
    return std::make_shared<osc::memory_sequence_store>();
}

// A unique, initially absent directory, removed afterwards.
struct temp_dir {
    temp_dir() {
        static int counter = 0;
        path = std::filesystem::temp_directory_path() /
               ("kythira-oscore-state-" + std::to_string(::getpid()) + "-" +
                std::to_string(counter++));
        std::filesystem::remove_all(path);
    }
    temp_dir(const temp_dir&) = delete;
    auto operator=(const temp_dir&) -> temp_dir& = delete;
    ~temp_dir() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
    std::filesystem::path path;
};

}  // namespace

BOOST_AUTO_TEST_SUITE(oscore_sequence_state_tests)

// ── Sender Sequence Number ─────────────────────────────────────────────────

// Two contexts on the same key in one process -- a server's stop()/start(), or
// a client and server built from the same credentials -- used to both start at
// zero.
BOOST_AUTO_TEST_CASE(contexts_sharing_a_key_never_share_a_partial_iv,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(15))) {
    const auto store = fresh_store();
    const osc::security_context first(client_credentials(), store);
    const osc::security_context second(client_credentials(), store);

    std::set<std::uint64_t> seen;
    osc::request_binding binding;
    for (int i = 0; i < 200; ++i) {
        const auto& sender = (i % 3 == 0) ? first : second;
        const auto piv = partial_iv_of(sender.protect_request(sample_request(), binding));
        BOOST_TEST_REQUIRE(seen.insert(piv).second, "Partial IV " << piv << " issued twice");
    }
}

BOOST_AUTO_TEST_CASE(the_default_store_spans_contexts_in_one_process,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(15))) {
    auto credentials = client_credentials();
    credentials.master_secret.back() = std::byte{0x42};  // a key no other case uses
    osc::request_binding binding;
    std::uint64_t before = 0;
    {
        const osc::security_context first(credentials);
        before = partial_iv_of(first.protect_request(sample_request(), binding));
    }
    const osc::security_context rebuilt(credentials);
    BOOST_TEST(partial_iv_of(rebuilt.protect_request(sample_request(), binding)) > before);
}

BOOST_AUTO_TEST_CASE(a_restart_never_reissues_a_partial_iv,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(15))) {
    const temp_dir dir;
    std::uint64_t highest = 0;
    osc::request_binding binding;
    {
        const osc::security_context before(client_credentials(),
                                           std::make_shared<osc::file_sequence_store>(dir.path));
        for (int i = 0; i < 5000; ++i) {  // crosses a 4096 reservation block
            highest = partial_iv_of(before.protect_request(sample_request(), binding));
        }
    }
    const osc::security_context after(client_credentials(),
                                      std::make_shared<osc::file_sequence_store>(dir.path));
    BOOST_TEST(partial_iv_of(after.protect_request(sample_request(), binding)) > highest);
}

// A crash between reserving and sending loses at most the rest of the block;
// it never hands the same block out twice.
BOOST_AUTO_TEST_CASE(a_reservation_is_durable_before_its_first_number_is_used,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(15))) {
    const temp_dir dir;
    osc::request_binding binding;
    const osc::security_context sender(client_credentials(),
                                       std::make_shared<osc::file_sequence_store>(dir.path));
    const auto first = partial_iv_of(sender.protect_request(sample_request(), binding));

    // What a second process would see right now, before `sender` is done.
    osc::file_sequence_store observer(dir.path);
    const auto key = osc::security_context::state_key(client_credentials(), "SSN",
                                                      client_credentials().sender_id, {});
    BOOST_TEST(observer.load(key) > first);
}

BOOST_AUTO_TEST_CASE(an_unreadable_state_file_is_an_error_not_zero,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(15))) {
    const temp_dir dir;
    osc::file_sequence_store store(dir.path);
    std::ofstream(dir.path / "corrupt") << "not a number";
    BOOST_CHECK_THROW((void)store.load("corrupt"), kythira::coap_security_error);
    std::ofstream(dir.path / "empty");
    BOOST_CHECK_THROW((void)store.reserve("empty", 1), kythira::coap_security_error);
}

BOOST_AUTO_TEST_CASE(an_exhausted_sequence_space_refuses_to_send,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(15))) {
    const auto store = fresh_store();
    const auto key = osc::security_context::state_key(client_credentials(), "SSN",
                                                      client_credentials().sender_id, {});
    store->raise_to(key, 0x10000000000ULL);
    const osc::security_context sender(client_credentials(), store);
    osc::request_binding binding;
    BOOST_CHECK_THROW((void)sender.protect_request(sample_request(), binding),
                      kythira::coap_security_error);
}

// ── Replay state across a restart ──────────────────────────────────────────

BOOST_AUTO_TEST_CASE(a_restarted_server_refuses_a_request_it_accepted_before,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(15))) {
    const temp_dir dir;
    const osc::security_context client(client_credentials(), fresh_store());
    osc::request_binding binding;
    const auto captured = client.protect_request(sample_request(), binding);
    osc::request_binding received;
    {
        osc::security_context before(server_credentials(),
                                     std::make_shared<osc::file_sequence_store>(dir.path));
        BOOST_CHECK_NO_THROW((void)before.unprotect_request(captured, received));
    }
    osc::security_context after(server_credentials(),
                                std::make_shared<osc::file_sequence_store>(dir.path));
    BOOST_CHECK_THROW((void)after.unprotect_request(captured, received), osc::verification_error);
}

// The cost of that floor, bounded: a client that did not restart has at most
// one replay block of its requests refused, then is accepted again.
BOOST_AUTO_TEST_CASE(a_surviving_client_recovers_after_a_server_restart,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(15))) {
    const temp_dir dir;
    const osc::security_context client(client_credentials(), fresh_store());
    osc::request_binding binding;
    osc::request_binding received;
    {
        osc::security_context before(server_credentials(),
                                     std::make_shared<osc::file_sequence_store>(dir.path));
        (void)before.unprotect_request(client.protect_request(sample_request(), binding), received);
    }
    osc::security_context after(server_credentials(),
                                std::make_shared<osc::file_sequence_store>(dir.path));
    int refused = 0;
    bool accepted = false;
    for (int i = 0; i < 200 && !accepted; ++i) {
        try {
            (void)after.unprotect_request(client.protect_request(sample_request(), binding),
                                          received);
            accepted = true;
        } catch (const osc::verification_error&) {
            ++refused;
        }
    }
    BOOST_TEST(accepted);
    BOOST_TEST(refused <= 64);
}

BOOST_AUTO_TEST_SUITE_END()
