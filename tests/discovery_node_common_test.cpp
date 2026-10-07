// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#define BOOST_TEST_MODULE DiscoveryNodeCommonTest
#include <boost/test/unit_test.hpp>

#include "peers_endpoint.hpp"
#include "tsig_key_dir.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <unistd.h>

namespace dn = kythira::discovery_node;

namespace {

struct fake_peer {
    std::string node_id;
    std::string address;
};

// A fresh directory under the system temp dir, removed on scope exit.
struct temp_dir {
    std::filesystem::path path;
    temp_dir() {
        path = std::filesystem::temp_directory_path() /
               ("discovery_node_common_test." + std::to_string(::getpid()) + "." +
                std::to_string(counter()++));
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
    }
    ~temp_dir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    temp_dir(const temp_dir&) = delete;
    auto operator=(const temp_dir&) -> temp_dir& = delete;
    void write(const char* leaf, std::string_view content) const {
        std::ofstream(path / leaf) << content;
    }
    static auto counter() -> int& {
        static int n = 0;
        return n;
    }
};

// parse_peers_timeout() as a count Boost.Test can print; -1 for rejected.
auto timeout_ms(std::optional<std::string_view> param) -> long long {
    const auto t = dn::parse_peers_timeout(param);
    return t ? t->count() : -1;
}

}  // namespace

BOOST_AUTO_TEST_SUITE(peers_timeout)

BOOST_AUTO_TEST_CASE(absent_parameter_gives_default, *boost::unit_test::timeout(10)) {
    BOOST_TEST(timeout_ms(std::nullopt) == dn::default_peers_timeout.count());
}

BOOST_AUTO_TEST_CASE(in_range_value_is_kept, *boost::unit_test::timeout(10)) {
    BOOST_TEST(timeout_ms("3000") == 3000);
    BOOST_TEST(timeout_ms("1") == 1);
    BOOST_TEST(timeout_ms("10000") == dn::max_peers_timeout.count());
}

// The old handler passed std::stoi's result straight to find_peers(), so
// "2147483647" held an HTTP worker for about 24 days.
BOOST_AUTO_TEST_CASE(large_values_are_clamped_to_the_maximum, *boost::unit_test::timeout(10)) {
    BOOST_TEST(timeout_ms("10001") == dn::max_peers_timeout.count());
    BOOST_TEST(timeout_ms("2147483647") == dn::max_peers_timeout.count());
    BOOST_TEST(timeout_ms("99999999999999999999999") == dn::max_peers_timeout.count());
}

BOOST_AUTO_TEST_CASE(zero_and_negative_values_are_clamped_to_one, *boost::unit_test::timeout(10)) {
    BOOST_TEST(timeout_ms("0") == 1);
    BOOST_TEST(timeout_ms("-5") == 1);
    BOOST_TEST(timeout_ms("-99999999999999999999999") == 1);
}

BOOST_AUTO_TEST_CASE(non_integers_are_rejected, *boost::unit_test::timeout(10)) {
    for (const std::string_view bad : {"", "abc", "12abc", "1.5", " 100", "100 ", "+100", "0x10"}) {
        BOOST_TEST_CONTEXT("timeout_ms=\"" << bad << "\"") {
            BOOST_TEST(timeout_ms(bad) == -1);
        }
    }
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(peers_json)

BOOST_AUTO_TEST_CASE(empty_list_is_an_empty_array, *boost::unit_test::timeout(10)) {
    BOOST_TEST(dn::peers_to_json(std::vector<fake_peer>{}) == "[]");
}

BOOST_AUTO_TEST_CASE(plain_values_render_unchanged, *boost::unit_test::timeout(10)) {
    const std::vector<fake_peer> peers{{"node1", "10.0.0.1"}, {"node2", "host-2:7031"}};
    BOOST_TEST(dn::peers_to_json(peers) ==
               R"([{"id":"node1","address":"10.0.0.1"},{"id":"node2","address":"host-2:7031"}])");
}

// A peer id comes from a DNS label or TXT record anyone who can update the
// zone controls. Unescaped, this one closed the string and added a field.
BOOST_AUTO_TEST_CASE(quotes_and_backslashes_are_escaped, *boost::unit_test::timeout(10)) {
    const std::vector<fake_peer> peers{{R"(x","admin":true,"y":")", R"(a\b)"}};
    BOOST_TEST(dn::peers_to_json(peers) ==
               R"([{"id":"x\",\"admin\":true,\"y\":\"","address":"a\\b"}])");
}

BOOST_AUTO_TEST_CASE(control_characters_are_escaped, *boost::unit_test::timeout(10)) {
    const std::vector<fake_peer> peers{{std::string("a\nb\tc\x01\x1f", 7), "\r\b\f"}};
    BOOST_TEST(dn::peers_to_json(peers) == R"([{"id":"a\nb\tc\u0001\u001f","address":"\r\b\f"}])");
}

BOOST_AUTO_TEST_CASE(utf8_passes_through, *boost::unit_test::timeout(10)) {
    const std::vector<fake_peer> peers{{"n\xc3\xa9ud", "h"}};
    BOOST_TEST(dn::peers_to_json(peers) == "[{\"id\":\"n\xc3\xa9ud\",\"address\":\"h\"}]");
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(tsig_key_dir)

BOOST_AUTO_TEST_CASE(reads_name_and_secret, *boost::unit_test::timeout(10)) {
    temp_dir dir;
    dir.write("name", "kythira-update.");
    dir.write("secret", "c2VjcmV0c2VjcmV0c2VjcmV0c2VjcmV0c2VjcmV0MTI=\n");
    const auto key = dn::read_tsig_key_dir(dir.path);
    BOOST_TEST(key.name == "kythira-update.");
    BOOST_TEST(key.secret_base64 == "c2VjcmV0c2VjcmV0c2VjcmV0c2VjcmV0c2VjcmV0MTI=");
}

BOOST_AUTO_TEST_CASE(missing_file_throws_naming_it, *boost::unit_test::timeout(10)) {
    temp_dir dir;
    dir.write("name", "kythira-update.");
    try {
        (void)dn::read_tsig_key_dir(dir.path);
        BOOST_FAIL("expected std::runtime_error");
    } catch (const std::runtime_error& ex) {
        BOOST_TEST(std::string(ex.what()).find("secret") != std::string::npos);
    }
}

BOOST_AUTO_TEST_CASE(empty_file_throws, *boost::unit_test::timeout(10)) {
    temp_dir dir;
    dir.write("name", "\n");
    dir.write("secret", "abc");
    BOOST_CHECK_THROW(static_cast<void>(dn::read_tsig_key_dir(dir.path)), std::runtime_error);
}

BOOST_AUTO_TEST_SUITE_END()
