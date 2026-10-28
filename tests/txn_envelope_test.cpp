// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file txn_envelope_test.cpp
/// @brief The transaction envelope and result wire formats (task 1 of
///        `.kiro/specs/multi-raft-transactions/`).
///
/// Every op is round-tripped and compared field by field (each payload struct
/// has a defaulted `==`), for a textual and a numeric key type. Then every
/// encoding is cut at every length short of its full size, and each prefix
/// must be refused. That is the property that matters for a log format: a
/// decoder that read past its end would fail on every replica at once.
/// The garbage cases forge the fields a hostile or corrupt entry would forge:
/// counts and lengths too large for the buffer, lengths that would wrap a
/// 64-bit offset, unknown op and status codes, and trailing bytes.

#define BOOST_TEST_MODULE txn_envelope_test
#include <boost/test/unit_test.hpp>

#include <raft/txn_envelope.hpp>

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

using kythira::decode_txn_envelope;
using kythira::decode_txn_result;
using kythira::encode_txn_envelope;
using kythira::encode_txn_result;
using kythira::isolation_unsupported_exception;
using kythira::key_locked_exception;
using kythira::serialization_exception;
using kythira::txn_envelope;
using kythira::txn_id;
using kythira::txn_isolation;
using kythira::txn_record_status;
using kythira::txn_result;
using kythira::txn_result_status;

namespace {

auto bytes(std::initializer_list<int> v) -> std::vector<std::byte> {
    std::vector<std::byte> out;
    for (int b : v) {
        out.push_back(static_cast<std::byte>(b));
    }
    return out;
}

auto text_bytes(const std::string& s) -> std::vector<std::byte> {
    std::vector<std::byte> out;
    for (char c : s) {
        out.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
    }
    return out;
}

constexpr txn_id k_txn{0x0123456789abcdefULL, 0xfedcba9876543210ULL};

/// One envelope of every op, keyed by `Key` through `make_key`.
template<typename Key, typename MakeKey> auto every_op(MakeKey make_key) {
    using namespace kythira;
    std::vector<txn_envelope<Key>> ops;
    ops.emplace_back(txn_plain{text_bytes("SET a 1")});
    ops.emplace_back(txn_one_phase<Key>{
        k_txn,
        {{make_key(1), text_bytes("SET a 1")}, {make_key(2), {}}},
        7,
        9,
    });
    ops.emplace_back(txn_prepare<Key>{
        k_txn,
        make_key(1),
        {{make_key(1), text_bytes("SET a 1")}, {make_key(3), bytes({0, 255, 0})}},
        {make_key(4), make_key(5)},
        11,
        1'700'000'000'000ULL,
        true,
    });
    ops.emplace_back(txn_commit_primary<Key>{k_txn, make_key(1), 42});
    ops.emplace_back(txn_commit_secondary<Key>{k_txn, {make_key(2), make_key(3)}, 42});
    ops.emplace_back(txn_abort<Key>{k_txn, make_key(1), {make_key(1), make_key(2)}});
    ops.emplace_back(txn_heartbeat<Key>{k_txn, make_key(1), 1'700'000'003'000ULL});
    ops.emplace_back(txn_check_and_resolve<Key>{k_txn, make_key(1), 1'700'000'004'000ULL});
    ops.emplace_back(txn_cleanup<Key>{{{make_key(1), k_txn}, {make_key(9), txn_id{1, 2}}}});
    ops.emplace_back(txn_gc{123});
    ops.emplace_back(txn_oracle_reserve{std::numeric_limits<std::uint64_t>::max()});
    return ops;
}

template<typename Key, typename MakeKey> auto every_result(MakeKey make_key) {
    using namespace kythira;
    std::vector<txn_result<Key>> out;
    txn_result<Key> ok;
    ok._payload = text_bytes("OK");
    out.push_back(ok);
    txn_result<Key> empty_ok;
    out.push_back(empty_ok);
    for (auto s : {txn_result_status::write_conflict, txn_result_status::key_locked,
                   txn_result_status::read_conflict}) {
        txn_result<Key> held;
        held._status = s;
        held._key = make_key(3);
        held._holder = txn_lock_holder<Key>{k_txn, make_key(1)};
        out.push_back(held);
        txn_result<Key> unheld;
        unheld._status = s;
        unheld._key = make_key(4);
        out.push_back(unheld);
    }
    for (auto rs :
         {txn_record_status::pending, txn_record_status::committed, txn_record_status::aborted}) {
        txn_result<Key> rec;
        rec._status = txn_result_status::record;
        rec._record = rs;
        rec._commit_ts = 77;
        out.push_back(rec);
    }
    txn_result<Key> too_old;
    too_old._status = txn_result_status::snapshot_too_old;
    too_old._safe_point = 500;
    out.push_back(too_old);
    txn_result<Key> aborted;
    aborted._status = txn_result_status::already_aborted;
    out.push_back(aborted);
    return out;
}

auto string_key(int i) -> std::string {
    return "key-" + std::to_string(i);
}
auto numeric_key(int i) -> std::uint64_t {
    return 1000ULL + static_cast<std::uint64_t>(i);
}

/// Every proper prefix of `encoded` must be refused by `decode`.
template<typename Decode>
auto check_every_prefix_refused(const std::vector<std::byte>& encoded, Decode decode) -> void {
    for (std::size_t n = 0; n < encoded.size(); ++n) {
        const std::vector<std::byte> prefix(encoded.begin(),
                                            encoded.begin() + static_cast<std::ptrdiff_t>(n));
        BOOST_CHECK_THROW(decode(prefix), serialization_exception);
    }
}

auto append_u64(std::vector<std::byte>& out, std::uint64_t v) -> void {
    for (int i = 7; i >= 0; --i) {
        out.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFFU));
    }
}

}  // namespace

BOOST_AUTO_TEST_SUITE(txn_envelope_codec)

BOOST_AUTO_TEST_CASE(every_op_round_trips_with_string_keys) {
    const auto ops = every_op<std::string>(string_key);
    BOOST_TEST(ops.size() == std::variant_size_v<txn_envelope<std::string>>);
    for (const auto& env : ops) {
        const auto encoded = encode_txn_envelope<std::string>(env);
        BOOST_TEST(static_cast<int>(encoded[2]) == static_cast<int>(env.index() + 1));
        BOOST_TEST((decode_txn_envelope<std::string>(encoded) == env));
    }
}

BOOST_AUTO_TEST_CASE(every_op_round_trips_with_numeric_keys) {
    for (const auto& env : every_op<std::uint64_t>(numeric_key)) {
        BOOST_TEST(
            (decode_txn_envelope<std::uint64_t>(encode_txn_envelope<std::uint64_t>(env)) == env));
    }
}

BOOST_AUTO_TEST_CASE(empty_lists_round_trip) {
    using namespace kythira;
    const txn_envelope<std::string> env = txn_prepare<std::string>{k_txn, "p", {}, {}, 0, 0, false};
    BOOST_TEST((decode_txn_envelope<std::string>(encode_txn_envelope<std::string>(env)) == env));
}

BOOST_AUTO_TEST_CASE(every_truncation_of_every_op_is_refused) {
    for (const auto& env : every_op<std::string>(string_key)) {
        check_every_prefix_refused(encode_txn_envelope<std::string>(env), [](const auto& b) {
            return decode_txn_envelope<std::string>(b);
        });
    }
}

BOOST_AUTO_TEST_CASE(trailing_bytes_are_refused) {
    for (const auto& env : every_op<std::string>(string_key)) {
        auto encoded = encode_txn_envelope<std::string>(env);
        encoded.push_back(std::byte{0});
        BOOST_CHECK_THROW(static_cast<void>(decode_txn_envelope<std::string>(encoded)),
                          serialization_exception);
    }
}

BOOST_AUTO_TEST_CASE(wrong_magic_version_and_op_are_refused) {
    const auto good =
        encode_txn_envelope<std::string>(txn_envelope<std::string>{kythira::txn_gc{1}});
    auto wrong_magic = good;
    wrong_magic[0] = std::byte{'J'};
    auto wrong_version = good;
    wrong_version[1] = std::byte{2};
    auto op_zero = good;
    op_zero[2] = std::byte{0};
    auto op_twelve = good;
    op_twelve[2] = std::byte{12};
    for (const auto* b : {&wrong_magic, &wrong_version, &op_zero, &op_twelve}) {
        BOOST_CHECK_THROW(static_cast<void>(decode_txn_envelope<std::string>(*b)),
                          serialization_exception);
    }
    BOOST_TEST(kythira::looks_like_txn_envelope(good));
    BOOST_TEST(!kythira::looks_like_txn_envelope(wrong_magic));
    BOOST_TEST(!kythira::looks_like_txn_envelope(wrong_version));
}

BOOST_AUTO_TEST_CASE(an_unenveloped_command_is_refused) {
    // A plain command written to a transactional host before it was wrapped
    // must not be mistaken for an envelope.
    BOOST_CHECK_THROW(static_cast<void>(decode_txn_envelope<std::string>(text_bytes("SET a 1"))),
                      serialization_exception);
}

BOOST_AUTO_TEST_CASE(a_boolean_other_than_zero_or_one_is_refused) {
    using namespace kythira;
    auto encoded = encode_txn_envelope<std::string>(
        txn_envelope<std::string>{txn_prepare<std::string>{k_txn, "p", {}, {}, 0, 0, true}});
    BOOST_REQUIRE(static_cast<int>(encoded.back()) == 1);
    encoded.back() = std::byte{2};
    BOOST_CHECK_THROW(static_cast<void>(decode_txn_envelope<std::string>(encoded)),
                      serialization_exception);
}

BOOST_AUTO_TEST_CASE(forged_counts_are_refused_before_allocation) {
    // txn_commit_secondary: header, op 5, txn id, then a key-list count.
    auto prefix = bytes({0x4B, 1, 5});
    append_u64(prefix, 1);
    append_u64(prefix, 2);

    // Over the fixed cap.
    auto over_cap = prefix;
    append_u64(over_cap, kythira::k_txn_max_keys + 1);
    // Under the cap, but more keys than the bytes left could hold.
    auto over_remaining = prefix;
    append_u64(over_remaining, 1000);
    append_u64(over_remaining, 0);
    // A count that would overflow any multiplication.
    auto huge = prefix;
    append_u64(huge, std::numeric_limits<std::uint64_t>::max());
    for (const auto* b : {&over_cap, &over_remaining, &huge}) {
        BOOST_CHECK_THROW(static_cast<void>(decode_txn_envelope<std::string>(*b)),
                          serialization_exception);
    }
}

BOOST_AUTO_TEST_CASE(forged_lengths_are_refused_including_one_that_wraps) {
    // txn_plain: header, op 1, then a command length.
    for (std::uint64_t length :
         {std::uint64_t{100}, std::uint64_t{kythira::k_txn_max_command_bytes} + 1,
          std::numeric_limits<std::uint64_t>::max(),
          std::numeric_limits<std::uint64_t>::max() - 2}) {
        auto b = bytes({0x4B, 1, 1});
        append_u64(b, length);
        b.push_back(std::byte{'x'});
        BOOST_CHECK_THROW(static_cast<void>(decode_txn_envelope<std::string>(b)),
                          serialization_exception);
    }
    // A key over its own cap, inside a commit_primary.
    auto b = bytes({0x4B, 1, 4});
    append_u64(b, 1);
    append_u64(b, 2);
    append_u64(b, kythira::k_txn_max_key_bytes + 1);
    BOOST_CHECK_THROW(static_cast<void>(decode_txn_envelope<std::string>(b)),
                      serialization_exception);
}

BOOST_AUTO_TEST_CASE(encode_refuses_what_decode_would_refuse) {
    using namespace kythira;
    const std::string long_key(k_txn_max_key_bytes + 1, 'k');
    BOOST_CHECK_THROW(static_cast<void>(encode_txn_envelope<std::string>(txn_envelope<std::string>{
                          txn_commit_primary<std::string>{k_txn, long_key, 1}})),
                      serialization_exception);
    txn_commit_secondary<std::string> many{k_txn, {}, 1};
    many._keys.assign(k_txn_max_keys + 1, "k");
    BOOST_CHECK_THROW(
        static_cast<void>(encode_txn_envelope<std::string>(txn_envelope<std::string>{many})),
        serialization_exception);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(txn_result_codec)

BOOST_AUTO_TEST_CASE(every_status_round_trips) {
    for (const auto& res : every_result<std::string>(string_key)) {
        BOOST_TEST((decode_txn_result<std::string>(encode_txn_result<std::string>(res)) == res));
    }
    for (const auto& res : every_result<std::uint64_t>(numeric_key)) {
        BOOST_TEST(
            (decode_txn_result<std::uint64_t>(encode_txn_result<std::uint64_t>(res)) == res));
    }
}

BOOST_AUTO_TEST_CASE(every_truncation_and_trailing_byte_is_refused) {
    for (const auto& res : every_result<std::string>(string_key)) {
        auto encoded = encode_txn_result<std::string>(res);
        check_every_prefix_refused(encoded,
                                   [](const auto& b) { return decode_txn_result<std::string>(b); });
        encoded.push_back(std::byte{0});
        BOOST_CHECK_THROW(static_cast<void>(decode_txn_result<std::string>(encoded)),
                          serialization_exception);
    }
}

BOOST_AUTO_TEST_CASE(unknown_status_and_record_values_are_refused) {
    auto status = bytes({0x4B, 1, 7});
    BOOST_CHECK_THROW(static_cast<void>(decode_txn_result<std::string>(status)),
                      serialization_exception);
    auto record = bytes({0x4B, 1, 3, 3});
    append_u64(record, 0);
    BOOST_CHECK_THROW(static_cast<void>(decode_txn_result<std::string>(record)),
                      serialization_exception);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(txn_exceptions)

BOOST_AUTO_TEST_CASE(key_locked_names_key_holder_and_primary) {
    const key_locked_exception<std::string> e("acct-7", k_txn, "acct-1");
    BOOST_TEST(e.key() == "acct-7");
    BOOST_TEST((e.holder() == k_txn));
    BOOST_TEST(e.primary() == "acct-1");
    const std::string what = e.what();
    BOOST_TEST(what.find("acct-7") != std::string::npos);
    BOOST_TEST(what.find("acct-1") != std::string::npos);
    BOOST_TEST(what.find("0123456789abcdeffedcba9876543210") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(isolation_unsupported_names_requested_and_offered) {
    const isolation_unsupported_exception e(
        txn_isolation::snapshot,
        {txn_isolation::read_committed, txn_isolation::read_committed_wait});
    BOOST_TEST((e.requested() == txn_isolation::snapshot));
    BOOST_TEST(e.offered().size() == 2u);
    const std::string what = e.what();
    BOOST_TEST(what.find("snapshot") != std::string::npos);
    BOOST_TEST(what.find("read_committed, read_committed_wait") != std::string::npos);
    BOOST_TEST(what.find("mvcc::on") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(txn_id_renders_as_32_hex_digits) {
    BOOST_TEST(txn_id{}.to_string() == std::string(32, '0'));
    BOOST_TEST((txn_id{0, 1} < txn_id{1, 0}));
}

BOOST_AUTO_TEST_SUITE_END()
