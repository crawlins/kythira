// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

// QLDB's `dot` combiner, proof folding, and the ledger's Merkle accumulator
// (.kiro/specs/verifiable-ledger/ task 2, Requirements 4.1, 4.4, 4.6, 12.2).
//
// `dot(a, b)` is SHA-256 over the two hashes in a fixed order: compare them
// from the last byte to the first as signed bytes and put the smaller first.
// That is the function in the AWS QLDB verification tutorial, so a proof
// carries no left/right flags and folds the same way in any order of
// siblings, and Kythira's proofs verify with AWS's published procedure:
// fold the proof hashes onto the leaf with `dot`, compare with the digest.
//
// The tree is the left-filled binary tree of RFC 6962 with `dot` as the
// node function: a node at level k covers 2^k consecutive leaves, and the
// root of n leaves is dot(root of the first 2^m, root of the rest) for the
// largest 2^m < n. Its complete nodes never change once written, so the
// accumulator stores each one once (level, index) and keeps only the
// frontier, at most one complete subtree per level, in memory. Appending is
// O(log n) writes amortised O(1); a digest at any earlier size reads one
// stored node per set bit of that size; a proof reads O(log n) nodes.
//
// QLDB's own tree shape is undocumented; nothing here depends on it, since
// verification only folds the proof (see the scoping doc).

#include <kythira/ledger/ledger_hash.hpp>

#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace kythira::ledger {

/// QLDB's hash order: compare from the last byte to the first, each as a
/// signed byte. Negative when `a` sorts first.
inline auto compare_hashes(const hash256& a, const hash256& b) -> int {
    for (std::size_t i = hash_size; i-- > 0;) {
        const int diff = static_cast<int>(static_cast<std::int8_t>(a[i])) -
                         static_cast<int>(static_cast<std::int8_t>(b[i]));
        if (diff != 0) {
            return diff;
        }
    }
    return 0;
}

/// QLDB's order-free combiner: SHA-256 of the smaller hash then the larger.
inline auto dot(const hash256& a, const hash256& b) -> hash256 {
    return compare_hashes(a, b) < 0 ? sha256({a, b}) : sha256({b, a});
}

/// Fold `proof` onto `leaf` with `dot`, first hash first.
inline auto fold_proof(const hash256& leaf, std::span<const hash256> proof) -> hash256 {
    hash256 acc = leaf;
    for (const auto& h : proof) {
        acc = dot(acc, h);
    }
    return acc;
}

/// AWS's verification procedure: the proof folded onto the leaf equals the
/// digest.
inline auto verify_proof(const hash256& leaf, std::span<const hash256> proof, const hash256& digest)
    -> bool {
    return fold_proof(leaf, proof) == digest;
}

namespace merkle_detail {

/// The largest power of two strictly below `n` (n >= 2).
inline auto split_point(std::uint64_t n) -> std::uint64_t {
    return std::bit_floor(n - 1);
}

/// The root of leaves [start, start + count) given `node(level, index)`
/// for complete subtrees. Count >= 1.
template<typename NodeFn>
auto subtree_root(NodeFn&& node, std::uint64_t start, std::uint64_t count) -> hash256 {
    if (std::has_single_bit(count) && start % count == 0) {
        const auto level = static_cast<std::uint32_t>(std::countr_zero(count));
        return node(level, start >> level);
    }
    const std::uint64_t k = split_point(count);
    return dot(subtree_root(node, start, k), subtree_root(node, start + k, count - k));
}

/// The audit path (RFC 6962 PATH) of leaf `m` of [start, start + count),
/// bottom-up.
template<typename NodeFn>
auto audit_path(NodeFn&& node, std::uint64_t m, std::uint64_t start, std::uint64_t count,
                std::vector<hash256>& out) -> void {
    if (count <= 1) {
        return;
    }
    const std::uint64_t k = split_point(count);
    if (m < k) {
        audit_path(node, m, start, k, out);
        out.push_back(subtree_root(node, start + k, count - k));
    } else {
        audit_path(node, m - k, start + k, count - k, out);
        out.push_back(subtree_root(node, start, k));
    }
}

}  // namespace merkle_detail

/// The Merkle root of `leaves` (same tree shape as the accumulator), for
/// small in-memory trees such as a block's entries. Throws on an empty
/// list.
inline auto merkle_root(std::span<const hash256> leaves) -> hash256 {
    if (leaves.empty()) {
        throw std::invalid_argument("merkle_root: no leaves");
    }
    if (leaves.size() == 1) {
        return leaves.front();
    }
    const std::uint64_t k = merkle_detail::split_point(leaves.size());
    return dot(merkle_root(leaves.first(k)), merkle_root(leaves.subspan(k)));
}

/// The proof that `leaves[index]` is under `merkle_root(leaves)`.
inline auto merkle_proof(std::span<const hash256> leaves, std::uint64_t index)
    -> std::vector<hash256> {
    if (index >= leaves.size()) {
        throw std::out_of_range("merkle_proof: index past the last leaf");
    }
    auto node = [&](std::uint32_t level, std::uint64_t i) {
        const std::uint64_t width = std::uint64_t{1} << level;
        return merkle_root(leaves.subspan(i * width, width));
    };
    std::vector<hash256> out;
    merkle_detail::audit_path(node, index, 0, leaves.size(), out);
    return out;
}

/// Where the accumulator keeps complete nodes. Level 0 holds the leaves
/// (block hashes); level k index i covers leaves [i * 2^k, (i + 1) * 2^k).
/// `put` is only ever called once per node, with its final value, so an
/// engine-backed store can add it to the same write batch as the block
/// (task 4).
template<typename S>
concept merkle_node_store = requires(S& store, const S& cstore, std::uint32_t level,
                                     std::uint64_t index, const hash256& h) {
    { cstore.get(level, index) } -> std::same_as<std::optional<hash256>>;
    store.put(level, index, h);
};

/// A merkle_node_store in memory, for tests and tools.
class memory_merkle_store {
public:
    [[nodiscard]] auto get(std::uint32_t level, std::uint64_t index) const
        -> std::optional<hash256> {
        const auto it = nodes_.find({level, index});
        if (it == nodes_.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    auto put(std::uint32_t level, std::uint64_t index, const hash256& h) -> void {
        nodes_[{level, index}] = h;
    }

    [[nodiscard]] auto node_count() const -> std::size_t { return nodes_.size(); }

    /// Overwrite a node, for tamper tests.
    auto corrupt(std::uint32_t level, std::uint64_t index, const hash256& h) -> void {
        nodes_[{level, index}] = h;
    }

private:
    std::map<std::pair<std::uint32_t, std::uint64_t>, hash256> nodes_;
};

static_assert(merkle_node_store<memory_merkle_store>);

/// Thrown when the store lacks a node the tree's size says it must hold.
class merkle_store_error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// The append-only Merkle accumulator over block hashes.
template<merkle_node_store Store> class merkle_accumulator {
public:
    /// Open over `store`, which holds the complete nodes of a tree of `size`
    /// leaves; the frontier is rebuilt from it (one read per set bit).
    explicit merkle_accumulator(Store& store, std::uint64_t size = 0) : store_(store), size_(size) {
        for (std::uint32_t level = 0; level < 64; ++level) {
            if (((size_ >> level) & 1U) != 0) {
                frontier_.resize(level + 1);
                frontier_[level] = read(level, (size_ >> level) - 1);
            }
        }
    }

    [[nodiscard]] auto size() const -> std::uint64_t { return size_; }

    /// Append a leaf; returns its index.
    auto append(const hash256& leaf) -> std::uint64_t {
        const std::uint64_t index = size_;
        store_.put(0, index, leaf);
        hash256 node = leaf;
        std::uint32_t level = 0;
        std::uint64_t i = index;
        while ((i & 1U) != 0) {
            node = dot(*frontier_[level], node);
            frontier_[level].reset();
            ++level;
            i >>= 1U;
            store_.put(level, i, node);
        }
        if (frontier_.size() <= level) {
            frontier_.resize(level + 1);
        }
        frontier_[level] = node;
        ++size_;
        return index;
    }

    /// The digest of the whole tree, from the in-memory frontier; nullopt
    /// while empty.
    [[nodiscard]] auto digest() const -> std::optional<hash256> {
        std::optional<hash256> acc;
        for (const auto& peak : frontier_) {
            if (peak) {
                acc = acc ? dot(*peak, *acc) : *peak;
            }
        }
        return acc;
    }

    /// The digest of the first `tree_size` leaves (1 <= tree_size <= size()).
    [[nodiscard]] auto digest_at(std::uint64_t tree_size) const -> hash256 {
        if (tree_size == 0 || tree_size > size_) {
            throw std::out_of_range("merkle_accumulator: digest size out of range");
        }
        std::optional<hash256> acc;
        for (std::uint32_t level = 0; level < 64; ++level) {
            if (((tree_size >> level) & 1U) != 0) {
                const hash256 peak = read(level, (tree_size >> level) - 1);
                acc = acc ? dot(peak, *acc) : peak;
            }
        }
        return *acc;
    }

    /// The proof that leaf `index` is under `digest_at(tree_size)`.
    [[nodiscard]] auto proof(std::uint64_t index, std::uint64_t tree_size) const
        -> std::vector<hash256> {
        if (tree_size > size_ || index >= tree_size) {
            throw std::out_of_range("merkle_accumulator: proof index or size out of range");
        }
        std::vector<hash256> out;
        merkle_detail::audit_path(
            [this](std::uint32_t level, std::uint64_t i) { return read(level, i); }, index, 0,
            tree_size, out);
        return out;
    }

    /// The stored leaf at `index`.
    [[nodiscard]] auto leaf(std::uint64_t index) const -> hash256 {
        if (index >= size_) {
            throw std::out_of_range("merkle_accumulator: leaf index out of range");
        }
        return read(0, index);
    }

private:
    [[nodiscard]] auto read(std::uint32_t level, std::uint64_t index) const -> hash256 {
        auto h = store_.get(level, index);
        if (!h) {
            throw merkle_store_error("merkle_accumulator: missing node " + std::to_string(level) +
                                     "/" + std::to_string(index));
        }
        return *h;
    }

    Store& store_;
    std::uint64_t size_;
    std::vector<std::optional<hash256>> frontier_;
};

}  // namespace kythira::ledger
