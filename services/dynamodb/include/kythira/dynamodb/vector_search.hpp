// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file
/// @brief Vector search (`.kiro/specs/dynamodb-compatible-api/` task 26,
/// Requirements 22.3 and 22.4): the graphs each node keeps of a vector
/// index's items, fed by the change log, `SearchVectors`, and the worker
/// that moves a new index to ACTIVE and drops a deleted one.
///
/// A node builds an index's graphs the first time it serves a search on
/// the index: it notes where each of the table's change log streams ends,
/// reads the whole table consistently, then applies every change record
/// from the noted positions on, older streams (split parents) before their
/// children, as the point-in-time archiver does. Each later search first
/// applies the records written since; one whose records were trimmed before
/// they were read builds the graphs again. So index maintenance follows the
/// item writes asynchronously, as a GSI's does, and a table with a vector
/// index always logs its changes (`table_descriptor::logs_changes`).
///
/// The graphs are per `HASH` value (one graph when the index has no
/// `HASH`). A search scores the partition's candidates that pass the
/// inline filters, then reads the best candidates again, consistently,
/// re-scores them from the item as it is, and drops any that are gone or
/// no longer match; it reads further candidates until it has `TopK` or none
/// are left. So a search never returns an item that is not in the table,
/// whatever the graphs lag.
///
/// `vector_graph` (`vector_graph.hpp`) is the nearest-neighbour structure:
/// HNSW through hnswlib where the build has it (`hnsw_graph.hpp`), which
/// scores every candidate while a partition is small, else exact search
/// throughout. `vector_registry` takes another through its graph factory.
///
/// `ConsumedCapacity` reports `VectorSearchRequestBytes`: the bytes of the
/// search vector as 32-bit floats. AWS does not document how it counts
/// them; this is our choice, recorded in `tasks.md`.

#include <kythira/dynamodb/api_common.hpp>
#include <kythira/dynamodb/backend.hpp>
#include <kythira/dynamodb/catalog.hpp>
#include <kythira/dynamodb/change_log.hpp>
#include <kythira/dynamodb/errors.hpp>
#include <kythira/dynamodb/expr_eval.hpp>
#include <kythira/dynamodb/expr_parser.hpp>
#include <kythira/dynamodb/front_end.hpp>
#include <kythira/dynamodb/item_codec.hpp>
#include <kythira/dynamodb/key_layout.hpp>
#include <kythira/dynamodb/streams.hpp>
#include <kythira/dynamodb/table_api.hpp>
#include <kythira/dynamodb/vector_graph.hpp>
#include <kythira/dynamodb/vector_index.hpp>
#if defined(KYTHIRA_DYNAMODB_HAS_HNSWLIB)
#include <kythira/dynamodb/hnsw_graph.hpp>
#endif

#include <raft/aws_json_protocol.hpp>

#include <boost/json.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace kythira::dynamodb {

namespace vector_search_detail {

/// The encoded table key of `it` (partition, then sort).
[[nodiscard]] inline auto key_bytes(const key_schema& schema, const item& it) -> std::string {
    item keys;
    if (const auto* p = find_attribute(it, schema.partition_name)) {
        keys.emplace_back(schema.partition_name, *p);
    }
    if (schema.sort_name) {
        if (const auto* s = find_attribute(it, *schema.sort_name)) {
            keys.emplace_back(*schema.sort_name, *s);
        }
    }
    return encode_item(keys);
}

/// The table key attributes of `it`.
[[nodiscard]] inline auto keys_of(const key_schema& schema, const item& it) -> item {
    item keys;
    for (const auto& [name, value] : it) {
        if (name == schema.partition_name || (schema.sort_name && name == *schema.sort_name)) {
            keys.emplace_back(name, value);
        }
    }
    return keys;
}

/// The partition `it` belongs to in `v` (its encoded `HASH` value), or
/// nothing when it has no `HASH` value.
[[nodiscard]] inline auto partition_of(const vector_index& v, const item& it)
    -> std::optional<std::string> {
    const auto* hash = v.hash_attribute();
    if (hash == nullptr) {
        return std::string{};
    }
    const auto* a = find_attribute(it, *hash);
    if (a == nullptr) {
        return std::nullopt;
    }
    return encode_item(item{{*hash, *a}});
}

/// `it`'s entry in `v`, or nothing when `v` does not hold it.
[[nodiscard]] inline auto entry_of(const key_schema& schema, const vector_index& v, const item& it)
    -> std::optional<vector_entry> {
    auto vec = vector_of(it, v);
    if (!vec) {
        return std::nullopt;
    }
    vector_entry e{keys_of(schema, it), std::move(*vec), {}};
    for (const auto& s : v.schema) {
        if (const auto* a = find_attribute(it, s.name)) {
            e.filters.emplace_back(s.name, *a);
        }
    }
    // Items are sorted by name: `find_attribute` searches them.
    std::ranges::sort(e.filters, {}, &item::value_type::first);
    return e;
}

/// What a result shows of `it` under `v`'s projection: everything for
/// `ALL`; else the table keys, the vector attribute, the search schema
/// attributes and, for `INCLUDE`, the named non-key attributes.
[[nodiscard]] inline auto project(const key_schema& schema, const vector_index& v, const item& it)
    -> item {
    if (v.projection == projection_kind::all) {
        return it;
    }
    item out;
    for (const auto& [name, value] : it) {
        const bool keep =
            name == schema.partition_name || (schema.sort_name && name == *schema.sort_name) ||
            name == v.attribute ||
            std::ranges::any_of(v.schema,
                                [&](const search_schema_element& e) { return e.name == name; }) ||
            (v.projection == projection_kind::include &&
             std::ranges::find(v.non_key, name) != v.non_key.end());
        if (keep) {
            out.emplace_back(name, value);
        }
    }
    return out;
}

}  // namespace vector_search_detail

/// One node's graphs of every vector index it has searched.
template<typename Backend> class vector_registry {
public:
    using graph_factory = std::function<std::unique_ptr<vector_graph>(const vector_index&)>;

    struct options {
        /// Change records read per page while catching up.
        std::size_t page_records = 1000;
    };

    explicit vector_registry(Backend& backend, options opts = {}, graph_factory factory = {})
        : _backend(backend), _options(opts), _factory(std::move(factory)) {
        if (!_factory) {
            _factory = default_factory();
        }
    }

    /// HNSW where the build has hnswlib, else exact search.
    [[nodiscard]] static auto default_factory() -> graph_factory {
#if defined(KYTHIRA_DYNAMODB_HAS_HNSWLIB)
        return [](const vector_index& v) -> std::unique_ptr<vector_graph> {
            return std::make_unique<hnsw_vector_graph>(v);
        };
#else
        return [](const vector_index&) -> std::unique_ptr<vector_graph> {
            return std::make_unique<exact_vector_graph>();
        };
#endif
    }

    /// A ranked candidate: its table key and its score in the graphs.
    struct candidate {
        item keys;
        double score = 0.0;
    };

    /// Brings `index` of `table` up to the change log, then returns up to
    /// `n` of its candidates in `partition` that meet `condition`, best
    /// first.
    auto nearest(const table_descriptor& table, const vector_index& index,
                 const std::string& partition, const std::vector<float>& query, std::size_t n,
                 const search_condition& condition) -> std::vector<candidate> {
        auto state = state_of(table, index);
        const std::lock_guard lock(state->mu);
        catch_up(table, index, *state);
        std::vector<candidate> out;
        const auto it = state->partitions.find(partition);
        if (it == state->partitions.end()) {
            return out;
        }
        const auto found = it->second->nearest(
            query, index.distance, n,
            [&](const vector_entry& e) { return condition.matches(e.filters); });
        out.reserve(found.size());
        for (const auto& [score, e] : found) {
            out.push_back(candidate{e->keys, score});
        }
        return out;
    }

    /// Builds (or catches up) `index`'s graphs now, as the worker does
    /// before it marks a new index ACTIVE.
    void warm(const table_descriptor& table, const vector_index& index) {
        auto state = state_of(table, index);
        const std::lock_guard lock(state->mu);
        catch_up(table, index, *state);
    }

    /// Items in `index`'s graphs on this node (0 before it is built).
    [[nodiscard]] auto item_count(const table_descriptor& table, const vector_index& index) const
        -> std::size_t {
        std::shared_ptr<index_state> state;
        {
            const std::lock_guard lock(_mu);
            const auto it = _states.find(state_key{table.table_id, index.name});
            if (it == _states.end() || it->second->created_ms != index.created_ms) {
                return 0;
            }
            state = it->second;
        }
        const std::lock_guard lock(state->mu);
        return state->where.size();
    }

    /// How many times this node built `index`'s graphs from a table read.
    [[nodiscard]] auto builds(const table_descriptor& table, const vector_index& index) const
        -> std::uint64_t {
        const std::lock_guard lock(_mu);
        const auto it = _states.find(state_key{table.table_id, index.name});
        return it == _states.end() ? 0 : it->second->builds;
    }

    /// Forgets an index's graphs (`index` empty: every index of the table).
    void forget(std::uint64_t table_id, std::string_view index = {}) {
        const std::lock_guard lock(_mu);
        std::erase_if(_states, [&](const auto& kv) {
            return kv.first.first == table_id && (index.empty() || kv.first.second == index);
        });
    }

private:
    using state_key = std::pair<std::uint64_t, std::string>;

    struct index_state {
        std::mutex mu;
        /// The index these graphs are of: one dropped and added again
        /// under its name is new.
        std::uint64_t created_ms = 0;
        bool built = false;
        std::uint64_t builds = 0;
        /// Where each change log stream is read up to (the next record).
        std::map<std::string, std::uint64_t> cursors;
        /// Streams closed and read to their end.
        std::set<std::string> drained;
        std::map<std::string, std::unique_ptr<vector_graph>> partitions;
        /// Each held item's partition, by encoded key.
        std::map<std::string, std::string> where;
    };

    auto state_of(const table_descriptor& table, const vector_index& index)
        -> std::shared_ptr<index_state> {
        const std::lock_guard lock(_mu);
        auto& s = _states[state_key{table.table_id, index.name}];
        if (!s || s->created_ms != index.created_ms) {
            s = std::make_shared<index_state>();
            s->created_ms = index.created_ms;
        }
        return s;
    }

    void apply(const table_descriptor& table, const vector_index& index, index_state& s,
               const item& keys, const std::optional<item>& image) {
        namespace vs = vector_search_detail;
        const auto key = vs::key_bytes(table.schema, keys);
        if (const auto at = s.where.find(key); at != s.where.end()) {
            if (const auto p = s.partitions.find(at->second); p != s.partitions.end()) {
                p->second->remove(key);
                if (p->second->size() == 0) {
                    s.partitions.erase(p);
                }
            }
            s.where.erase(at);
        }
        if (!image) {
            return;
        }
        auto partition = vs::partition_of(index, *image);
        auto entry = vs::entry_of(table.schema, index, *image);
        if (!partition || !entry) {
            return;
        }
        auto& g = s.partitions[*partition];
        if (!g) {
            g = _factory(index);
        }
        g->upsert(key, std::move(*entry));
        s.where.emplace(key, std::move(*partition));
    }

    /// The table's change log streams, parents (older epochs) first.
    auto shards_of(const table_descriptor& table) -> std::vector<change_shard> {
        auto shards = _backend.change_shards(table_range(table.table_id));
        std::ranges::sort(shards, [](const change_shard& a, const change_shard& b) {
            const auto ea = streams_detail::epoch_of(a.stream.key);
            const auto eb = streams_detail::epoch_of(b.stream.key);
            return ea != eb ? ea < eb : a.stream.key < b.stream.key;
        });
        return shards;
    }

    void build(const table_descriptor& table, const vector_index& index, index_state& s) {
        s.partitions.clear();
        s.where.clear();
        s.cursors.clear();
        s.drained.clear();
        // Positions first: a write after them is in the log, and replaying
        // it over the table read below leaves the item as it is now.
        for (const auto& shard : shards_of(table)) {
            const auto page = _backend.read_changes(shard.stream.key, 0, 0);
            s.cursors[shard.stream.key] = page.known ? page.last_seq + 1 : shard.stream.first_seq;
        }
        _backend.read_range(table_range(table.table_id), kythira::scan_direction::forward, true,
                            [&](std::string_view, const item& it) {
                                apply(table, index, s, it, it);
                                return true;
                            });
        s.built = true;
        ++s.builds;
    }

    /// Applies the records since the cursors; false when some were trimmed
    /// before they were read, so the graphs must be built again.
    auto tail(const table_descriptor& table, const vector_index& index, index_state& s) -> bool {
        const auto shards = shards_of(table);
        std::set<std::string> listed;
        for (const auto& shard : shards) {
            listed.insert(shard.stream.key);
        }
        std::set<std::string> undrained;
        for (const auto& shard : shards) {
            const auto& key = shard.stream.key;
            const bool parent_pending = std::ranges::any_of(
                shard.stream.parents,
                [&](const auto& p) { return listed.contains(p) && undrained.contains(p); });
            if (parent_pending) {
                undrained.insert(key);
                continue;
            }
            const bool tracked = s.cursors.contains(key);
            auto at = tracked ? s.cursors[key] : shard.stream.first_seq;
            if (s.drained.contains(key)) {
                continue;
            }
            bool open = true;
            for (;;) {
                const auto page = _backend.read_changes(key, at, _options.page_records);
                if (!page.known) {
                    // Gone before it was read to its end.
                    if (tracked) {
                        return false;
                    }
                    open = false;
                    break;
                }
                open = page.open;
                if (at < page.first_seq && (tracked || page.first_seq > shard.stream.first_seq)) {
                    return false;
                }
                if (page.records.empty()) {
                    break;
                }
                for (const auto& [seq, rec] : page.records) {
                    at = seq + 1;
                    if (rec.table_id == table.table_id) {
                        apply(table, index, s, rec.keys, rec.new_image);
                    }
                }
            }
            s.cursors[key] = at;
            if (open) {
                undrained.insert(key);
            } else {
                s.drained.insert(key);
            }
        }
        // Streams no longer listed were trimmed after being read.
        std::erase_if(s.cursors, [&](const auto& kv) { return !listed.contains(kv.first); });
        std::erase_if(s.drained, [&](const std::string& k) { return !listed.contains(k); });
        return true;
    }

    void catch_up(const table_descriptor& table, const vector_index& index, index_state& s) {
        if (!s.built) {
            build(table, index, s);
        }
        // A build reads the table again from fresh positions, so a second
        // failure means records are being trimmed faster than they are
        // read; the next search tries again.
        for (int attempt = 0; attempt < 2; ++attempt) {
            if (tail(table, index, s)) {
                return;
            }
            build(table, index, s);
        }
    }

    Backend& _backend;
    options _options;
    graph_factory _factory;
    mutable std::mutex _mu;
    std::map<state_key, std::shared_ptr<index_state>> _states;
};

/// `SearchVectors`.
template<dynamo_backend Backend>
requires change_feed_backend<Backend>
class vector_search_api {
public:
    using result = aws_json::result<boost::json::object>;

    explicit vector_search_api(Backend& backend) : _backend(backend), _registry(backend) {}

    template<typename FrontEnd> void register_with(FrontEnd& fe) {
        fe.on("SearchVectors",
              [this](call_context& c, boost::json::object& in) { return search(c, in); });
    }

    [[nodiscard]] auto registry() -> vector_registry<Backend>& { return _registry; }

    auto search(call_context& ctx, boost::json::object& in) -> result {
        namespace vs = vector_search_detail;
        auto table = api_detail::usable_table(_backend, ctx, in.at("TableName").as_string());
        if (!table) {
            return table.error();
        }
        const std::string index_name(in.at("IndexName").as_string());
        const auto* index = table->vector_named(index_name);
        if (index == nullptr) {
            return table_api_detail::modeled(
                "ResourceNotFoundException",
                "Requested resource not found: Vector index " + index_name + " does not exist");
        }
        if (index->status != index_status::active) {
            return validation_error("Vector index " + index_name + " is " +
                                    std::string(index_status_name(index->status)) +
                                    " and cannot be searched yet");
        }
        auto query = search_vector_from(in.at("SearchVector").as_array(), *index);
        if (!query) {
            return query.error();
        }
        const auto top_k = in.at("TopK").to_number<std::int64_t>();
        if (top_k < 1) {
            return validation_error("TopK must be at least 1");
        }
        const auto k = static_cast<std::size_t>(top_k);
        const auto* names = in.if_contains("ExpressionAttributeNames");
        const auto* values = in.if_contains("ExpressionAttributeValues");
        const auto* names_obj =
            names != nullptr && names->is_object() ? &names->as_object() : nullptr;
        const auto* values_obj =
            values != nullptr && values->is_object() ? &values->as_object() : nullptr;
        auto condition = parse_search_condition(api_detail::text(in, "SearchConditionExpression"),
                                                *index, names_obj, values_obj);
        if (!condition) {
            return condition.error();
        }
        std::optional<projection_expr> projection;
        if (const auto text = api_detail::text(in, "ProjectionExpression"); !text.empty()) {
            auto compiler = expression_compiler::make(names_obj, nullptr);
            if (!compiler) {
                return compiler.error();
            }
            auto p = compiler->projection(text);
            if (!p) {
                return p.error();
            }
            projection = std::move(*p);
        }
        std::string partition;
        if (const auto* hash = index->hash_attribute()) {
            partition = encode_item(item{{*hash, *condition->value_of(*hash)}});
        }

        // Re-read the best candidates; fetch more while some drop out.
        std::vector<std::pair<double, item>> found;
        std::size_t asked = k;
        std::size_t seen = 0;
        for (;;) {
            const auto candidates =
                _registry.nearest(*table, *index, partition, *query, asked, *condition);
            for (std::size_t i = seen; i < candidates.size() && found.size() < k; ++i) {
                auto now = _backend.read(table->table_id, table->schema, candidates[i].keys, true);
                if (!now) {
                    continue;
                }
                const auto p = vs::partition_of(*index, *now);
                auto entry = vs::entry_of(table->schema, *index, *now);
                if (!p || *p != partition || !entry || !condition->matches(entry->filters)) {
                    continue;
                }
                found.emplace_back(vector_score(index->distance, *query, entry->vector),
                                   std::move(*now));
            }
            seen = candidates.size();
            if (found.size() >= k || candidates.size() < asked) {
                break;
            }
            asked *= 2;
        }
        // Re-scored from the items as they are now.
        std::stable_sort(found.begin(), found.end(), [&](const auto& a, const auto& b) {
            return better_score(index->distance, a.first, b.first);
        });
        boost::json::array results;
        for (const auto& [score, it] : found) {
            auto shown = vs::project(table->schema, *index, it);
            if (projection) {
                shown = project_item(*projection, shown);
            }
            results.push_back(boost::json::object{{"Item", item_to_json(shown)}, {"Score", score}});
        }
        boost::json::object out{{"SearchResults", std::move(results)}};
        if (api_detail::wants_capacity(in)) {
            out["ConsumedCapacity"] = boost::json::object{
                {"VectorSearchRequestBytes", static_cast<double>(query->size() * sizeof(float))}};
        }
        return out;
    }

private:
    Backend& _backend;
    vector_registry<Backend> _registry;
};

/// Moves each CREATING vector index to ACTIVE, once `backfill_delay_ms`
/// has passed since it was added (warming this node's graphs first), and
/// drops each DELETING one; run on the node that leads the catalog.
template<dynamo_backend Backend>
requires change_feed_backend<Backend>
class vector_index_worker {
public:
    struct options {
        std::uint64_t backfill_delay_ms = 0;
    };

    vector_index_worker(Backend& backend, vector_registry<Backend>* registry, options opts = {})
        : _backend(backend), _registry(registry), _options(opts) {}

    /// One pass over every table; returns the indexes it changed.
    auto pass() -> std::size_t {
        std::size_t changed = 0;
        for (const auto& name : _backend.list_tables("", SIZE_MAX)) {
            const auto d = _backend.describe(name);
            if (!d || d->status != table_status::active || d->vectors.empty()) {
                continue;
            }
            auto list = d->vectors;
            bool change = false;
            const auto now = _backend.now_ms();
            for (auto& v : list) {
                if (v.status == index_status::creating &&
                    now >= v.created_ms + _options.backfill_delay_ms) {
                    if (_registry != nullptr) {
                        _registry->warm(*d, v);
                    }
                    v.status = index_status::active;
                    change = true;
                }
            }
            const auto dropped = std::erase_if(
                list, [](const vector_index& v) { return v.status == index_status::deleting; });
            if (dropped > 0) {
                change = true;
            }
            if (!change) {
                continue;
            }
            catalog_command c;
            c.op = catalog_command::kind::update_table;
            c.table = *d;
            c.table.vectors = std::move(list);
            c.update_fields = catalog_command::update_vectors;
            c.now_ms = now;
            const auto r = _backend.catalog(c);
            if (!r.ok()) {
                continue;
            }
            ++changed;
            if (_registry != nullptr) {
                for (const auto& v : d->vectors) {
                    if (v.status == index_status::deleting) {
                        _registry->forget(d->table_id, v.name);
                    }
                }
            }
        }
        return changed;
    }

private:
    Backend& _backend;
    vector_registry<Backend>* _registry;
    options _options;
};

}  // namespace kythira::dynamodb
