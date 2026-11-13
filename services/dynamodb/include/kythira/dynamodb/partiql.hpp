// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file partiql.hpp
/// @brief PartiQL for DynamoDB: `ExecuteStatement`, `BatchExecuteStatement`
///        and `ExecuteTransaction` (`.kiro/specs/dynamodb-compatible-api/`
///        task 14, Requirements 13.1 and 13.2).
///
/// A statement is parsed here and compiled into the JSON API request it
/// stands for, which the JSON API's own handler then runs, so behaviour,
/// limits, errors and capacity are the JSON API's (13.2). Paths become
/// `ExpressionAttributeNames` and literals and `?` parameters become
/// `ExpressionAttributeValues`, so the expression compiler sees ordinary
/// expressions and no reserved word gets in the way.
///
/// | Statement | Runs as |
/// |---|---|
/// | `SELECT ... WHERE <pk> = v [AND <sk cond>] [AND ...]` | `Query`, the rest a filter |
/// | `SELECT ... WHERE <pk> IN [v, ...]` | one `Query` per value, in order |
/// | any other `SELECT` | `Scan` with the whole condition as a filter |
/// | `INSERT INTO t VALUE {...}` | `PutItem` unless the key exists (`DuplicateItemException`) |
/// | `UPDATE t SET ... REMOVE ... WHERE <full key> [AND ...]` | `UpdateItem` on an existing item |
/// | `DELETE FROM t WHERE <full key> [AND ...]` | `DeleteItem` |
/// | `EXISTS(SELECT ... WHERE <full key> AND ...)` (transactions) | `ConditionCheck` |
///
/// The grammar is the subset AWS documents for DynamoDB: `"quoted"` or bare
/// identifiers, `t."index"`, paths with `.name` and `[n]`, `'strings'`,
/// numbers, `TRUE`/`FALSE`/`NULL`, `[lists]`, `{'tuples': ...}`,
/// `<<sets>>`, `?` parameters; `AND`, `OR`, `NOT`, comparisons, `BETWEEN`,
/// `IN`, `IS [NOT] MISSING`, `IS [NOT] NULL`; the functions `begins_with`,
/// `contains`, `attribute_type`, `attribute_exists`, `attribute_not_exists`,
/// `size`, and in `SET` `list_append`, `set_add`, `set_delete` and
/// `if_not_exists`; `ORDER BY` the sort key; `RETURNING ALL|MODIFIED
/// OLD|NEW *`.
///
/// Answers follow DynamoDB Local 3.3, request for request: writes answer
/// `{"Items": []}`, an `UPDATE` of a missing item fails its condition, a
/// `DELETE` of one succeeds, and a nested path in a `SELECT` list comes back
/// under its last name (`m.k` as `k`, `l[1]` as `l[1]`). Where Local's error
/// differs from AWS's model the model's name is used: `DuplicateItemException`
/// (Local says `DuplicateItem`).
///
/// Authorization is the PartiQL action of each statement (`PartiQLSelect`,
/// `PartiQLInsert`, `PartiQLUpdate`, `PartiQLDelete`) on its table or index,
/// through `call_context::authorize`; the JSON API handlers then run with no
/// further checks.

#include <kythira/dynamodb/api_common.hpp>
#include <kythira/dynamodb/backend.hpp>
#include <kythira/dynamodb/errors.hpp>
#include <kythira/dynamodb/front_end.hpp>
#include <kythira/dynamodb/global_index.hpp>
#include <kythira/dynamodb/item_api.hpp>
#include <kythira/dynamodb/item_codec.hpp>
#include <kythira/dynamodb/local_index.hpp>
#include <kythira/dynamodb/query_api.hpp>
#include <kythira/dynamodb/table_api.hpp>
#include <kythira/dynamodb/txn_api.hpp>

#include <raft/aws_json_protocol.hpp>

#include <boost/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kythira::dynamodb {

namespace partiql_detail {

// ---------------------------------------------------------------------------
// Tokens
// ---------------------------------------------------------------------------

enum class tok : std::uint8_t {
    word,    // bare identifier or keyword
    quoted,  // "identifier"
    string,  // 'string'
    number,
    param,  // ?
    punct,
    end,
};

struct token {
    tok kind = tok::end;
    std::string text;
};

/// The statement is not PartiQL this module reads.
struct malformed {};

/// A statement that parsed but cannot run, with DynamoDB's message.
struct refused {
    std::string message;
};

[[nodiscard]] inline auto upper(std::string_view s) -> std::string {
    std::string out(s);
    for (auto& c : out) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return out;
}

[[nodiscard]] inline auto tokenize(std::string_view s) -> std::vector<token> {
    std::vector<token> out;
    std::size_t i = 0;
    const auto at = [&](std::size_t k) { return k < s.size() ? s[k] : '\0'; };
    while (i < s.size()) {
        const char c = s[i];
        if (std::isspace(static_cast<unsigned char>(c)) != 0) {
            ++i;
            continue;
        }
        if (std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_') {
            std::size_t j = i;
            while (j < s.size() && (std::isalnum(static_cast<unsigned char>(s[j])) != 0 ||
                                    s[j] == '_' || s[j] == '$')) {
                ++j;
            }
            out.push_back({tok::word, std::string(s.substr(i, j - i))});
            i = j;
            continue;
        }
        if (std::isdigit(static_cast<unsigned char>(c)) != 0 ||
            (c == '.' && std::isdigit(static_cast<unsigned char>(at(i + 1))) != 0)) {
            std::size_t j = i;
            while (j < s.size() &&
                   (std::isdigit(static_cast<unsigned char>(s[j])) != 0 || s[j] == '.')) {
                ++j;
            }
            if (j < s.size() && (s[j] == 'e' || s[j] == 'E')) {
                ++j;
                if (j < s.size() && (s[j] == '+' || s[j] == '-')) {
                    ++j;
                }
                while (j < s.size() && std::isdigit(static_cast<unsigned char>(s[j])) != 0) {
                    ++j;
                }
            }
            out.push_back({tok::number, std::string(s.substr(i, j - i))});
            i = j;
            continue;
        }
        if (c == '\'' || c == '"') {
            std::string text;
            std::size_t j = i + 1;
            bool closed = false;
            while (j < s.size()) {
                if (s[j] == c) {
                    if (at(j + 1) == c) {  // a doubled quote is the quote
                        text.push_back(c);
                        j += 2;
                        continue;
                    }
                    closed = true;
                    ++j;
                    break;
                }
                text.push_back(s[j++]);
            }
            if (!closed) {
                throw malformed{};
            }
            out.push_back({c == '\'' ? tok::string : tok::quoted, std::move(text)});
            i = j;
            continue;
        }
        if (c == '?') {
            out.push_back({tok::param, "?"});
            ++i;
            continue;
        }
        static constexpr std::string_view k_two[] = {"<<", ">>", "<=", ">=", "<>", "!="};
        bool two = false;
        for (const auto t : k_two) {
            if (s.substr(i, 2) == t) {
                out.push_back({tok::punct, std::string(t)});
                i += 2;
                two = true;
                break;
            }
        }
        if (two) {
            continue;
        }
        if (std::string_view("()[]{},.:*=<>+-;").find(c) != std::string_view::npos) {
            out.push_back({tok::punct, std::string(1, c)});
            ++i;
            continue;
        }
        throw malformed{};
    }
    out.push_back({tok::end, ""});
    return out;
}

// ---------------------------------------------------------------------------
// Syntax tree
// ---------------------------------------------------------------------------

struct path_seg {
    std::string name;
    std::optional<std::size_t> index;  // set: `[n]`
};

struct path {
    std::vector<path_seg> segs;

    [[nodiscard]] auto top() const -> const std::string& { return segs.front().name; }
    [[nodiscard]] auto simple() const -> bool { return segs.size() == 1; }
};

/// An operand: a path, a literal (an AttributeValue in JSON form), a
/// function call, or `a + b` / `a - b`.
struct operand {
    enum class kind : std::uint8_t {
        path_,
        literal,
        call,
        arith
    };
    kind k = kind::literal;
    struct path p;
    boost::json::object literal;
    std::string fn;  // lower case; for arith, "+" or "-"
    std::vector<operand> args;
};

struct cond {
    enum class kind : std::uint8_t {
        and_,
        or_,
        not_,
        cmp,
        between,
        in,
        is_missing,
        is_not_missing,
        is_null,
        is_not_null,
        call,
    };
    kind k = kind::cmp;
    std::string op;  // cmp: the operator; call: the function
    std::vector<cond> kids;
    std::vector<operand> args;
};

struct table_ref {
    std::string table;
    std::string index;
};

struct assignment {
    bool remove = false;
    struct path target;
    operand value;
};

struct statement {
    enum class kind : std::uint8_t {
        select,
        insert,
        update,
        delete_,
        exists
    };
    kind k = kind::select;
    table_ref from;
    /// SELECT: the projected paths; empty for `*`.
    std::vector<path> projection;
    std::optional<cond> where;
    /// ORDER BY ... DESC.
    bool descending = false;
    bool ordered = false;
    /// INSERT: the item.
    boost::json::object item;
    std::vector<assignment> assignments;
    /// RETURNING: ALL_OLD, UPDATED_OLD, ALL_NEW, UPDATED_NEW, or empty.
    std::string returning;
};

// ---------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------

class parser {
public:
    parser(std::vector<token> toks, const boost::json::array* params)
        : _t(std::move(toks)), _params(params) {}

    auto parse() -> statement {
        statement s;
        if (keyword("SELECT")) {
            s = select_rest();
        } else if (keyword("INSERT")) {
            s = insert_rest();
        } else if (keyword("UPDATE")) {
            s = update_rest();
        } else if (keyword("DELETE")) {
            s = delete_rest();
        } else if (keyword("EXISTS")) {
            expect("(");
            if (!keyword("SELECT")) {
                throw malformed{};
            }
            s = select_rest();
            expect(")");
            s.k = statement::kind::exists;
        } else {
            throw malformed{};
        }
        (void)punct(";");
        if (peek().kind != tok::end) {
            throw malformed{};
        }
        const auto given = _params == nullptr ? 0 : _params->size();
        if (given != _next_param) {
            throw refused{"Number of parameters in request and statement don't match."};
        }
        return s;
    }

private:
    [[nodiscard]] auto peek(std::size_t ahead = 0) const -> const token& {
        return _t[std::min(_at + ahead, _t.size() - 1)];
    }
    auto next() -> const token& {
        const auto& t = peek();
        if (_at < _t.size() - 1) {
            ++_at;
        }
        return t;
    }
    [[nodiscard]] auto is_keyword(std::string_view k, std::size_t ahead = 0) const -> bool {
        const auto& t = peek(ahead);
        return t.kind == tok::word && upper(t.text) == k;
    }
    auto keyword(std::string_view k) -> bool {
        if (is_keyword(k)) {
            next();
            return true;
        }
        return false;
    }
    auto expect_keyword(std::string_view k) -> void {
        if (!keyword(k)) {
            throw malformed{};
        }
    }
    [[nodiscard]] auto is_punct(std::string_view p, std::size_t ahead = 0) const -> bool {
        const auto& t = peek(ahead);
        return t.kind == tok::punct && t.text == p;
    }
    auto punct(std::string_view p) -> bool {
        if (is_punct(p)) {
            next();
            return true;
        }
        return false;
    }
    auto expect(std::string_view p) -> void {
        if (!punct(p)) {
            throw malformed{};
        }
    }

    auto name() -> std::string {
        const auto& t = peek();
        if (t.kind == tok::word || t.kind == tok::quoted) {
            return next().text;
        }
        throw malformed{};
    }

    auto table() -> table_ref {
        table_ref r;
        r.table = name();
        if (punct(".")) {
            r.index = name();
        }
        return r;
    }

    auto parse_path() -> path {
        path p;
        p.segs.push_back({name(), std::nullopt});
        for (;;) {
            if (punct(".")) {
                p.segs.push_back({name(), std::nullopt});
            } else if (is_punct("[")) {
                next();
                const auto& t = next();
                if (t.kind == tok::number &&
                    t.text.find_first_not_of("0123456789") == std::string::npos) {
                    p.segs.push_back({"", static_cast<std::size_t>(std::stoull(t.text))});
                } else if (t.kind == tok::string) {
                    p.segs.push_back({t.text, std::nullopt});
                } else {
                    throw malformed{};
                }
                expect("]");
            } else {
                return p;
            }
        }
    }

    auto select_rest() -> statement {
        statement s;
        s.k = statement::kind::select;
        if (!punct("*")) {
            do {
                s.projection.push_back(parse_path());
            } while (punct(","));
        }
        expect_keyword("FROM");
        s.from = table();
        if (keyword("WHERE")) {
            s.where = parse_or();
        }
        if (keyword("ORDER")) {
            expect_keyword("BY");
            (void)parse_path();
            s.ordered = true;
            if (keyword("DESC")) {
                s.descending = true;
            } else {
                (void)keyword("ASC");
            }
        }
        return s;
    }

    auto insert_rest() -> statement {
        statement s;
        s.k = statement::kind::insert;
        expect_keyword("INTO");
        s.from = table();
        expect_keyword("VALUE");
        auto v = literal();
        if (!v.contains("M")) {
            throw malformed{};
        }
        s.item = v.at("M").as_object();
        return s;
    }

    auto update_rest() -> statement {
        statement s;
        s.k = statement::kind::update;
        s.from = table();
        bool any = false;
        for (;;) {
            if (keyword("SET")) {
                do {
                    assignment a;
                    a.target = parse_path();
                    expect("=");
                    a.value = parse_operand();
                    s.assignments.push_back(std::move(a));
                } while (punct(","));
                any = true;
            } else if (keyword("REMOVE")) {
                do {
                    assignment a;
                    a.remove = true;
                    a.target = parse_path();
                    s.assignments.push_back(std::move(a));
                } while (punct(","));
                any = true;
            } else {
                break;
            }
        }
        if (!any) {
            throw malformed{};
        }
        expect_keyword("WHERE");
        s.where = parse_or();
        s.returning = returning();
        return s;
    }

    auto delete_rest() -> statement {
        statement s;
        s.k = statement::kind::delete_;
        expect_keyword("FROM");
        s.from = table();
        expect_keyword("WHERE");
        s.where = parse_or();
        s.returning = returning();
        return s;
    }

    auto returning() -> std::string {
        if (!keyword("RETURNING")) {
            return {};
        }
        std::string which;
        if (keyword("ALL")) {
            which = "ALL_";
        } else if (keyword("MODIFIED")) {
            which = "UPDATED_";
        } else {
            throw malformed{};
        }
        if (keyword("OLD")) {
            which += "OLD";
        } else if (keyword("NEW")) {
            which += "NEW";
        } else {
            throw malformed{};
        }
        expect("*");
        return which;
    }

    // -- Conditions -----------------------------------------------------------

    auto parse_or() -> cond {
        auto left = parse_and();
        while (keyword("OR")) {
            cond c;
            c.k = cond::kind::or_;
            c.kids.push_back(std::move(left));
            c.kids.push_back(parse_and());
            left = std::move(c);
        }
        return left;
    }

    auto parse_and() -> cond {
        auto left = parse_not();
        while (keyword("AND")) {
            cond c;
            c.k = cond::kind::and_;
            c.kids.push_back(std::move(left));
            c.kids.push_back(parse_not());
            left = std::move(c);
        }
        return left;
    }

    auto parse_not() -> cond {
        if (keyword("NOT")) {
            cond c;
            c.k = cond::kind::not_;
            c.kids.push_back(parse_not());
            return c;
        }
        return parse_predicate();
    }

    auto parse_predicate() -> cond {
        if (is_punct("(")) {
            // A parenthesized condition (an operand never starts with one).
            next();
            auto c = parse_or();
            expect(")");
            return c;
        }
        auto left = parse_operand();
        const auto& t = peek();
        if (t.kind == tok::punct &&
            (t.text == "=" || t.text == "<>" || t.text == "!=" || t.text == "<" || t.text == "<=" ||
             t.text == ">" || t.text == ">=")) {
            cond c;
            c.k = cond::kind::cmp;
            c.op = next().text == "!=" ? "<>" : _t[_at - 1].text;
            c.args.push_back(std::move(left));
            c.args.push_back(parse_operand());
            return c;
        }
        bool negate = false;
        if (is_keyword("NOT") && (is_keyword("BETWEEN", 1) || is_keyword("IN", 1))) {
            next();
            negate = true;
        }
        cond c;
        if (keyword("BETWEEN")) {
            c.k = cond::kind::between;
            c.args.push_back(std::move(left));
            c.args.push_back(parse_operand());
            expect_keyword("AND");
            c.args.push_back(parse_operand());
        } else if (keyword("IN")) {
            c.k = cond::kind::in;
            c.args.push_back(std::move(left));
            const bool square = punct("[");
            if (!square) {
                expect("(");
            }
            do {
                c.args.push_back(parse_operand());
            } while (punct(","));
            expect(square ? "]" : ")");
        } else if (keyword("IS")) {
            const bool is_not = keyword("NOT");
            if (keyword("MISSING")) {
                c.k = is_not ? cond::kind::is_not_missing : cond::kind::is_missing;
            } else if (keyword("NULL")) {
                c.k = is_not ? cond::kind::is_not_null : cond::kind::is_null;
            } else {
                throw malformed{};
            }
            c.args.push_back(std::move(left));
        } else if (left.k == operand::kind::call) {
            c.k = cond::kind::call;
            c.op = left.fn;
            c.args = std::move(left.args);
        } else {
            throw malformed{};
        }
        if (negate) {
            cond n;
            n.k = cond::kind::not_;
            n.kids.push_back(std::move(c));
            return n;
        }
        return c;
    }

    // -- Operands -------------------------------------------------------------

    auto parse_operand() -> operand {
        auto left = primary();
        while (is_punct("+") || is_punct("-")) {
            operand a;
            a.k = operand::kind::arith;
            a.fn = next().text;
            a.args.push_back(std::move(left));
            a.args.push_back(primary());
            left = std::move(a);
        }
        return left;
    }

    auto primary() -> operand {
        const auto& t = peek();
        if (t.kind == tok::word && is_punct("(", 1) && !is_literal_word(t.text)) {
            operand o;
            o.k = operand::kind::call;
            o.fn = t.text;
            std::transform(o.fn.begin(), o.fn.end(), o.fn.begin(),
                           [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            next();
            next();
            if (!punct(")")) {
                do {
                    o.args.push_back(parse_operand());
                } while (punct(","));
                expect(")");
            }
            return o;
        }
        if ((t.kind == tok::word && !is_literal_word(t.text)) || t.kind == tok::quoted) {
            operand o;
            o.k = operand::kind::path_;
            o.p = parse_path();
            return o;
        }
        operand o;
        o.k = operand::kind::literal;
        o.literal = literal();
        return o;
    }

    static auto is_literal_word(std::string_view w) -> bool {
        const auto u = upper(w);
        return u == "TRUE" || u == "FALSE" || u == "NULL" || u == "MISSING";
    }

    /// A literal as an AttributeValue.
    auto literal() -> boost::json::object {
        const auto& t = next();
        switch (t.kind) {
            case tok::string:
                return {{"S", t.text}};
            case tok::number:
                return {{"N", t.text}};
            case tok::param: {
                const auto i = _next_param++;
                if (_params == nullptr || i >= _params->size() || !(*_params)[i].is_object()) {
                    throw refused{"Number of parameters in request and statement don't match."};
                }
                return (*_params)[i].as_object();
            }
            case tok::word: {
                const auto u = upper(t.text);
                if (u == "TRUE" || u == "FALSE") {
                    return {{"BOOL", u == "TRUE"}};
                }
                if (u == "NULL") {
                    return {{"NULL", true}};
                }
                throw malformed{};
            }
            case tok::punct:
                if (t.text == "-" && peek().kind == tok::number) {
                    return {{"N", "-" + next().text}};
                }
                if (t.text == "[") {
                    boost::json::array l;
                    if (!punct("]")) {
                        do {
                            l.push_back(literal());
                        } while (punct(","));
                        expect("]");
                    }
                    return {{"L", std::move(l)}};
                }
                if (t.text == "{") {
                    boost::json::object m;
                    if (!punct("}")) {
                        do {
                            const auto& k = next();
                            if (k.kind != tok::string && k.kind != tok::quoted) {
                                throw malformed{};
                            }
                            std::string key = k.text;
                            expect(":");
                            m[key] = literal();
                        } while (punct(","));
                        expect("}");
                    }
                    return {{"M", std::move(m)}};
                }
                if (t.text == "<<") {
                    return set_literal();
                }
                throw malformed{};
            default:
                throw malformed{};
        }
    }

    auto set_literal() -> boost::json::object {
        boost::json::array members;
        std::string type;
        if (!punct(">>")) {
            do {
                auto v = literal();
                std::string t;
                if (v.contains("S")) {
                    t = "SS";
                    members.push_back(v.at("S"));
                } else if (v.contains("N")) {
                    t = "NS";
                    members.push_back(v.at("N"));
                } else if (v.contains("B")) {
                    t = "BS";
                    members.push_back(v.at("B"));
                } else {
                    throw refused{"Unsupported type passed for a set"};
                }
                if (!type.empty() && t != type) {
                    throw refused{"Mixed types are not supported in a set"};
                }
                type = t;
            } while (punct(","));
            expect(">>");
        }
        if (type.empty()) {
            throw refused{"Empty set is not supported"};
        }
        return {{type, std::move(members)}};
    }

    std::vector<token> _t;
    std::size_t _at = 0;
    const boost::json::array* _params;
    std::size_t _next_param = 0;
};

// ---------------------------------------------------------------------------
// Emitting expressions
// ---------------------------------------------------------------------------

/// Writes conditions and updates as expression text, collecting the names
/// and values they use.
class emitter {
public:
    auto name(const std::string& n) -> std::string {
        const auto it = _names.find(n);
        if (it != _names.end()) {
            return it->second;
        }
        auto ph = "#p" + std::to_string(_names.size());
        _names.emplace(n, ph);
        return ph;
    }

    auto value(const boost::json::object& v) -> std::string {
        auto ph = ":v" + std::to_string(_values.size());
        _values[ph] = v;
        return ph;
    }

    auto text(const path& p) -> std::string {
        std::string out;
        for (std::size_t i = 0; i < p.segs.size(); ++i) {
            const auto& s = p.segs[i];
            if (s.index) {
                out += "[" + std::to_string(*s.index) + "]";
            } else {
                out += (i == 0 ? "" : ".") + name(s.name);
            }
        }
        return out;
    }

    auto text(const operand& o) -> std::string {
        switch (o.k) {
            case operand::kind::path_:
                return text(o.p);
            case operand::kind::literal:
                return value(o.literal);
            case operand::kind::arith:
                return text(o.args[0]) + " " + o.fn + " " + text(o.args[1]);
            case operand::kind::call: {
                std::string out = o.fn + "(";
                for (std::size_t i = 0; i < o.args.size(); ++i) {
                    out += (i == 0 ? "" : ", ") + text(o.args[i]);
                }
                return out + ")";
            }
        }
        return {};
    }

    auto text(const cond& c) -> std::string {
        switch (c.k) {
            case cond::kind::and_:
                return "(" + text(c.kids[0]) + " AND " + text(c.kids[1]) + ")";
            case cond::kind::or_:
                return "(" + text(c.kids[0]) + " OR " + text(c.kids[1]) + ")";
            case cond::kind::not_:
                return "(NOT " + text(c.kids[0]) + ")";
            case cond::kind::cmp:
                return text(c.args[0]) + " " + c.op + " " + text(c.args[1]);
            case cond::kind::between:
                return text(c.args[0]) + " BETWEEN " + text(c.args[1]) + " AND " + text(c.args[2]);
            case cond::kind::in: {
                std::string out = text(c.args[0]) + " IN (";
                for (std::size_t i = 1; i < c.args.size(); ++i) {
                    out += (i == 1 ? "" : ", ") + text(c.args[i]);
                }
                return out + ")";
            }
            case cond::kind::is_missing:
                return "attribute_not_exists(" + text(c.args[0]) + ")";
            case cond::kind::is_not_missing:
                return "attribute_exists(" + text(c.args[0]) + ")";
            case cond::kind::is_null: {
                const auto p = text(c.args[0]);
                return "(attribute_not_exists(" + p + ") OR attribute_type(" + p + ", " +
                       value({{"S", "NULL"}}) + "))";
            }
            case cond::kind::is_not_null: {
                const auto p = text(c.args[0]);
                return "(attribute_exists(" + p + ") AND NOT attribute_type(" + p + ", " +
                       value({{"S", "NULL"}}) + "))";
            }
            case cond::kind::call: {
                std::string out = c.op + "(";
                for (std::size_t i = 0; i < c.args.size(); ++i) {
                    out += (i == 0 ? "" : ", ") + text(c.args[i]);
                }
                return out + ")";
            }
        }
        return {};
    }

    /// Puts the collected names and values on a request.
    auto attach(boost::json::object& req) const -> void {
        if (!_names.empty()) {
            boost::json::object n;
            for (const auto& [attr, ph] : _names) {
                n[ph] = attr;
            }
            req["ExpressionAttributeNames"] = std::move(n);
        }
        if (!_values.empty()) {
            req["ExpressionAttributeValues"] = _values;
        }
    }

private:
    std::map<std::string, std::string> _names;
    boost::json::object _values;
};

/// The top-level conjuncts of `c`.
inline void conjuncts(const cond& c, std::vector<const cond*>& out) {
    if (c.k == cond::kind::and_) {
        conjuncts(c.kids[0], out);
        conjuncts(c.kids[1], out);
    } else {
        out.push_back(&c);
    }
}

/// `c` names `key` alone on one side and a literal on the other: the
/// literal, and the operator as if the path were on the left.
struct key_term {
    std::string op;
    const boost::json::object* value = nullptr;
};

[[nodiscard]] inline auto is_key_path(const operand& o, const std::string& key) -> bool {
    return o.k == operand::kind::path_ && o.p.simple() && o.p.top() == key;
}

[[nodiscard]] inline auto key_comparison(const cond& c, const std::string& key)
    -> std::optional<key_term> {
    if (c.k != cond::kind::cmp || c.op == "<>") {
        return std::nullopt;
    }
    const auto& a = c.args[0];
    const auto& b = c.args[1];
    if (is_key_path(a, key) && b.k == operand::kind::literal) {
        return key_term{c.op, &b.literal};
    }
    if (is_key_path(b, key) && a.k == operand::kind::literal) {
        static const std::map<std::string, std::string> flip{
            {"=", "="}, {"<", ">"}, {"<=", ">="}, {">", "<"}, {">=", "<="}};
        return key_term{flip.at(c.op), &a.literal};
    }
    return std::nullopt;
}

/// Joins conditions with AND; nothing for none.
[[nodiscard]] inline auto join(emitter& e, const std::vector<const cond*>& cs)
    -> std::optional<std::string> {
    if (cs.empty()) {
        return std::nullopt;
    }
    std::string out;
    for (const auto* c : cs) {
        out += (out.empty() ? "" : " AND ") + e.text(*c);
    }
    return out;
}

/// The full key from equality conjuncts of `where`, and the conjuncts left.
struct keyed {
    boost::json::object key;
    std::vector<const cond*> rest;
};

[[nodiscard]] inline auto full_key(const std::optional<cond>& where, const key_schema& schema)
    -> std::optional<keyed> {
    if (!where) {
        return std::nullopt;
    }
    std::vector<const cond*> all;
    conjuncts(*where, all);
    keyed out;
    bool pk = false;
    bool sk = !schema.sort_name.has_value();
    for (const auto* c : all) {
        if (!pk) {
            if (auto t = key_comparison(*c, schema.partition_name); t && t->op == "=") {
                out.key[schema.partition_name] = *t->value;
                pk = true;
                continue;
            }
        }
        if (!sk) {
            if (auto t = key_comparison(*c, *schema.sort_name); t && t->op == "=") {
                out.key[*schema.sort_name] = *t->value;
                sk = true;
                continue;
            }
        }
        out.rest.push_back(c);
    }
    if (!pk || !sk) {
        return std::nullopt;
    }
    return out;
}

[[nodiscard]] inline auto no_full_key() -> refused {
    return refused{"Where clause does not contain a mandatory equality on all key attributes"};
}

/// The name a projected path comes back under, as DynamoDB Local names it:
/// its last name, with any indexes after that name.
[[nodiscard]] inline auto projected_name(const path& p) -> std::string {
    std::size_t last = 0;
    for (std::size_t i = 0; i < p.segs.size(); ++i) {
        if (!p.segs[i].index) {
            last = i;
        }
    }
    std::string out = p.segs[last].name;
    for (std::size_t i = last + 1; i < p.segs.size(); ++i) {
        out += "[" + std::to_string(*p.segs[i].index) + "]";
    }
    return out;
}

/// The value at `p` in an item in JSON form, or null.
[[nodiscard]] inline auto value_at(const boost::json::object& it, const path& p)
    -> const boost::json::value* {
    const boost::json::value* v = it.if_contains(p.segs.front().name);
    for (std::size_t i = 1; v != nullptr && i < p.segs.size(); ++i) {
        const auto& s = p.segs[i];
        const auto& av = v->as_object();
        if (s.index) {
            const auto* l = av.if_contains("L");
            v = l != nullptr && *s.index < l->as_array().size() ? &l->as_array()[*s.index]
                                                                : nullptr;
        } else {
            const auto* m = av.if_contains("M");
            v = m != nullptr ? m->as_object().if_contains(s.name) : nullptr;
        }
    }
    return v;
}

[[nodiscard]] inline auto token_of(const boost::json::object& t) -> std::string {
    const auto s = boost::json::serialize(t);
    return aws_json::base64_encode(
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(s.data()), s.size()));
}

[[nodiscard]] inline auto token_from(std::string_view text) -> std::optional<boost::json::object> {
    const auto bytes = aws_json::base64_decode(text);
    if (!bytes) {
        return std::nullopt;
    }
    boost::system::error_code ec;
    auto v = boost::json::parse(
        std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()), ec);
    if (ec || !v.is_object()) {
        return std::nullopt;
    }
    return v.as_object();
}

/// The `BatchStatementError` code for an error type.
[[nodiscard]] inline auto batch_code(std::string_view type) -> std::string {
    const auto name = type.substr(type.find('#') + 1);
    static const std::map<std::string, std::string, std::less<>> codes{
        {"ConditionalCheckFailedException", "ConditionalCheckFailed"},
        {"ItemCollectionSizeLimitExceededException", "ItemCollectionSizeLimitExceeded"},
        {"RequestLimitExceeded", "RequestLimitExceeded"},
        {"ValidationException", "ValidationError"},
        {"ProvisionedThroughputExceededException", "ProvisionedThroughputExceeded"},
        {"TransactionConflictException", "TransactionConflict"},
        {"ThrottlingException", "ThrottlingError"},
        {"ResourceNotFoundException", "ResourceNotFound"},
        {"AccessDeniedException", "AccessDenied"},
        {"DuplicateItemException", "DuplicateItem"},
    };
    const auto it = codes.find(name);
    return it == codes.end() ? "InternalServerError" : it->second;
}

}  // namespace partiql_detail

/// The three PartiQL operations over the JSON API's handlers.
template<dynamo_backend Backend> class partiql_api {
public:
    using result = aws_json::result<boost::json::object>;

    partiql_api(Backend& backend, item_api<Backend>& items, query_api<Backend>& queries,
                txn_api<Backend>& transactions)
        : _backend(backend), _items(items), _queries(queries), _transactions(transactions) {}

    template<typename FrontEnd> void register_with(FrontEnd& fe) {
        fe.on("ExecuteStatement",
              [this](call_context& c, boost::json::object& in) { return execute(c, in); });
        fe.on("BatchExecuteStatement",
              [this](call_context& c, boost::json::object& in) { return batch(c, in); });
        fe.on("ExecuteTransaction",
              [this](call_context& c, boost::json::object& in) { return transaction(c, in); });
    }

    auto execute(call_context& ctx, boost::json::object& in) -> result {
        auto s = parse(in);
        if (!s) {
            return s.error();
        }
        if (s->k == partiql_detail::statement::kind::exists) {
            return malformed(in);
        }
        if (auto e = authorize(ctx, *s)) {
            return *e;
        }
        auto quiet = unchecked(ctx);
        if (s->k == partiql_detail::statement::kind::select) {
            return select(quiet, in, *s);
        }
        auto req = write_request(ctx, *s, in);
        if (!req) {
            return req.error();
        }
        return run_write(quiet, *s, *req);
    }

    auto batch(call_context& ctx, boost::json::object& in) -> result {
        using partiql_detail::statement;
        const auto& list = in.at("Statements").as_array();
        // Parse every statement first: a malformed one fails the call.
        std::vector<std::optional<statement>> parsed;
        std::vector<std::optional<aws_json::error>> errors;
        bool reads = false;
        bool writes = false;
        for (const auto& v : list) {
            auto st = parse(v.as_object());
            if (!st) {
                if (st.error().message.value_or("").starts_with("Statement wasn't well formed")) {
                    return st.error();
                }
                parsed.emplace_back();
                errors.emplace_back(st.error());
                continue;
            }
            (st->k == statement::kind::select ? reads : writes) = true;
            parsed.emplace_back(std::move(*st));
            errors.emplace_back();
        }
        if (reads && writes) {
            return validation_error(
                "Read and write requests together in the same batch is not supported.");
        }
        auto quiet = unchecked(ctx);
        boost::json::array responses;
        for (std::size_t i = 0; i < list.size(); ++i) {
            const auto& entry = list[i].as_object();
            boost::json::object r;
            const auto fail = [&](const aws_json::error& e) {
                r["Error"] = boost::json::object{{"Code", partiql_detail::batch_code(e.type)},
                                                 {"Message", e.message.value_or("")}};
                responses.push_back(std::move(r));
            };
            if (errors[i]) {
                fail(*errors[i]);
                continue;
            }
            const auto& s = *parsed[i];
            if (auto e = authorize(ctx, s)) {
                fail(*e);
                continue;
            }
            const auto table = api_detail::usable_table(_backend, ctx, s.from.table);
            if (!table) {
                fail(table.error());
                continue;
            }
            r["TableName"] = s.from.table;
            if (s.k == statement::kind::select) {
                auto got = single_get(s, entry, *table);
                if (!got) {
                    r.erase("TableName");
                    fail(got.error());
                    continue;
                }
                auto out = _items.get_item(quiet, *got);
                if (!out) {
                    fail(out.error());
                    continue;
                }
                if (const auto* item = out->if_contains("Item")) {
                    r["Item"] = *item;
                }
                responses.push_back(std::move(r));
                continue;
            }
            auto req = write_request(ctx, s, entry);
            if (!req) {
                fail(req.error());
                continue;
            }
            auto out = run_write(quiet, s, *req);
            if (!out) {
                fail(out.error());
                continue;
            }
            responses.push_back(std::move(r));
        }
        // Every statement failed to parse: none named a table to authorize.
        if (std::all_of(errors.begin(), errors.end(),
                        [](const auto& e) { return e.has_value(); })) {
            ctx.touched_no_resource = true;
        }
        return boost::json::object{{"Responses", std::move(responses)}};
    }

    auto transaction(call_context& ctx, boost::json::object& in) -> result {
        using partiql_detail::statement;
        const auto& list = in.at("TransactStatements").as_array();
        std::vector<statement> parsed;
        bool reads = false;
        bool writes = false;
        for (const auto& entry : list) {
            auto st = parse(entry.as_object());
            if (!st) {
                return st.error();
            }
            (st->k == statement::kind::select ? reads : writes) = true;
            parsed.push_back(std::move(*st));
        }
        if (reads && writes) {
            return validation_error(
                "ExecuteTransaction API does not support both read and write operations in "
                "the same request.");
        }
        for (const auto& s : parsed) {
            if (auto e = authorize(ctx, s)) {
                return *e;
            }
        }
        boost::json::array items;
        for (std::size_t i = 0; i < parsed.size(); ++i) {
            auto element = transact_element(ctx, parsed[i], list[i].as_object());
            if (!element) {
                return validation_error("Validation failed in TransactStatements[" +
                                        std::to_string(i) +
                                        "]: " + element.error().message.value_or(""));
            }
            items.push_back(std::move(*element));
        }
        boost::json::object req{{"TransactItems", std::move(items)}};
        for (const auto* m : {"ClientRequestToken", "ReturnConsumedCapacity"}) {
            if (const auto* v = in.if_contains(m)) {
                req[m] = *v;
            }
        }
        auto quiet = unchecked(ctx);
        if (reads) {
            auto out = _transactions.transact_get(quiet, req);
            if (!out) {
                return out.error();
            }
            return out;
        }
        auto out = _transactions.transact_write(quiet, req);
        if (!out) {
            return inserts_as_duplicates(parsed, std::move(out.error()));
        }
        // DynamoDB Local answers a committed write transaction with an
        // empty Responses list.
        out->emplace("Responses", boost::json::array{});
        return out;
    }

private:
    using statement = partiql_detail::statement;

    /// An INSERT's failed `attribute_not_exists` check is a duplicate key:
    /// DynamoDB Local reports it as a ValidationError reason with
    /// ExecuteStatement's message, and names the codes again in the error's
    /// own message.
    static auto inserts_as_duplicates(const std::vector<statement>& parsed, aws_json::error e)
        -> aws_json::error {
        auto* reasons = e.extra.if_contains("CancellationReasons");
        if (reasons == nullptr || !reasons->is_array()) {
            return e;
        }
        std::string codes;
        auto& list = reasons->as_array();
        for (std::size_t i = 0; i < list.size(); ++i) {
            auto& reason = list[i].as_object();
            if (i < parsed.size() && parsed[i].k == statement::kind::insert &&
                reason.at("Code").as_string() == "ConditionalCheckFailed") {
                reason["Code"] = "ValidationError";
                reason["Message"] = "Duplicate primary key exists in table";
                reason.erase("Item");
            }
            codes += (i == 0 ? "" : ", ") + std::string(reason.at("Code").as_string());
        }
        e.message =
            "Transaction cancelled, please refer cancellation reasons for specific reasons [" +
            codes + "]";
        return e;
    }

    static auto malformed(const boost::json::object& in) -> aws_json::error {
        return validation_error("Statement wasn't well formed, can't be processed: " +
                                std::string(in.at("Statement").as_string()));
    }

    static auto parse(const boost::json::object& in) -> aws_json::result<statement> {
        const auto text = in.at("Statement").as_string();
        const boost::json::array* params = nullptr;
        if (const auto* p = in.if_contains("Parameters")) {
            params = &p->as_array();
        }
        try {
            partiql_detail::parser p(partiql_detail::tokenize(text), params);
            auto s = p.parse();
            if (!table_api_detail::valid_table_name(s.from.table)) {
                return validation_error(
                    "Table name should be within the length [3, 255] and only contain 'a-z', "
                    "'A-Z', '0-9', '-', '_', '.'.");
            }
            return s;
        } catch (const partiql_detail::malformed&) {
            return malformed(in);
        } catch (const partiql_detail::refused& r) {
            return validation_error(r.message);
        }
    }

    /// The statement's PartiQL action on its table (or index).
    auto authorize(call_context& ctx, const statement& s) -> std::optional<aws_json::error> {
        if (!ctx.authorize) {
            return std::nullopt;
        }
        aws_auth::authz_request r;
        switch (s.k) {
            case statement::kind::select:
            case statement::kind::exists:
                r.action = "dynamodb:PartiQLSelect";
                break;
            case statement::kind::insert:
                r.action = "dynamodb:PartiQLInsert";
                break;
            case statement::kind::update:
                r.action = "dynamodb:PartiQLUpdate";
                break;
            case statement::kind::delete_:
                r.action = "dynamodb:PartiQLDelete";
                break;
        }
        r.resource = ctx.table_arn(s.from.table);
        if (!s.from.index.empty()) {
            r.resource += "/index/" + s.from.index;
        }
        return ctx.authorize(std::move(r));
    }

    /// The context the JSON API handlers run under: authorized already.
    static auto unchecked(const call_context& ctx) -> call_context {
        call_context c = ctx;
        c.authorize = nullptr;
        return c;
    }

    static auto copy_members(const boost::json::object& from, boost::json::object& to,
                             std::initializer_list<const char*> members) -> void {
        for (const auto* m : members) {
            if (const auto* v = from.if_contains(m)) {
                to[m] = *v;
            }
        }
    }

    /// The key schema a SELECT reads: the table's, or its index's.
    auto read_schema(const statement& s, const table_descriptor& t)
        -> aws_json::result<key_schema> {
        if (s.from.index.empty()) {
            return t.schema;
        }
        if (const auto* l = query_detail::find_index(t, s.from.index)) {
            return index_schema(t.schema, *l);
        }
        if (const auto* g = query_detail::find_global_index(t, s.from.index)) {
            return index_schema(*g);
        }
        return validation_error("The table does not have the specified index: " + s.from.index);
    }

    // -- SELECT ---------------------------------------------------------------

    auto select(call_context& ctx, const boost::json::object& in, const statement& s) -> result {
        using namespace partiql_detail;
        const auto table = api_detail::usable_table(_backend, ctx, s.from.table);
        if (!table) {
            return table.error();
        }
        auto schema = read_schema(s, *table);
        if (!schema) {
            return schema.error();
        }
        // The key terms: the partition key's equality or IN list, and one
        // sort key condition.
        std::vector<const cond*> all;
        if (s.where) {
            conjuncts(*s.where, all);
        }
        std::vector<boost::json::object> pk_values;
        const cond* sk_cond = nullptr;
        std::vector<const cond*> rest;
        for (const auto* c : all) {
            if (pk_values.empty()) {
                if (auto t = key_comparison(*c, schema->partition_name); t && t->op == "=") {
                    pk_values.push_back(*t->value);
                    continue;
                }
                if (c->k == cond::kind::in && is_key_path(c->args[0], schema->partition_name) &&
                    std::all_of(c->args.begin() + 1, c->args.end(),
                                [](const operand& o) { return o.k == operand::kind::literal; })) {
                    for (std::size_t i = 1; i < c->args.size(); ++i) {
                        pk_values.push_back(c->args[i].literal);
                    }
                    continue;
                }
            }
            if (sk_cond == nullptr && schema->sort_name && sort_key_term(*c, *schema->sort_name)) {
                sk_cond = c;
                continue;
            }
            rest.push_back(c);
        }
        std::optional<boost::json::object> token;
        if (const auto* t = in.if_contains("NextToken")) {
            token = token_from(t->as_string());
            if (!token) {
                return validation_error("Invalid NextToken");
            }
        }
        std::optional<std::size_t> limit;
        if (const auto* l = in.if_contains("Limit")) {
            limit = static_cast<std::size_t>(l->to_number<std::int64_t>());
        }

        if (pk_values.empty()) {
            // A Scan: the whole condition filters.
            if (sk_cond != nullptr) {
                rest.insert(rest.begin(), sk_cond);
            }
            emitter e;
            boost::json::object req{{"TableName", s.from.table}};
            if (!s.from.index.empty()) {
                req["IndexName"] = s.from.index;
            }
            if (auto f = join(e, rest)) {
                req["FilterExpression"] = *f;
            }
            add_projection(e, s, req);
            e.attach(req);
            copy_members(in, req, {"ConsistentRead", "ReturnConsumedCapacity"});
            if (limit) {
                req["Limit"] = static_cast<std::int64_t>(*limit);
            }
            if (token && token->contains("k")) {
                req["ExclusiveStartKey"] = token->at("k");
            }
            auto out = _queries.scan(ctx, req);
            if (!out) {
                return out.error();
            }
            return page_out(s, *out, 0, false);
        }

        // One Query per partition key value, from where the token left off.
        std::size_t at = token && token->contains("i")
                             ? static_cast<std::size_t>(token->at("i").to_number<std::int64_t>())
                             : 0;
        boost::json::array items;
        boost::json::array consumed;
        std::optional<boost::json::value> start;
        if (token && token->contains("k")) {
            start = token->at("k");
        }
        for (; at < pk_values.size(); ++at) {
            emitter e;
            boost::json::object req{{"TableName", s.from.table}};
            if (!s.from.index.empty()) {
                req["IndexName"] = s.from.index;
            }
            std::string key = e.name(schema->partition_name) + " = " + e.value(pk_values[at]);
            if (sk_cond != nullptr) {
                key += " AND " + e.text(*sk_cond);
            }
            req["KeyConditionExpression"] = key;
            if (auto f = join(e, rest)) {
                req["FilterExpression"] = *f;
            }
            add_projection(e, s, req);
            e.attach(req);
            copy_members(in, req, {"ConsistentRead", "ReturnConsumedCapacity"});
            if (s.descending) {
                req["ScanIndexForward"] = false;
            }
            if (limit) {
                req["Limit"] = static_cast<std::int64_t>(*limit - std::min(*limit, items.size()));
            }
            if (start) {
                req["ExclusiveStartKey"] = *start;
                start.reset();
            }
            auto out = _queries.query(ctx, req);
            if (!out) {
                return out.error();
            }
            for (auto& it : out->at("Items").as_array()) {
                items.push_back(std::move(it));
            }
            if (const auto* c = out->if_contains("ConsumedCapacity")) {
                consumed.push_back(*c);
            }
            if (const auto* lek = out->if_contains("LastEvaluatedKey")) {
                boost::json::object page{{"Items", std::move(items)}};
                page["NextToken"] = token_of({{"i", static_cast<std::int64_t>(at)}, {"k", *lek}});
                return finish_page(s, std::move(page), consumed);
            }
            if (limit && items.size() >= *limit && at + 1 < pk_values.size()) {
                boost::json::object page{{"Items", std::move(items)}};
                page["NextToken"] = token_of({{"i", static_cast<std::int64_t>(at + 1)}});
                return finish_page(s, std::move(page), consumed);
            }
        }
        return finish_page(s, boost::json::object{{"Items", std::move(items)}}, consumed);
    }

    static auto sort_key_term(const partiql_detail::cond& c, const std::string& sk) -> bool {
        using namespace partiql_detail;
        if (key_comparison(c, sk)) {
            return true;
        }
        if (c.k == cond::kind::between && is_key_path(c.args[0], sk) &&
            c.args[1].k == operand::kind::literal && c.args[2].k == operand::kind::literal) {
            return true;
        }
        return c.k == cond::kind::call && c.op == "begins_with" && c.args.size() == 2 &&
               is_key_path(c.args[0], sk) && c.args[1].k == operand::kind::literal;
    }

    static auto add_projection(partiql_detail::emitter& e, const statement& s,
                               boost::json::object& req) -> void {
        if (s.projection.empty()) {
            return;
        }
        // Only each path's top-level attribute is projected here: a
        // projected list keeps just the chosen elements, renumbered, so
        // `l[1]` would no longer be at index 1 when finish_page() reads it.
        std::string list;
        std::vector<std::string> seen;
        for (const auto& p : s.projection) {
            const auto& name = p.segs.front().name;
            if (std::find(seen.begin(), seen.end(), name) != seen.end()) {
                continue;
            }
            seen.push_back(name);
            partiql_detail::path top;
            top.segs.push_back(p.segs.front());
            list += (list.empty() ? "" : ", ") + e.text(top);
        }
        req["ProjectionExpression"] = list;
    }

    /// A Scan's page as ExecuteStatement answers it.
    static auto page_out(const statement& s, boost::json::object& out, std::size_t /*at*/,
                         bool /*unused*/) -> boost::json::object {
        boost::json::object page{{"Items", std::move(out.at("Items"))}};
        if (const auto* lek = out.if_contains("LastEvaluatedKey")) {
            page["NextToken"] = partiql_detail::token_of({{"k", *lek}});
        }
        boost::json::array consumed;
        if (const auto* c = out.if_contains("ConsumedCapacity")) {
            consumed.push_back(*c);
        }
        return finish_page(s, std::move(page), consumed);
    }

    /// Renames projected nested paths as DynamoDB Local does, and adds the
    /// capacity (summed per table) when asked for.
    static auto finish_page(const statement& s, boost::json::object page,
                            const boost::json::array& consumed) -> boost::json::object {
        if (!s.projection.empty()) {
            boost::json::array renamed;
            for (const auto& v : page.at("Items").as_array()) {
                boost::json::object item;
                for (const auto& p : s.projection) {
                    if (const auto* x = partiql_detail::value_at(v.as_object(), p)) {
                        item[partiql_detail::projected_name(p)] = *x;
                    }
                }
                renamed.push_back(std::move(item));
            }
            page["Items"] = std::move(renamed);
        }
        if (!consumed.empty()) {
            boost::json::object total = consumed.front().as_object();
            double units = 0;
            for (const auto& c : consumed) {
                units += c.at("CapacityUnits").to_number<double>();
            }
            total["CapacityUnits"] = units;
            page["ConsumedCapacity"] = std::move(total);
        }
        return page;
    }

    /// A batch SELECT: one item by its full key, nothing else.
    auto single_get(const statement& s, const boost::json::object& entry, const table_descriptor& t)
        -> aws_json::result<boost::json::object> {
        const auto k = partiql_detail::full_key(s.where, t.schema);
        if (!s.from.index.empty() || !k || !k->rest.empty()) {
            return validation_error("Only single item select is supported");
        }
        boost::json::object req{{"TableName", s.from.table}, {"Key", k->key}};
        partiql_detail::emitter e;
        add_projection(e, s, req);
        e.attach(req);
        copy_members(entry, req, {"ConsistentRead"});
        return req;
    }

    // -- Writes ---------------------------------------------------------------

    /// The PutItem, UpdateItem or DeleteItem request a write stands for.
    auto write_request(const call_context& ctx, const statement& s, const boost::json::object& in)
        -> aws_json::result<boost::json::object> {
        using namespace partiql_detail;
        const auto table = api_detail::usable_table(_backend, ctx, s.from.table);
        if (!table) {
            return table.error();
        }
        const auto& schema = table->schema;
        boost::json::object req{{"TableName", s.from.table}};
        copy_members(in, req, {"ReturnConsumedCapacity", "ReturnValuesOnConditionCheckFailure"});
        emitter e;
        if (s.k == statement::kind::insert) {
            req["Item"] = s.item;
            req["ConditionExpression"] =
                "attribute_not_exists(" + e.name(schema.partition_name) + ")";
            e.attach(req);
            return req;
        }
        const auto k = full_key(s.where, schema);
        if (!k) {
            return validation_error(no_full_key().message);
        }
        req["Key"] = k->key;
        auto condition = join(e, k->rest);
        if (s.k == statement::kind::update) {
            // An UPDATE changes an existing item only.
            const auto exists = "attribute_exists(" + e.name(schema.partition_name) + ")";
            condition = condition ? exists + " AND " + *condition : exists;
            auto update = update_text(e, s);
            if (!update) {
                return update.error();
            }
            req["UpdateExpression"] = *update;
        }
        if (condition) {
            req["ConditionExpression"] = *condition;
        }
        if (!s.returning.empty()) {
            req["ReturnValues"] = s.returning;
        }
        e.attach(req);
        return req;
    }

    static auto update_text(partiql_detail::emitter& e, const statement& s)
        -> aws_json::result<std::string> {
        using partiql_detail::operand;
        std::string set;
        std::string remove;
        std::string add;
        std::string del;
        const auto append = [](std::string& to, const std::string& part) {
            to += (to.empty() ? "" : ", ") + part;
        };
        for (const auto& a : s.assignments) {
            const auto target = e.text(a.target);
            if (a.remove) {
                append(remove, target);
                continue;
            }
            const auto& v = a.value;
            if (v.k == operand::kind::call && (v.fn == "set_add" || v.fn == "set_delete")) {
                if (v.args.size() != 2) {
                    return validation_error(std::string("Invalid number of arguments for ") + v.fn);
                }
                append(v.fn == "set_add" ? add : del, target + " " + e.text(v.args[1]));
                continue;
            }
            append(set, target + " = " + e.text(v));
        }
        std::string out;
        if (!set.empty()) {
            out += "SET " + set;
        }
        if (!remove.empty()) {
            out += (out.empty() ? "" : " ") + std::string("REMOVE ") + remove;
        }
        if (!add.empty()) {
            out += (out.empty() ? "" : " ") + std::string("ADD ") + add;
        }
        if (!del.empty()) {
            out += (out.empty() ? "" : " ") + std::string("DELETE ") + del;
        }
        return out;
    }

    auto run_write(call_context& ctx, const statement& s, boost::json::object& req) -> result {
        result out = boost::json::object{};
        switch (s.k) {
            case statement::kind::insert:
                out = _items.put_item(ctx, req);
                if (!out && out.error().type.ends_with("ConditionalCheckFailedException")) {
                    return table_api_detail::modeled("DuplicateItemException",
                                                     "Duplicate primary key exists in table");
                }
                break;
            case statement::kind::update:
                out = _items.update_item(ctx, req);
                break;
            case statement::kind::delete_:
                out = _items.delete_item(ctx, req);
                break;
            default:
                return validation_error("Unsupported operation");
        }
        if (!out) {
            return out.error();
        }
        boost::json::array items;
        if (const auto* a = out->if_contains("Attributes")) {
            items.push_back(*a);
        }
        boost::json::object answer{{"Items", std::move(items)}};
        if (const auto* c = out->if_contains("ConsumedCapacity")) {
            answer["ConsumedCapacity"] = *c;
        }
        return answer;
    }

    /// One element of a TransactGetItems or TransactWriteItems request.
    auto transact_element(const call_context& ctx, const statement& s,
                          const boost::json::object& entry)
        -> aws_json::result<boost::json::object> {
        using namespace partiql_detail;
        const auto table = api_detail::usable_table(_backend, ctx, s.from.table);
        if (!table) {
            return table.error();
        }
        switch (s.k) {
            case statement::kind::select: {
                auto get = single_get(s, entry, *table);
                if (!get) {
                    return get.error();
                }
                get->erase("ConsistentRead");
                return boost::json::object{{"Get", std::move(*get)}};
            }
            case statement::kind::exists: {
                const auto k = full_key(s.where, table->schema);
                if (!k || k->rest.empty()) {
                    return validation_error(
                        "EXISTS() must contain a single item read with additional condition");
                }
                emitter e;
                boost::json::object check{{"TableName", s.from.table}, {"Key", k->key}};
                check["ConditionExpression"] = *join(e, k->rest);
                e.attach(check);
                copy_members(entry, check, {"ReturnValuesOnConditionCheckFailure"});
                return boost::json::object{{"ConditionCheck", std::move(check)}};
            }
            case statement::kind::insert:
            case statement::kind::update:
            case statement::kind::delete_: {
                auto req = write_request(ctx, s, entry);
                if (!req) {
                    return req.error();
                }
                req->erase("ReturnConsumedCapacity");
                req->erase("ReturnValues");
                const char* kind = s.k == statement::kind::insert   ? "Put"
                                   : s.k == statement::kind::update ? "Update"
                                                                    : "Delete";
                return boost::json::object{{kind, std::move(*req)}};
            }
        }
        return validation_error("Unsupported operation");
    }

    Backend& _backend;
    item_api<Backend>& _items;
    query_api<Backend>& _queries;
    txn_api<Backend>& _transactions;
};

}  // namespace kythira::dynamodb
