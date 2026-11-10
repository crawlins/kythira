// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file batch_api.hpp
/// @brief `BatchGetItem` and `BatchWriteItem`
///        (`.kiro/specs/dynamodb-compatible-api/` task 8, Requirements
///        10.1, 10.2).
///
/// A batch is checked whole before anything runs: every table exists, the
/// request counts are within AWS's limits (100 keys, 25 writes), every key
/// and item is valid, and no table names the same key twice. Then each
/// key is read, or each write is sent through the backend as its own
/// unconditional `item_command`, routed by key like a single-item call.
/// There is no atomicity across items, as on AWS.
///
/// `UnprocessedKeys` holds the keys left over when the response would pass
/// AWS's 16 MB limit, with each table's read settings, so the caller can
/// retry them as AWS documents. `UnprocessedItems` is empty: a write either
/// commits or fails the call. Throttling (task 12) will fill both.

#include <kythira/dynamodb/api_common.hpp>
#include <kythira/dynamodb/backend.hpp>
#include <kythira/dynamodb/disk_guard.hpp>
#include <kythira/dynamodb/errors.hpp>
#include <kythira/dynamodb/expr_eval.hpp>
#include <kythira/dynamodb/expr_request.hpp>
#include <kythira/dynamodb/front_end.hpp>
#include <kythira/dynamodb/item_codec.hpp>
#include <kythira/dynamodb/item_state_machine.hpp>
#include <kythira/dynamodb/key_layout.hpp>

#include <raft/aws_json_protocol.hpp>

#include <boost/json.hpp>

#include <cstddef>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kythira::dynamodb {

namespace batch_detail {

inline constexpr std::size_t k_max_get_keys = 100;
inline constexpr std::size_t k_max_writes = 25;
/// AWS's limit on a `BatchGetItem` response.
inline constexpr std::size_t k_max_response_bytes = std::size_t{16} * 1024 * 1024;

[[nodiscard]] inline auto duplicates() -> aws_json::error {
    return validation_error("Provided list of item keys contains duplicates");
}

}  // namespace batch_detail

/// The batch handlers over a backend.
template<dynamo_backend Backend> class batch_api {
public:
    using result = aws_json::result<boost::json::object>;

    /// `guard` may be null (no disk floor).
    explicit batch_api(Backend& backend, const disk_guard* guard = nullptr)
        : _backend(backend), _guard(guard) {}

    template<typename FrontEnd> void register_with(FrontEnd& fe) {
        fe.on("BatchGetItem",
              [this](call_context& c, boost::json::object& in) { return batch_get(c, in); });
        fe.on("BatchWriteItem",
              [this](call_context& c, boost::json::object& in) { return batch_write(c, in); });
    }

    auto batch_get(call_context& /*ctx*/, boost::json::object& in) -> result {
        using namespace batch_detail;
        // The codec has checked the model's limits: at least one table, and
        // 1 to 100 keys per table. The total is the handler's to check.
        const auto& requests = in.at("RequestItems").as_object();
        std::size_t total = 0;
        for (const auto& [name, req] : requests) {
            total += req.as_object().at("Keys").as_array().size();
        }
        if (total > k_max_get_keys) {
            return validation_error("Too many items requested for the BatchGetItem call");
        }

        struct table_get {
            std::string name;
            table_descriptor table;
            expression_set compiled;
            bool consistent = false;
            std::vector<item> keys;
            const boost::json::object* request = nullptr;
        };
        std::vector<table_get> gets;
        for (const auto& [name, req] : requests) {
            const auto& r = req.as_object();
            const auto& keys = r.at("Keys").as_array();
            auto table = api_detail::usable_table(_backend, name);
            if (!table) {
                return table.error();
            }
            request_keys rk;
            rk.table = table->schema;
            auto compiled = compile_request("GetItem", r, rk);
            if (!compiled) {
                return compiled.error();
            }
            table_get g{std::string(name),
                        std::move(*table),
                        std::move(*compiled),
                        api_detail::flag(r, "ConsistentRead"),
                        {},
                        &r};
            std::set<std::string> seen;
            for (const auto& k : keys) {
                auto key = item_from_json(k.as_object());
                if (!key) {
                    return key.error();
                }
                if (auto e = validate_key(*key, g.table.schema)) {
                    return *e;
                }
                if (!seen.insert(*base_item_key(g.table.table_id, g.table.schema, *key)).second) {
                    return duplicates();
                }
                g.keys.push_back(std::move(*key));
            }
            gets.push_back(std::move(g));
        }

        boost::json::object responses;
        boost::json::object unprocessed;
        boost::json::array consumed;
        std::size_t response_bytes = 0;
        for (auto& g : gets) {
            boost::json::array found;
            boost::json::array left;
            double units = 0;
            for (const auto& key : g.keys) {
                if (response_bytes >= k_max_response_bytes) {
                    left.push_back(item_to_json(key));
                    continue;
                }
                const auto it = _backend.read(g.table.table_id, g.table.schema, key, g.consistent);
                units += api_detail::read_units(it ? item_size(*it) : 0, g.consistent);
                if (it) {
                    response_bytes += item_size(*it);
                    found.push_back(item_to_json(
                        g.compiled.projection ? project_item(*g.compiled.projection, *it) : *it));
                }
            }
            responses[g.name] = std::move(found);
            if (!left.empty()) {
                // The table's read settings come back with its keys, so the
                // entry can be sent again as it is.
                boost::json::object again = *g.request;
                again["Keys"] = std::move(left);
                unprocessed[g.name] = std::move(again);
            }
            if (api_detail::wants_capacity(in)) {
                consumed.push_back(api_detail::capacity(in, g.table.name, units));
            }
        }
        boost::json::object out{{"Responses", std::move(responses)},
                                {"UnprocessedKeys", std::move(unprocessed)}};
        if (api_detail::wants_capacity(in)) {
            out["ConsumedCapacity"] = std::move(consumed);
        }
        return out;
    }

    auto batch_write(call_context& /*ctx*/, boost::json::object& in) -> result {
        using namespace batch_detail;
        // As for BatchGetItem, the codec has checked 1 to 25 writes per
        // table; the total across tables is checked here.
        const auto& requests = in.at("RequestItems").as_object();
        std::size_t total = 0;
        for (const auto& [name, list] : requests) {
            total += list.as_array().size();
        }
        if (total > k_max_writes) {
            return validation_error("Too many items requested for the BatchWriteItem call");
        }

        struct table_writes {
            table_descriptor table;
            std::vector<item_command> commands;
            double units = 0;
        };
        std::vector<table_writes> writes;
        for (const auto& [name, list] : requests) {
            auto table = api_detail::usable_table(_backend, name);
            if (!table) {
                return table.error();
            }
            table_writes w{std::move(*table), {}, 0};
            std::set<std::string> seen;
            for (const auto& entry : list.as_array()) {
                const auto& e = entry.as_object();
                const auto* put = e.if_contains("PutRequest");
                const auto* del = e.if_contains("DeleteRequest");
                if ((put == nullptr) == (del == nullptr)) {
                    return validation_error(
                        "Supplied AttributeValue has more than one datatypes set, must contain "
                        "exactly one of the supported datatypes");
                }
                item_command c;
                c.table_id = w.table.table_id;
                c.schema = w.table.schema;
                if (put != nullptr) {
                    auto it = item_from_json(put->as_object().at("Item").as_object());
                    if (!it) {
                        return it.error();
                    }
                    if (auto err = validate_key_attributes(*it, w.table.schema)) {
                        return *err;
                    }
                    if (auto err = check_item_size(*it)) {
                        return *err;
                    }
                    c.op = item_command::kind::put;
                    w.units += api_detail::write_units(item_size(*it));
                    c.payload = std::move(*it);
                } else {
                    auto key = item_from_json(del->as_object().at("Key").as_object());
                    if (!key) {
                        return key.error();
                    }
                    if (auto err = validate_key(*key, w.table.schema)) {
                        return *err;
                    }
                    c.op = item_command::kind::delete_;
                    w.units += 1.0;
                    c.payload = std::move(*key);
                }
                if (!seen.insert(*base_item_key(c.table_id, c.schema, c.payload)).second) {
                    return duplicates();
                }
                w.commands.push_back(std::move(c));
            }
            writes.push_back(std::move(w));
        }

        if (_guard != nullptr) {
            if (auto e = _guard->check_write()) {
                return *e;
            }
        }
        boost::json::array consumed;
        for (const auto& w : writes) {
            for (const auto& c : w.commands) {
                const auto r = _backend.write(c);
                if (r.outcome == item_result::status::error) {
                    aws_json::error e;
                    e.type = r.error_type;
                    e.message = r.error_message;
                    return e;
                }
            }
            if (api_detail::wants_capacity(in)) {
                consumed.push_back(api_detail::capacity(in, w.table.name, w.units));
            }
        }
        boost::json::object out{{"UnprocessedItems", boost::json::object{}}};
        if (api_detail::wants_capacity(in)) {
            out["ConsumedCapacity"] = std::move(consumed);
        }
        return out;
    }

private:
    Backend& _backend;
    const disk_guard* _guard;
};

}  // namespace kythira::dynamodb
