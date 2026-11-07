// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// `dot`, proof folding and the Merkle accumulator
// (`.kiro/specs/verifiable-ledger/` task 2, Requirements 4.1, 4.4, 4.6 and
// 12.2). The fixed vectors come from tools/qldb_dot_vectors.py, which runs
// the AWS QLDB verification tutorial's own Python functions; the rest are
// properties over every tip and leaf of trees up to 70 leaves.

#define BOOST_TEST_MODULE ledger_merkle_test

#include <boost/test/unit_test.hpp>

#include <kythira/ledger/merkle.hpp>

#include <bit>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace ledger = kythira::ledger;
using ledger::hash256;

namespace {

auto from_hex(std::string_view hex) -> hash256 {
    BOOST_REQUIRE_EQUAL(hex.size(), 64U);
    hash256 out{};
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] =
            static_cast<std::uint8_t>(std::stoul(std::string(hex.substr(i * 2, 2)), nullptr, 16));
    }
    return out;
}

auto label(std::string_view s) -> hash256 {
    return ledger::sha256(
        std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(s.data()), s.size()));
}

auto leaves(std::size_t n) -> std::vector<hash256> {
    std::vector<hash256> out;
    for (std::size_t i = 0; i < n; ++i) {
        out.push_back(label(std::to_string(i)));
    }
    return out;
}

/// A store that counts reads, for the O(log n) checks.
class counting_store {
public:
    [[nodiscard]] auto get(std::uint32_t level, std::uint64_t index) const
        -> std::optional<hash256> {
        ++reads;
        return inner.get(level, index);
    }
    auto put(std::uint32_t level, std::uint64_t index, const hash256& h) -> void {
        inner.put(level, index, h);
    }
    ledger::memory_merkle_store inner;
    mutable std::size_t reads = 0;
};

auto ceil_log2(std::uint64_t n) -> std::size_t {
    std::size_t bits = 0;
    while ((std::uint64_t{1} << bits) < n) {
        ++bits;
    }
    return bits;
}

}  // namespace

BOOST_AUTO_TEST_CASE(dot_matches_the_qldb_tutorial_function) {
    struct vec {
        const char* a;
        const char* b;
        const char* expected;
    };
    // Output of tools/qldb_dot_vectors.py (join_hash_pairwise, as published).
    const vec vectors[] = {
        // decided by the last byte
        {"ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff01",
         "0000000000000000000000000000000000000000000000000000000000000002",
         "26cfdd5c55acd6b84e0d9ade40296b87b46376a3a41602f95ca3bbfdcb44553e"},
        // signed: 0x80 sorts before 0x7f
        {"000000000000000000000000000000000000000000000000000000000000007f",
         "0000000000000000000000000000000000000000000000000000000000000080",
         "7035948d9d59da9d6ad20d11b3bc4a1e4c386dbe686fcadb095afb6bb462e6e3"},
        // tie on the last byte, decided earlier
        {"0000000000000000000000000000000000000000000000000000000000000509",
         "0000000000000000000000000000000000000000000000000000000000000409",
         "c5606aacbbace4d5d3c9b33f8cd5d6d3a7cd2b2605a0dec13d369c7e591ea4b3"},
        // equal hashes
        {"1111111111111111111111111111111111111111111111111111111111111111",
         "1111111111111111111111111111111111111111111111111111111111111111",
         "9aed5fce4bb60c40cb8a2983b43540adb4c8ac8aa1ef1f20de57526f9ed86e38"},
        // sha256 labels
        {"ca978112ca1bbdcafac231b39a23dc4da786eff8147c4e72b9807785afee48bb",
         "3e23e8160039594a33894f6564e1b1348bbd7a0088d42c4acb73eeaed59c009d",
         "18d79cb747ea174c59f3a3b41768672526d56fecc58360a99d283d0f9b0a3cc0"},
    };
    for (const auto& v : vectors) {
        const hash256 a = from_hex(v.a);
        const hash256 b = from_hex(v.b);
        BOOST_TEST(ledger::to_hex(ledger::dot(a, b)) == v.expected);
        BOOST_TEST(ledger::to_hex(ledger::dot(b, a)) == v.expected);  // order-free
    }
}

BOOST_AUTO_TEST_CASE(compare_is_signed_and_last_byte_first) {
    hash256 a{};
    hash256 b{};
    a[31] = 0x80;  // -128 as a signed byte
    b[31] = 0x7F;
    BOOST_TEST(ledger::compare_hashes(a, b) < 0);
    BOOST_TEST(ledger::compare_hashes(b, a) > 0);
    a[31] = b[31];
    a[0] = 0x01;  // only the first byte differs
    BOOST_TEST(ledger::compare_hashes(a, b) > 0);
    BOOST_TEST(ledger::compare_hashes(b, b) == 0);
}

BOOST_AUTO_TEST_CASE(fold_matches_the_qldb_tutorial_procedure) {
    const std::vector<hash256> proof = {label("proof-0"), label("proof-1"), label("proof-2"),
                                        label("proof-3")};
    // calculate_root_hash_from_internal_hashes(proof, leaf), as published.
    const hash256 digest =
        from_hex("3557cf1407f582677b57f7dcfdab3a304ac9d4f4ae44795d05c1500e666451c4");
    BOOST_TEST(ledger::verify_proof(label("revision"), proof, digest));
    BOOST_TEST(!ledger::verify_proof(label("revisioN"), proof, digest));
    auto reordered = proof;
    std::swap(reordered[0], reordered[1]);
    BOOST_TEST(!ledger::verify_proof(label("revision"), reordered, digest));
}

BOOST_AUTO_TEST_CASE(tree_shape_is_pinned) {
    // tools/qldb_dot_vectors.py: RFC 6962 roots with join_hash_pairwise.
    const char* roots[] = {
        "5feceb66ffc86f38d952786c6d696c79c2dbc239dd4e91b46729d73a27fb57e9",
        "b9b10a1bc77d2a241d120324db7f3b81b2edb67eb8e9cf02af9c95d30329aef5",
        "c80f77387d860fa469920d7ac2f8a959ef83a651f76dc54923734ed76daaef53",
        "3adc753b7317e280a98c1438447f17cbb0842a9b83724fff70ad4f0b454f4805",
        "a0ee53656a3c8800955b1c7c928e41195ce8d3a2d58c299cd083274ad9ab863b",
        "f9fcd235299fa8c6458dd833313302736c96f86ffea5557cacc350a43038e57e",
        "ccd5ecd1825539b1b4315c41a22b86fcb82c6d85080669a580ebdd96b29eaa56",
        "3c87386727a74507b1930d8df118a173a8ae8bf569c8c1bf466ef7e1cfbce2d5",
        "ae8a264a5061470255f8dc5a129af067117283a2c19790d9bbfd9c09878d8c7e",
    };
    const auto all = leaves(9);
    ledger::memory_merkle_store store;
    ledger::merkle_accumulator acc(store);
    for (std::size_t n = 1; n <= 9; ++n) {
        acc.append(all[n - 1]);
        const std::span<const hash256> prefix(all.data(), n);
        BOOST_TEST(ledger::to_hex(ledger::merkle_root(prefix)) == roots[n - 1]);
        BOOST_TEST(ledger::to_hex(*acc.digest()) == roots[n - 1]);
        BOOST_TEST(ledger::to_hex(acc.digest_at(n)) == roots[n - 1]);
    }
}

BOOST_AUTO_TEST_CASE(every_proof_at_every_tip_verifies) {
    constexpr std::size_t max_leaves = 70;
    const auto all = leaves(max_leaves);
    counting_store store;
    ledger::merkle_accumulator acc(store);
    BOOST_TEST(!acc.digest().has_value());
    for (const auto& h : all) {
        acc.append(h);
    }
    BOOST_TEST(acc.size() == max_leaves);
    BOOST_TEST(*acc.digest() == acc.digest_at(max_leaves));

    for (std::uint64_t tip = 1; tip <= max_leaves; ++tip) {
        store.reads = 0;
        const hash256 digest = acc.digest_at(tip);
        BOOST_TEST(store.reads == static_cast<std::size_t>(std::popcount(tip)));
        BOOST_TEST(digest == ledger::merkle_root(std::span<const hash256>(all.data(), tip)));
        for (std::uint64_t i = 0; i < tip; ++i) {
            store.reads = 0;
            const auto proof = acc.proof(i, tip);
            // Requirement 4.4: O(log n) reads.
            BOOST_TEST(store.reads <= 2 * ceil_log2(tip) + 1);
            BOOST_TEST(proof.size() <= ceil_log2(tip));
            BOOST_TEST(ledger::verify_proof(all[i], proof, digest));
            BOOST_TEST(proof == ledger::merkle_proof(std::span<const hash256>(all.data(), tip), i));
            // The proof binds the leaf and the tip.
            BOOST_TEST(!ledger::verify_proof(all[(i + 1) % max_leaves], proof, digest));
            if (tip < max_leaves) {
                BOOST_TEST(!ledger::verify_proof(all[i], proof, acc.digest_at(tip + 1)));
            }
        }
    }
}

BOOST_AUTO_TEST_CASE(reopening_rebuilds_the_frontier) {
    const auto all = leaves(45);
    ledger::memory_merkle_store reference_store;
    ledger::merkle_accumulator reference(reference_store);
    ledger::memory_merkle_store store;
    for (std::size_t n = 0; n < all.size(); ++n) {
        // A fresh accumulator over the same store at every size, as a
        // restarted replica opens one.
        ledger::merkle_accumulator reopened(store, n);
        BOOST_TEST(reopened.size() == n);
        if (n > 0) {
            BOOST_TEST(*reopened.digest() == *reference.digest());
        }
        reopened.append(all[n]);
        reference.append(all[n]);
        BOOST_TEST(*reopened.digest() == *reference.digest());
    }
    BOOST_TEST(store.node_count() == reference_store.node_count());
}

BOOST_AUTO_TEST_CASE(a_changed_node_is_detected) {
    const auto all = leaves(16);
    ledger::memory_merkle_store store;
    ledger::merkle_accumulator acc(store);
    for (const auto& h : all) {
        acc.append(h);
    }
    const hash256 digest = acc.digest_at(16);
    const hash256 digest6 = acc.digest_at(6);
    const auto proof = acc.proof(7, 16);
    // Level 1, index 2 covers leaves 4-5: a peak of the 6-leaf tree and a
    // node on leaf 7's audit path. (The 16-leaf digest reads only the root
    // node; finding a changed interior node under it is ledger_ctl
    // verify's job, which recomputes the tree.)
    store.corrupt(1, 2, label("tampered"));
    BOOST_TEST(acc.digest_at(6) != digest6);
    BOOST_TEST(!ledger::verify_proof(all[7], acc.proof(7, 16), digest));
    BOOST_TEST(ledger::verify_proof(all[7], proof, digest));  // the saved proof still holds
}

BOOST_AUTO_TEST_CASE(out_of_range_requests_are_rejected) {
    ledger::memory_merkle_store store;
    ledger::merkle_accumulator acc(store);
    BOOST_CHECK_THROW((void)acc.digest_at(1), std::out_of_range);
    acc.append(label("0"));
    acc.append(label("1"));
    BOOST_CHECK_THROW((void)acc.digest_at(0), std::out_of_range);
    BOOST_CHECK_THROW((void)acc.digest_at(3), std::out_of_range);
    BOOST_CHECK_THROW((void)acc.proof(2, 2), std::out_of_range);  // leaf past the tip
    BOOST_CHECK_THROW((void)acc.proof(0, 3), std::out_of_range);  // tip past the tree
    BOOST_CHECK_THROW((void)acc.leaf(2), std::out_of_range);
    BOOST_TEST(acc.proof(0, 1).empty());
    BOOST_CHECK_THROW((void)ledger::merkle_root({}), std::invalid_argument);

    ledger::memory_merkle_store empty;
    BOOST_CHECK_THROW((ledger::merkle_accumulator(empty, 3)), ledger::merkle_store_error);
}
