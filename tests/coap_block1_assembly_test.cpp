// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#define BOOST_TEST_MODULE coap_block1_assembly_test
#include <boost/test/unit_test.hpp>

// raft/coap_block1_assembly.hpp is the server-side Block1 reassembly the
// libcoap OSCORE path and the libnyoci and cantcoap servers share. The
// backends' suites drive it over the wire; this pins the bookkeeping
// itself, in every build, with no CoAP library installed.
#include <raft/coap_block1_assembly.hpp>

#include <cstddef>
#include <string>
#include <vector>

namespace {

using kythira::block1_assembly;
using kythira::block1_outcome;

constexpr std::size_t block = 16;
constexpr std::size_t max_body = 1024;
constexpr std::size_t max_uploads = 2;

auto bytes(std::size_t count, unsigned char fill) -> std::vector<std::byte> {
    return std::vector<std::byte>(count, static_cast<std::byte>(fill));
}

auto offer(block1_assembly& assembly, const std::string& key, std::uint32_t number, bool more,
           const std::vector<std::byte>& chunk) {
    return assembly.offer(key, number, more, block, chunk, max_body, max_uploads);
}

}  // namespace

BOOST_AUTO_TEST_CASE(blocks_in_order_reassemble_the_body) {
    block1_assembly assembly;
    BOOST_TEST((offer(assembly, "a", 0, true, bytes(block, 1)).outcome == block1_outcome::more));
    BOOST_TEST((offer(assembly, "a", 1, true, bytes(block, 2)).outcome == block1_outcome::more));
    const auto done = offer(assembly, "a", 2, false, bytes(5, 3));
    BOOST_TEST((done.outcome == block1_outcome::complete));
    BOOST_REQUIRE_EQUAL(done.body.size(), 2 * block + 5);
    BOOST_TEST((done.body.front() == std::byte{1}));
    BOOST_TEST((done.body[block] == std::byte{2}));
    BOOST_TEST((done.body.back() == std::byte{3}));
    BOOST_TEST(assembly.open_uploads() == 0U);
}

BOOST_AUTO_TEST_CASE(uploads_under_different_keys_do_not_interleave) {
    block1_assembly assembly;
    (void)offer(assembly, "peer-a|/x", 0, true, bytes(block, 1));
    (void)offer(assembly, "peer-b|/x", 0, true, bytes(block, 9));
    const auto a = offer(assembly, "peer-a|/x", 1, false, bytes(1, 1));
    const auto b = offer(assembly, "peer-b|/x", 1, false, bytes(1, 9));
    BOOST_TEST((a.outcome == block1_outcome::complete));
    BOOST_TEST((b.outcome == block1_outcome::complete));
    BOOST_TEST((a.body.front() == std::byte{1}));
    BOOST_TEST((b.body.front() == std::byte{9}));
}

BOOST_AUTO_TEST_CASE(a_missing_block_is_incomplete_and_forgets_the_upload) {
    block1_assembly assembly;
    (void)offer(assembly, "a", 0, true, bytes(block, 1));
    BOOST_TEST(
        (offer(assembly, "a", 2, true, bytes(block, 1)).outcome == block1_outcome::incomplete));
    BOOST_TEST(assembly.open_uploads() == 0U);
    // Starting mid-way with nothing open is just as incomplete.
    BOOST_TEST((offer(assembly, "b", 1, false, bytes(1, 1)).outcome == block1_outcome::incomplete));
}

BOOST_AUTO_TEST_CASE(block_zero_restarts_an_open_upload) {
    block1_assembly assembly;
    (void)offer(assembly, "a", 0, true, bytes(block, 1));
    (void)offer(assembly, "a", 0, true, bytes(block, 7));
    const auto done = offer(assembly, "a", 1, false, bytes(1, 7));
    BOOST_TEST((done.outcome == block1_outcome::complete));
    BOOST_REQUIRE_EQUAL(done.body.size(), block + 1);
    BOOST_TEST((done.body.front() == std::byte{7}));
}

BOOST_AUTO_TEST_CASE(a_body_past_the_limit_is_too_large) {
    block1_assembly assembly;
    for (std::uint32_t n = 0; n < max_body / block; ++n) {
        BOOST_REQUIRE(
            (offer(assembly, "a", n, true, bytes(block, 1)).outcome == block1_outcome::more));
    }
    BOOST_TEST((offer(assembly, "a", max_body / block, true, bytes(block, 1)).outcome ==
                block1_outcome::too_large));
    BOOST_TEST(assembly.open_uploads() == 0U);
}

BOOST_AUTO_TEST_CASE(too_many_open_uploads_is_busy) {
    block1_assembly assembly;
    (void)offer(assembly, "a", 0, true, bytes(block, 1));
    (void)offer(assembly, "b", 0, true, bytes(block, 1));
    BOOST_TEST((offer(assembly, "c", 0, true, bytes(block, 1)).outcome == block1_outcome::busy));
    // An upload already open still progresses at the limit.
    BOOST_TEST((offer(assembly, "a", 1, false, bytes(1, 1)).outcome == block1_outcome::complete));
    BOOST_TEST((offer(assembly, "c", 0, true, bytes(block, 1)).outcome == block1_outcome::more));
}
