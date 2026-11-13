// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file txn_api.hpp
/// @brief `TransactWriteItems` and `TransactGetItems`
///        (`.kiro/specs/dynamodb-compatible-api/` task 10, Requirement 12).
///
/// A transaction is checked whole before anything runs, as a batch is:
/// every element names exactly one action, every table can serve data
/// calls, every expression compiles, every key and item fits the table, no
/// two actions touch one item, and the caller may perform each action (the
/// element's own `dynamodb:` action on its table, through
/// `call_context::authorize`). Then the backend runs it (`transact`): every
/// item is locked, each action's condition is judged against the committed
/// item under its lock (Requirement 12.1; a `ConditionCheck` locks and
/// judges without writing, 12.2), and all apply or none do.
///
/// A refused transaction answers `TransactionCanceledException` with one
/// `CancellationReasons` entry per action, `None` for those that passed
/// (12.3). The codes are `ConditionalCheckFailed` (with the stored item
/// when the action asked for `ReturnValuesOnConditionCheckFailure=ALL_OLD`),
/// `TransactionConflict` (another transaction held the item) and
/// `ItemCollectionSizeLimitExceeded`. An update that cannot apply to the
/// stored item is answered as a plain `ValidationException`, as DynamoDB
/// Local does. The messages are DynamoDB Local's, down to the full stop a
/// `ConditionCheck`'s failure ends with and a `Put`'s does not.
///
/// `ClientRequestToken` (12.4): the transaction also writes a token record,
/// an item of the reserved table id 0 (never a table's: ids start at 1)
/// keyed by a hash of the caller and the token, holding a hash of the
/// request and an expiry ten minutes out. Its condition lets it through
/// only if no unexpired record exists, and reports the stored one
/// otherwise; so a retry of a committed request finds its own record and
/// answers success without applying anything again, and a changed request
/// with the same token answers `IdempotentParameterMismatchException`. A
/// refused transaction writes no record, so its retry runs again, as
/// DynamoDB Local's does. Expired records stay until the same token comes
/// back; TTL (task 13) will remove them.
///
/// `TransactGetItems` (12.5) locks every item it reads in one transaction of
/// `read_lock` actions and reads each under its lock, so the reads are
/// serializable with the writes around them.
///
/// `ConsumedCapacity` counts each action twice, as transactional reads and
/// writes cost on AWS; task 12 brings the exact formula. Transactions do
/// not answer `ItemCollectionMetrics` yet (DynamoDB Local never does).

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
#include <kythira/dynamodb/table_api.hpp>

#include <raft/aws_json_protocol.hpp>
#include <raft/persistent_state_machine.hpp>

#include <boost/json.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kythira::dynamodb {

namespace txn_detail {

inline constexpr std::size_t k_max_request_bytes = std::size_t{4} * 1024 * 1024;
/// How long a committed request's token answers for it (Requirement 12.4).
inline constexpr std::uint64_t k_token_window_ms = std::uint64_t{10} * 60 * 1000;
/// The reserved table id that holds token records.
inline constexpr std::uint64_t k_token_table_id = 0;

[[nodiscard]] inline auto token_schema() -> key_schema {
    key_schema s;
    s.partition_name = "t";
    s.partition_type = attribute_type::S;
    return s;
}

[[nodiscard]] inline auto hex_sha256(std::string_view text) -> std::string {
    static constexpr char k_digits[] = "0123456789abcdef";
    const auto digest = kythira::sha256_of(text);
    std::string out;
    out.reserve(digest.size() * 2);
    for (const auto b : digest) {
        out.push_back(k_digits[b >> 4U]);
        out.push_back(k_digits[b & 0x0FU]);
    }
    return out;
}

[[nodiscard]] inline auto multiple_on_one_item() -> aws_json::error {
    return validation_error("Transaction request cannot include multiple operations on one item");
}

/// A transaction naming a multi-region strongly consistent table (task
/// 27): AWS documents that these "will return an error"; the code and
/// wording here are Kythira's, not recorded against AWS.
[[nodiscard]] inline auto strong_refused(std::string_view table) -> aws_json::error {
    return validation_error("Transactions are not supported for multi-Region strongly consistent "
                            "global tables: " +
                            std::string(table));
}

/// The reason code an action's refusal maps to.
[[nodiscard]] inline auto reason_code(const item_result& r) -> std::string {
    if (r.outcome == item_result::status::condition_failed) {
        return "ConditionalCheckFailed";
    }
    if (r.error_type.ends_with("ItemCollectionSizeLimitExceededException")) {
        return "ItemCollectionSizeLimitExceeded";
    }
    return "ValidationError";
}

}  // namespace txn_detail

/// The transaction handlers over a backend.
template<dynamo_backend Backend> class txn_api {
public:
    using result = aws_json::result<boost::json::object>;

    /// `guard` may be null (no disk floor).
    explicit txn_api(Backend& backend, const disk_guard* guard = nullptr)
        : _backend(backend), _guard(guard) {}

    /// The cluster's compatibility switches (Requirement 3); AWS's by

    /// default. A table's tags override them.

    void set_compat_profile(const dynamo_compat_profile& p) { _profile = p; }

    template<typename FrontEnd> void register_with(FrontEnd& fe) {
        fe.on("TransactWriteItems",
              [this](call_context& c, boost::json::object& in) { return transact_write(c, in); });
        fe.on("TransactGetItems",
              [this](call_context& c, boost::json::object& in) { return transact_get(c, in); });
    }

    auto transact_write(call_context& ctx, boost::json::object& in) -> result {
        // The codec has checked 1 to 100 elements and each action's
        // required members.
        const auto& elements = in.at("TransactItems").as_array();
        std::vector<action> actions;
        std::set<std::string> seen;
        std::size_t bytes = 0;
        for (const auto& element : elements) {
            const auto& e = element.as_object();
            static constexpr std::string_view k_kinds[] = {"ConditionCheck", "Put", "Delete",
                                                           "Update"};
            std::string_view kind;
            std::size_t set = 0;
            for (const auto k : k_kinds) {
                if (e.contains(k)) {
                    kind = k;
                    ++set;
                }
            }
            if (set == 0) {
                return validation_error(
                    "Invalid Request: TransactWriteRequest should contain Delete or Put or Update "
                    "request");
            }
            if (set > 1) {
                return validation_error(
                    "TransactItems can only contain one of Check, Put, Update or Delete");
            }
            auto a = parse_write(ctx, kind, e.at(kind).as_object());
            if (!a) {
                return a.error();
            }
            if (a->table.strong) {
                return txn_detail::strong_refused(a->table.name);
            }
            if (!seen.insert(*base_item_key(a->table.table_id, a->table.schema, a->command.payload))
                     .second) {
                return txn_detail::multiple_on_one_item();
            }
            bytes += item_size(a->command.payload);
            actions.push_back(std::move(*a));
        }
        if (bytes > txn_detail::k_max_request_bytes) {
            return validation_error("Transaction request size exceeded the 4 MB limit");
        }

        std::vector<item_command> commands;
        commands.reserve(actions.size() + 1);
        for (const auto& a : actions) {
            commands.push_back(a.command);
        }
        std::optional<std::string> request_hash;
        if (const auto token = api_detail::text(in, "ClientRequestToken"); !token.empty()) {
            request_hash = txn_detail::hex_sha256(boost::json::serialize(elements));
            commands.push_back(token_command(ctx, token, *request_hash));
        }

        if (_guard != nullptr) {
            if (auto e = _guard->check_write()) {
                return *e;
            }
        }
        if (auto e = admit_all(ctx, actions)) {
            return *e;
        }
        const auto report = _backend.transact(commands);

        if (request_hash) {
            const auto& token = report.results.back();
            if (token && token->outcome == item_result::status::condition_failed) {
                const auto* stored =
                    token->attributes ? find_attribute(*token->attributes, "h") : nullptr;
                if (stored != nullptr && *stored == attribute_value::string(*request_hash)) {
                    // The first attempt committed: answer as it did.
                    return write_response(in, actions);
                }
                return table_api_detail::modeled("IdempotentParameterMismatchException", "");
            }
        }
        charge_all(actions, report);
        if (report.outcome == txn_report::status::committed) {
            for (const auto& a : actions) {
                if (a.kind != "ConditionCheck" && api_detail::sync_gsis(_profile, a.table)) {
                    _backend.settle_item(a.table, a.command.payload);
                }
            }
            return write_response(in, actions, &report);
        }
        for (std::size_t i = 0; i < actions.size(); ++i) {
            const auto& r = report.results[i];
            if (r && r->outcome == item_result::status::error &&
                r->error_type == dynamodb_error_style().validation_type) {
                // An update that cannot apply to the stored item.
                aws_json::error e;
                e.type = r->error_type;
                e.message = r->error_message;
                return e;
            }
        }
        return cancelled(actions, report);
    }

    auto transact_get(call_context& ctx, boost::json::object& in) -> result {
        const auto& elements = in.at("TransactItems").as_array();
        std::vector<action> actions;
        std::set<std::string> seen;
        for (const auto& element : elements) {
            const auto& get = element.as_object().at("Get").as_object();
            auto table = api_detail::usable_table(_backend, get.at("TableName").as_string());
            if (!table) {
                return table.error();
            }
            if (table->strong) {
                return txn_detail::strong_refused(table->name);
            }
            if (auto denied = authorize(ctx, "GetItem", table->name)) {
                return *denied;
            }
            request_keys keys;
            keys.table = table->schema;
            auto compiled = compile_request("GetItem", get, keys);
            if (!compiled) {
                return compiled.error();
            }
            auto key = parse_key(get, *table);
            if (!key) {
                return key.error();
            }
            action a;
            a.kind = "Get";
            a.command = command(item_command::kind::read_lock, *table, std::move(*key));
            a.projection = std::move(compiled->projection);
            a.table = std::move(*table);
            if (!seen.insert(*base_item_key(a.table.table_id, a.table.schema, a.command.payload))
                     .second) {
                return txn_detail::multiple_on_one_item();
            }
            actions.push_back(std::move(a));
        }

        std::vector<item_command> commands;
        commands.reserve(actions.size());
        for (const auto& a : actions) {
            commands.push_back(a.command);
        }
        if (auto e = admit_all(ctx, actions)) {
            return *e;
        }
        const auto report = _backend.transact(commands);
        charge_all(actions, report);
        if (report.outcome == txn_report::status::cancelled) {
            return cancelled(actions, report);
        }
        boost::json::array responses;
        std::vector<std::pair<std::string, double>> units;
        for (std::size_t i = 0; i < actions.size(); ++i) {
            const auto& a = actions[i];
            const auto& r = report.results[i];
            boost::json::object entry;
            if (r && r->attributes) {
                entry["Item"] = item_to_json(
                    a.projection ? project_item(*a.projection, *r->attributes) : *r->attributes);
            }
            responses.push_back(std::move(entry));
            add_units(units, a.table.name, action_units(a, r));
        }
        boost::json::object out{{"Responses", std::move(responses)}};
        if (api_detail::wants_capacity(in)) {
            out["ConsumedCapacity"] = capacity_list(in, units);
        }
        return out;
    }

private:
    /// One element of a transaction, checked and turned into its command.
    struct action {
        std::string_view kind;
        table_descriptor table;
        item_command command;
        std::optional<projection_expr> projection;
    };

    auto authorize(call_context& ctx, std::string_view item_action, const std::string& table)
        -> std::optional<aws_json::error> {
        if (!ctx.authorize) {
            return std::nullopt;
        }
        aws_auth::authz_request r;
        r.action = "dynamodb:" + std::string(item_action);
        r.resource = ctx.table_arn(table);
        return ctx.authorize(std::move(r));
    }

    static auto parse_key(const boost::json::object& in, const table_descriptor& table)
        -> aws_json::result<item> {
        auto key = item_from_json(in.at("Key").as_object());
        if (!key) {
            return key.error();
        }
        if (auto e = validate_key(*key, table.schema)) {
            return *e;
        }
        return std::move(*key);
    }

    static auto command(item_command::kind op, const table_descriptor& table, item payload)
        -> item_command {
        item_command c;
        c.op = op;
        c.table_id = table.table_id;
        c.schema = table.schema;
        c.payload = std::move(payload);
        c.lsis = table.lsis;
        c.gsis = maintained_indexes(table.gsis);
        c.log_changes = table.logs_changes();
        c.stream_ms = table.stream_label();
        c.region = table.region;
        return c;
    }

    /// One `TransactWriteItems` action, checked as its single-item call
    /// would check it.
    auto parse_write(call_context& ctx, std::string_view kind, const boost::json::object& in)
        -> aws_json::result<action> {
        auto table = api_detail::usable_table(_backend, in.at("TableName").as_string());
        if (!table) {
            return table.error();
        }
        const std::string_view iam_action = kind == "ConditionCheck" ? "ConditionCheckItem"
                                            : kind == "Put"          ? "PutItem"
                                            : kind == "Delete"       ? "DeleteItem"
                                                                     : "UpdateItem";
        if (auto denied = authorize(ctx, iam_action, table->name)) {
            return *denied;
        }
        // A ConditionCheck takes what a DeleteItem takes: a key and a
        // condition.
        const std::string_view op = kind == "Put"      ? "PutItem"
                                    : kind == "Update" ? "UpdateItem"
                                                       : "DeleteItem";
        request_keys keys;
        keys.table = table->schema;
        auto compiled = compile_request(op, in, keys);
        if (!compiled) {
            return compiled.error();
        }
        action a;
        a.kind = kind;
        if (kind == "Put") {
            auto parsed = item_from_json(in.at("Item").as_object());
            if (!parsed) {
                return parsed.error();
            }
            if (auto e = validate_key_attributes(*parsed, table->schema)) {
                return *e;
            }
            if (auto e = check_item_size(*parsed)) {
                return *e;
            }
            if (auto e = validate_index_keys(*parsed, table->lsis)) {
                return *e;
            }
            if (auto e = validate_global_index_keys(*parsed, maintained_indexes(table->gsis))) {
                return *e;
            }
            a.command = command(item_command::kind::put, *table, std::move(*parsed));
        } else {
            auto key = parse_key(in, *table);
            if (!key) {
                return key.error();
            }
            const auto k = kind == "ConditionCheck" ? item_command::kind::condition_check
                           : kind == "Delete"       ? item_command::kind::delete_
                                                    : item_command::kind::update;
            a.command = command(k, *table, std::move(*key));
        }
        a.command.expressions = std::move(*compiled);
        a.command.old_on_condition_failure =
            api_detail::text(in, "ReturnValuesOnConditionCheckFailure") == "ALL_OLD";
        a.table = std::move(*table);
        return a;
    }

    /// The token record's write: allowed only where no unexpired record
    /// stands, and reporting the one that does.
    auto token_command(const call_context& ctx, std::string_view token,
                       const std::string& request_hash) -> item_command {
        const auto now = _backend.now_ms();
        item_command c;
        c.op = item_command::kind::put;
        c.table_id = txn_detail::k_token_table_id;
        c.schema = txn_detail::token_schema();
        // Attributes in name order, as an item keeps them.
        c.payload = item{
            {"e", attribute_value::number(*dynamo_decimal::from_string(
                      std::to_string(now + txn_detail::k_token_window_ms)))},
            {"h", attribute_value::string(request_hash)},
            {"t", attribute_value::string(
                      txn_detail::hex_sha256(ctx.principal.arn + "\n" + std::string(token)))},
        };
        boost::json::object condition{
            {"ConditionExpression", "attribute_not_exists(#t) OR #e < :now"},
            {"ExpressionAttributeNames", boost::json::object{{"#t", "t"}, {"#e", "e"}}},
            {"ExpressionAttributeValues",
             boost::json::object{{":now", boost::json::object{{"N", std::to_string(now)}}}}},
        };
        request_keys keys;
        keys.table = c.schema;
        auto compiled = compile_request("PutItem", condition, keys);
        if (!compiled) {
            throw std::logic_error("txn_api: the token condition does not compile");
        }
        c.expressions = std::move(*compiled);
        c.old_on_condition_failure = true;
        return c;
    }

    static auto add_units(std::vector<std::pair<std::string, double>>& units,
                          const std::string& table, double n) -> void {
        for (auto& [name, total] : units) {
            if (name == table) {
                total += n;
                return;
            }
        }
        units.emplace_back(table, n);
    }

    static auto capacity_list(const boost::json::object& in,
                              const std::vector<std::pair<std::string, double>>& units)
        -> boost::json::array {
        boost::json::array out;
        for (const auto& [name, n] : units) {
            out.push_back(api_detail::capacity(in, name, n));
        }
        return out;
    }

    /// What one action of a transaction costs: twice the plain read or
    /// write, sized from what the item group reported when it did.
    static auto action_units(const action& a, const std::optional<item_result>& r) -> double {
        if (a.kind == "Get") {
            return 2.0 *
                   api_detail::read_units(r && r->attributes ? item_size(*r->attributes) : 0, true);
        }
        if (r && r->charged_bytes != 0) {
            return 2.0 * api_detail::write_units(r->charged_bytes);
        }
        return 2.0 * (a.command.op == item_command::kind::put
                          ? api_detail::write_units(item_size(a.command.payload))
                          : 1.0);
    }

    static auto targets_of(const action& a) -> std::vector<capacity_target> {
        if (a.kind == "Get") {
            return {api_detail::read_target(a.table, a.command.payload)};
        }
        return api_detail::write_targets(a.table, a.command.payload, a.command.op);
    }

    /// Admits every action, or cancels the transaction naming the throttled
    /// ones with `ThrottlingError`, as AWS does.
    auto admit_all(const call_context& ctx, const std::vector<action>& actions)
        -> std::optional<aws_json::error> {
        std::vector<std::optional<aws_json::error>> refused(actions.size());
        bool any = false;
        for (std::size_t i = 0; i < actions.size(); ++i) {
            refused[i] = api_detail::admit(_backend, _profile, ctx, actions[i].table,
                                           targets_of(actions[i]));
            any = any || refused[i].has_value();
        }
        if (!any) {
            return std::nullopt;
        }
        boost::json::array reasons;
        std::string codes;
        for (std::size_t i = 0; i < actions.size(); ++i) {
            boost::json::object reason;
            if (refused[i]) {
                reason["Code"] = "ThrottlingError";
                reason["Message"] = std::string(capacity_detail::k_hot_message);
            } else {
                reason["Code"] = "None";
            }
            codes += (i == 0 ? "" : ", ") + std::string(reason.at("Code").as_string());
            reasons.push_back(std::move(reason));
        }
        auto e = table_api_detail::modeled(
            "TransactionCanceledException",
            "Transaction cancelled, please refer cancellation reasons for specific reasons [" +
                codes + "]");
        e.extra["CancellationReasons"] = std::move(reasons);
        return e;
    }

    /// Charges every action that ran, committed or not.
    auto charge_all(const std::vector<action>& actions, const txn_report& report) -> void {
        for (std::size_t i = 0; i < actions.size(); ++i) {
            const auto& r =
                i < report.results.size() ? report.results[i] : std::optional<item_result>{};
            api_detail::charge_all(_backend, _profile, actions[i].table, targets_of(actions[i]),
                                   action_units(actions[i], r));
        }
    }

    static auto write_response(const boost::json::object& in, const std::vector<action>& actions,
                               const txn_report* report = nullptr) -> boost::json::object {
        boost::json::object out;
        if (api_detail::wants_capacity(in)) {
            std::vector<std::pair<std::string, double>> units;
            for (std::size_t i = 0; i < actions.size(); ++i) {
                const auto& a = actions[i];
                const auto r = report != nullptr && i < report->results.size()
                                   ? report->results[i]
                                   : std::optional<item_result>{};
                add_units(units, a.table.name, action_units(a, r));
            }
            out["ConsumedCapacity"] = capacity_list(in, units);
        }
        return out;
    }

    /// `TransactionCanceledException` with one reason per action.
    static auto cancelled(const std::vector<action>& actions, const txn_report& report)
        -> aws_json::error {
        boost::json::array reasons;
        std::string codes;
        for (std::size_t i = 0; i < actions.size(); ++i) {
            boost::json::object reason;
            const auto& r = report.results[i];
            if (report.conflicts[i]) {
                reason["Code"] = "TransactionConflict";
                reason["Message"] = "Transaction is ongoing for the item";
            } else if (r && r->outcome != item_result::status::ok) {
                const auto code = txn_detail::reason_code(*r);
                reason["Code"] = code;
                if (r->outcome == item_result::status::condition_failed) {
                    reason["Message"] = actions[i].kind == "ConditionCheck"
                                            ? "The conditional request failed."
                                            : "The conditional request failed";
                    if (r->attributes) {
                        reason["Item"] = item_to_json(*r->attributes);
                    }
                } else {
                    reason["Message"] = r->error_message;
                }
            } else {
                reason["Code"] = "None";
            }
            codes += (i == 0 ? "" : ", ") + std::string(reason.at("Code").as_string());
            reasons.push_back(std::move(reason));
        }
        auto e = table_api_detail::modeled(
            "TransactionCanceledException",
            "Transaction cancelled, please refer cancellation reasons for specific reasons [" +
                codes + "]");
        e.extra["CancellationReasons"] = std::move(reasons);
        return e;
    }

    Backend& _backend;
    const disk_guard* _guard;
    dynamo_compat_profile _profile{};
};

}  // namespace kythira::dynamodb
