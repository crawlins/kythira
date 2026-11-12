// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file
/// @brief An approximate nearest-neighbour `vector_graph` (task 26) over
/// hnswlib's HNSW index, for builds that have hnswlib
/// (`KYTHIRA_DYNAMODB_HAS_HNSWLIB`; Clark chose hnswlib through vcpkg on
/// 2026-11-12).
///
/// A partition holds its entries in a map, as the exact graph does, and
/// scores them all while it is small (`exact_below` entries): there exact
/// search is cheap and ranks exactly. From that size on it searches an HNSW
/// index of the same entries instead, asking for more neighbours than it
/// needs (`ef` at least `min_ef`), and re-scores what it finds from the
/// stored vectors so scores are exact even when the ranking is
/// approximate.
///
/// HNSW's spaces map onto the distance functions: `EUCLIDEAN` on the
/// squared L2 space; `DOT_PRODUCT` on the inner product space (1 - dot);
/// `COSINE` on the inner product space over unit vectors (1 - cosine). A
/// zero vector stays zero, so under `COSINE` it is at 1 from everything, as
/// `vector_score` says. Deleted entries are marked deleted and their slots
/// reused; the index doubles when full.

#include <kythira/dynamodb/vector_graph.hpp>
#include <kythira/dynamodb/vector_index.hpp>

#include <hnswlib/hnswlib.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace kythira::dynamodb {

class hnsw_vector_graph final : public vector_graph {
public:
    struct options {
        /// Below this many entries a search scores every one.
        std::size_t exact_below = 1000;
        /// HNSW's graph degree and construction breadth.
        std::size_t m = 16;
        std::size_t ef_construction = 100;
        /// The least search breadth; a search asks for at least twice the
        /// neighbours it returns.
        std::size_t min_ef = 64;
        std::size_t initial_capacity = 1024;
    };

    hnsw_vector_graph(const vector_index& index, options opts)
        : _distance(index.distance), _dimensions(index.dimensions), _options(opts) {}
    explicit hnsw_vector_graph(const vector_index& index) : hnsw_vector_graph(index, options{}) {}

    void upsert(const std::string& key, vector_entry entry) override {
        remove(key);
        const auto label = _next_label++;
        if (_index) {
            add_point(label, entry.vector);
        }
        _labels.emplace(key, label);
        _entries.emplace(label, std::make_pair(key, std::move(entry)));
        if (!_index && _entries.size() >= _options.exact_below) {
            build();
        }
    }

    void remove(const std::string& key) override {
        const auto it = _labels.find(key);
        if (it == _labels.end()) {
            return;
        }
        if (_index) {
            _index->markDelete(it->second);
        }
        _entries.erase(it->second);
        _labels.erase(it);
    }

    [[nodiscard]] auto size() const -> std::size_t override { return _entries.size(); }

    /// Whether searches go through the HNSW index (else every entry is
    /// scored).
    [[nodiscard]] auto approximate() const -> bool { return _index != nullptr; }

    [[nodiscard]] auto nearest(const std::vector<float>& query, vector_distance d, std::size_t n,
                               const std::function<bool(const vector_entry&)>& keep) const
        -> std::vector<std::pair<double, const vector_entry*>> override {
        struct hit {
            double score;
            const vector_entry* entry;
            const std::string* key;
        };
        std::vector<hit> hits;
        const auto add = [&](const std::pair<std::string, vector_entry>& kv) {
            hits.push_back(hit{vector_score(d, query, kv.second.vector), &kv.second, &kv.first});
        };
        if (!_index) {
            for (const auto& [label, kv] : _entries) {
                if (keep(kv.second)) {
                    add(kv);
                }
            }
        } else if (n > 0) {
            struct filter final : hnswlib::BaseFilterFunctor {
                const hnsw_vector_graph* self;
                const std::function<bool(const vector_entry&)>* keep;
                filter(const hnsw_vector_graph* s,
                       const std::function<bool(const vector_entry&)>* k)
                    : self(s), keep(k) {}
                auto operator()(hnswlib::labeltype id) -> bool override {
                    const auto it = self->_entries.find(id);
                    return it != self->_entries.end() && (*keep)(it->second.second);
                }
            };
            filter allowed(this, &keep);
            _index->setEf(std::max(_options.min_ef, 2 * n));
            const auto q = prepared(query);
            auto found = _index->searchKnn(q.data(), n, &allowed);
            while (!found.empty()) {
                const auto it = _entries.find(found.top().second);
                found.pop();
                if (it != _entries.end()) {
                    add(it->second);
                }
            }
        }
        // Ties rank by key, the same on every node.
        const auto better = [&](const hit& a, const hit& b) {
            if (a.score != b.score) {
                return better_score(d, a.score, b.score);
            }
            return *a.key < *b.key;
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
        for (const auto& h : hits) {
            out.emplace_back(h.score, h.entry);
        }
        return out;
    }

private:
    using space_ptr = std::unique_ptr<hnswlib::SpaceInterface<float>>;

    /// `v` as the index stores it: unit length under `COSINE`.
    [[nodiscard]] auto prepared(const std::vector<float>& v) const -> std::vector<float> {
        std::vector<float> out(v.begin(), v.end());
        out.resize(_dimensions, 0.0F);
        if (_distance == vector_distance::cosine) {
            double norm = 0.0;
            for (const float x : out) {
                norm += static_cast<double>(x) * x;
            }
            if (norm > 0.0) {
                const auto scale = static_cast<float>(1.0 / std::sqrt(norm));
                for (auto& x : out) {
                    x *= scale;
                }
            }
        }
        return out;
    }

    void add_point(hnswlib::labeltype label, const std::vector<float>& v) {
        if (_index->getDeletedCount() == 0 &&
            _index->getCurrentElementCount() >= _index->getMaxElements()) {
            _index->resizeIndex(_index->getMaxElements() * 2);
        }
        const auto p = prepared(v);
        _index->addPoint(p.data(), label, /*replace_deleted=*/true);
    }

    void build() {
        if (_distance == vector_distance::euclidean) {
            _space = std::make_unique<hnswlib::L2Space>(_dimensions);
        } else {
            _space = std::make_unique<hnswlib::InnerProductSpace>(_dimensions);
        }
        const auto capacity = std::max(_options.initial_capacity, 2 * _entries.size());
        _index = std::make_unique<hnswlib::HierarchicalNSW<float>>(
            _space.get(), capacity, _options.m, _options.ef_construction, /*random_seed=*/100,
            /*allow_replace_deleted=*/true);
        for (const auto& [label, kv] : _entries) {
            add_point(label, kv.second.vector);
        }
    }

    vector_distance _distance;
    std::size_t _dimensions;
    options _options;
    space_ptr _space;
    std::unique_ptr<hnswlib::HierarchicalNSW<float>> _index;
    hnswlib::labeltype _next_label = 0;
    std::unordered_map<std::string, hnswlib::labeltype> _labels;
    std::map<hnswlib::labeltype, std::pair<std::string, vector_entry>> _entries;
};

}  // namespace kythira::dynamodb
