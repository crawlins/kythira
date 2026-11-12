// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file
/// @brief The nearest-neighbour structure behind a vector index partition
/// (task 26): `vector_graph`, and `exact_vector_graph`, which scores every
/// entry. `hnsw_graph.hpp` has the approximate one.

#include <kythira/dynamodb/item_codec.hpp>
#include <kythira/dynamodb/vector_index.hpp>

#include <algorithm>
#include <cstddef>
#include <functional>
#include <map>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace kythira::dynamodb {

/// One item in a vector index: its table key, its vector and the values of
/// its inline filter attributes.
struct vector_entry {
    item keys;
    std::vector<float> vector;
    item filters;
};

/// A nearest-neighbour structure over one partition of a vector index,
/// keyed by the item's encoded table key.
class vector_graph {
public:
    vector_graph() = default;
    vector_graph(const vector_graph&) = delete;
    auto operator=(const vector_graph&) -> vector_graph& = delete;
    vector_graph(vector_graph&&) = delete;
    auto operator=(vector_graph&&) -> vector_graph& = delete;
    virtual ~vector_graph() = default;

    virtual void upsert(const std::string& key, vector_entry entry) = 0;
    virtual void remove(const std::string& key) = 0;
    [[nodiscard]] virtual auto size() const -> std::size_t = 0;
    /// Up to `n` entries passing `keep`, best first under `d`, with their
    /// scores against `query`.
    [[nodiscard]] virtual auto nearest(const std::vector<float>& query, vector_distance d,
                                       std::size_t n,
                                       const std::function<bool(const vector_entry&)>& keep) const
        -> std::vector<std::pair<double, const vector_entry*>> = 0;
};

/// Scores every entry: exact, and linear in the partition's size.
class exact_vector_graph final : public vector_graph {
public:
    void upsert(const std::string& key, vector_entry entry) override {
        _entries.insert_or_assign(key, std::move(entry));
    }
    void remove(const std::string& key) override { _entries.erase(key); }
    [[nodiscard]] auto size() const -> std::size_t override { return _entries.size(); }

    [[nodiscard]] auto nearest(const std::vector<float>& query, vector_distance d, std::size_t n,
                               const std::function<bool(const vector_entry&)>& keep) const
        -> std::vector<std::pair<double, const vector_entry*>> override {
        std::vector<std::tuple<double, const std::string*, const vector_entry*>> hits;
        for (const auto& [key, e] : _entries) {
            if (keep(e)) {
                hits.emplace_back(vector_score(d, query, e.vector), &key, &e);
            }
        }
        // Ties rank by key, the same on every node.
        const auto better = [d](const auto& a, const auto& b) {
            if (std::get<0>(a) != std::get<0>(b)) {
                return better_score(d, std::get<0>(a), std::get<0>(b));
            }
            return *std::get<1>(a) < *std::get<1>(b);
        };
        if (hits.size() > n) {
            std::partial_sort(hits.begin(), hits.begin() + static_cast<std::ptrdiff_t>(n),
                              hits.end(), better);
            hits.resize(n);
        } else {
            std::sort(hits.begin(), hits.end(), better);
        }
        std::vector<std::pair<double, const vector_entry*>> out;
        out.reserve(hits.size());
        for (const auto& [score, key, e] : hits) {
            out.emplace_back(score, e);
        }
        return out;
    }

private:
    std::map<std::string, vector_entry> _entries;
};

}  // namespace kythira::dynamodb
