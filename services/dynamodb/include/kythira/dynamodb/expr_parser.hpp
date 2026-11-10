// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file expr_parser.hpp
/// @brief The expression language: lexer, parser and the checks DynamoDB
///        makes before it reads an item (`.kiro/specs/dynamodb-compatible-api/`
///        task 5, Requirements 8.3 and 21.3).
///
/// `expression_compiler` turns condition, filter, key condition, update and
/// projection expressions into the trees of `expr.hpp`, substituting the
/// request's `ExpressionAttributeNames` and `ExpressionAttributeValues`.
/// Every rejection is DynamoDB Local 3.3.2's, message for message: the
/// recorded corpus in `tests/data/expression_corpus.jsonl` (tools/expr-corpus)
/// pins them and `dynamodb_expression_corpus_test` replays it.
///
/// DynamoDB checks an expression in passes, and which error a request with
/// several faults gets depends on the order, so the compiler keeps it:
///
///   1. empty text, then the 4 KB size limit;
///   2. syntax: the first token no valid expression can continue with,
///      reported with the text from the token before it to the token after;
///   3. redundant parentheses;
///   4. function names, in text order;
///   5. reserved words and list-index range, in text order, then an update
///      clause used twice;
///   6. path nesting (32 levels), `IN` operands (100), operators (300);
///   7. one walk in text order, children before their operator: placeholder
///      substitution, operand counts and types, document-path operands,
///      distinct operands, `BETWEEN` bounds. Update and projection
///      expressions instead walk their actions (paths) last to first: first
///      each target path's names and its overlap with the paths after it,
///      then each action's operands;
///   8. per kind: the key-schema rules of key conditions and Query filters.
///
/// Unused placeholders are checked once every expression of a request has
/// compiled (`finish`), names before values.

#include <kythira/dynamodb/errors.hpp>
#include <kythira/dynamodb/expr.hpp>
#include <kythira/dynamodb/expr_reserved.hpp>
#include <kythira/dynamodb/item_codec.hpp>

#include <raft/aws_json_protocol.hpp>

#include <boost/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kythira::dynamodb {

/// Which parameter an expression came in, as messages name it.
enum class expr_kind : std::uint8_t {
    condition,
    update,
    filter,
    key_condition,
    projection,
};

[[nodiscard]] constexpr auto expr_parameter(expr_kind k) noexcept -> std::string_view {
    switch (k) {
        case expr_kind::condition:
            return "ConditionExpression";
        case expr_kind::update:
            return "UpdateExpression";
        case expr_kind::filter:
            return "FilterExpression";
        case expr_kind::key_condition:
            return "KeyConditionExpression";
        case expr_kind::projection:
            return "ProjectionExpression";
    }
    return "";
}

inline constexpr std::size_t k_max_expression_bytes = 4096;
inline constexpr std::size_t k_max_in_operands = 100;
inline constexpr std::size_t k_max_operators = 300;
inline constexpr std::size_t k_max_path_depth = 32;
inline constexpr std::int64_t k_max_list_index = 2147483647;
inline constexpr std::size_t k_max_placeholder_bytes = 255;
inline constexpr std::size_t k_max_attribute_name_bytes = 65535;

namespace expr_detail {

// ---------------------------------------------------------------------------
// Lexer
// ---------------------------------------------------------------------------

enum class tok : std::uint8_t {
    ident,
    name_ph,
    value_ph,
    number,
    kw_and,
    kw_or,
    kw_not,
    kw_between,
    kw_in,
    kw_set,
    kw_remove,
    kw_add,
    kw_delete,
    eq,
    ne,
    lt,
    le,
    gt,
    ge,
    lparen,
    rparen,
    lbracket,
    rbracket,
    comma,
    dot,
    plus,
    minus,
    unknown,
    eof,
};

struct token {
    tok kind = tok::eof;
    std::size_t begin = 0;
    std::size_t end = 0;
};

[[nodiscard]] inline auto is_alpha(char c) -> bool {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
[[nodiscard]] inline auto is_digit(char c) -> bool {
    return c >= '0' && c <= '9';
}
[[nodiscard]] inline auto is_word(char c) -> bool {
    return is_alpha(c) || is_digit(c) || c == '_';
}

[[nodiscard]] inline auto iequals(std::string_view a, std::string_view b) -> bool {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](unsigned char x, unsigned char y) {
               return std::tolower(x) == std::tolower(y);
           });
}

// Length of the UTF-8 sequence starting with `lead` (1 for a stray byte).
[[nodiscard]] inline auto utf8_length(unsigned char lead) -> std::size_t {
    if (lead >= 0xF0U && lead < 0xF8U) {
        return 4;
    }
    if (lead >= 0xE0U) {
        return lead < 0xF0U ? 3 : 1;
    }
    if (lead >= 0xC0U) {
        return 2;
    }
    return 1;
}

// UTF-16 code units in UTF-8 text: one per sequence, two for a four-byte
// one.
[[nodiscard]] inline auto utf16_length(std::string_view s) -> std::size_t {
    std::size_t n = 0;
    for (const char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        if ((c & 0xC0U) != 0x80U) {
            n += c >= 0xF0U ? 2 : 1;
        }
    }
    return n;
}

// `remove` is a keyword only in update and projection expressions; in
// conditions it is a name (and then a reserved word).
[[nodiscard]] inline auto lex(std::string_view s, bool update_mode) -> std::vector<token> {
    std::vector<token> out;
    std::size_t i = 0;
    while (i < s.size()) {
        const char c = s[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            ++i;
            continue;
        }
        token t;
        t.begin = i;
        if (is_alpha(c)) {
            std::size_t j = i + 1;
            while (j < s.size() && is_word(s[j])) {
                ++j;
            }
            const auto word = s.substr(i, j - i);
            t.kind = tok::ident;
            if (iequals(word, "and")) {
                t.kind = tok::kw_and;
            } else if (iequals(word, "or")) {
                t.kind = tok::kw_or;
            } else if (iequals(word, "not")) {
                t.kind = tok::kw_not;
            } else if (iequals(word, "between")) {
                t.kind = tok::kw_between;
            } else if (iequals(word, "in")) {
                t.kind = tok::kw_in;
            } else if (iequals(word, "set")) {
                t.kind = tok::kw_set;
            } else if (iequals(word, "add")) {
                t.kind = tok::kw_add;
            } else if (iequals(word, "delete")) {
                t.kind = tok::kw_delete;
            } else if (update_mode && iequals(word, "remove")) {
                t.kind = tok::kw_remove;
            }
            i = j;
        } else if (is_digit(c)) {
            std::size_t j = i + 1;
            if (c != '0') {
                while (j < s.size() && is_digit(s[j])) {
                    ++j;
                }
            }
            t.kind = tok::number;
            i = j;
        } else if ((c == '#' || c == ':') && i + 1 < s.size() && is_word(s[i + 1])) {
            std::size_t j = i + 1;
            while (j < s.size() && is_word(s[j])) {
                ++j;
            }
            t.kind = c == '#' ? tok::name_ph : tok::value_ph;
            i = j;
        } else {
            std::size_t len = 1;
            switch (c) {
                case '=':
                    t.kind = tok::eq;
                    break;
                case '<':
                    if (i + 1 < s.size() && s[i + 1] == '>') {
                        t.kind = tok::ne;
                        len = 2;
                    } else if (i + 1 < s.size() && s[i + 1] == '=') {
                        t.kind = tok::le;
                        len = 2;
                    } else {
                        t.kind = tok::lt;
                    }
                    break;
                case '>':
                    if (i + 1 < s.size() && s[i + 1] == '=') {
                        t.kind = tok::ge;
                        len = 2;
                    } else {
                        t.kind = tok::gt;
                    }
                    break;
                case '(':
                    t.kind = tok::lparen;
                    break;
                case ')':
                    t.kind = tok::rparen;
                    break;
                case '[':
                    t.kind = tok::lbracket;
                    break;
                case ']':
                    t.kind = tok::rbracket;
                    break;
                case ',':
                    t.kind = tok::comma;
                    break;
                case '.':
                    t.kind = tok::dot;
                    break;
                case '+':
                    t.kind = tok::plus;
                    break;
                case '-':
                    t.kind = tok::minus;
                    break;
                default:
                    t.kind = tok::unknown;
                    len = std::min(utf8_length(static_cast<unsigned char>(c)), s.size() - i);
                    break;
            }
            i += len;
        }
        t.end = i;
        out.push_back(t);
    }
    token eof;
    eof.kind = tok::eof;
    eof.begin = s.size();
    eof.end = s.size();
    out.push_back(eof);
    return out;
}

// ---------------------------------------------------------------------------
// Parse tree
// ---------------------------------------------------------------------------

struct raw_step {
    bool placeholder = false;  ///< `#name`
    bool is_index = false;
    std::string text;  ///< the name, placeholder or index digits as written
};

enum class raw_kind : std::uint8_t {
    path,
    value,
    func,
    compare,
    between,
    in,
    and_,
    or_,
    not_,
    paren,
    plus,
    minus,
};

// What a node can stand for syntactically: a function call can be either
// until the checks decide.
enum class syn : std::uint8_t {
    operand,
    boolean,
    either,
};

struct raw_node {
    raw_kind kind = raw_kind::path;
    syn cls = syn::operand;
    std::string text;  ///< function name, value placeholder or operator
    expr_op op = expr_op::eq;
    std::vector<raw_step> path;
    std::vector<raw_node> args;
};

struct raw_update_action {
    update_kind kind = update_kind::set;
    std::vector<raw_step> path;
    raw_node value;
};

struct raw_update {
    std::vector<raw_update_action> actions;
    /// Clause keywords in order, for the used-once rule.
    std::vector<update_kind> clauses;
};

struct syntax_failure {
    std::size_t token = 0;
};

class parser {
public:
    parser(std::string_view text, bool update_mode) : _text(text), _toks(lex(text, update_mode)) {}

    /// "Syntax error; token: "x", near: "a x b"".
    [[nodiscard]] auto syntax_message(std::size_t at) const -> std::string {
        const auto& t = _toks[at];
        const std::string token =
            t.kind == tok::eof ? "<EOF>" : std::string(_text.substr(t.begin, t.end - t.begin));
        const std::size_t from = at == 0 ? 0 : _toks[at - 1].begin;
        const std::size_t to =
            t.kind == tok::eof || at + 1 >= _toks.size() ? _text.size() : _toks[at + 1].end;
        return "Syntax error; token: \"" + token + "\", near: \"" +
               std::string(_text.substr(from, to - from)) + "\"";
    }

    [[nodiscard]] auto condition() -> raw_node {
        auto n = parse_or();
        require_bool(n);
        expect(tok::eof);
        return n;
    }

    [[nodiscard]] auto update() -> raw_update {
        raw_update u;
        if (!is_clause(peek())) {
            fail();
        }
        while (is_clause(peek())) {
            const auto kw = peek();
            advance();
            const auto kind = kw == tok::kw_set      ? update_kind::set
                              : kw == tok::kw_remove ? update_kind::remove
                              : kw == tok::kw_add    ? update_kind::add
                                                     : update_kind::delete_;
            u.clauses.push_back(kind);
            for (;;) {
                raw_update_action a;
                a.kind = kind;
                a.path = parse_path();
                if (kind == update_kind::set) {
                    expect(tok::eq);
                    a.value = parse_set_value();
                } else if (kind != update_kind::remove) {
                    if (peek() != tok::value_ph) {
                        fail();
                    }
                    a.value.kind = raw_kind::value;
                    a.value.text = current_text();
                    advance();
                }
                u.actions.push_back(std::move(a));
                if (peek() != tok::comma) {
                    break;
                }
                advance();
            }
        }
        expect(tok::eof);
        return u;
    }

    [[nodiscard]] auto projection() -> std::vector<std::vector<raw_step>> {
        std::vector<std::vector<raw_step>> out;
        out.push_back(parse_path());
        while (peek() == tok::comma) {
            advance();
            out.push_back(parse_path());
        }
        expect(tok::eof);
        return out;
    }

private:
    std::string_view _text;
    std::vector<token> _toks;
    std::size_t _pos = 0;

    [[nodiscard]] auto peek() const -> tok { return _toks[_pos].kind; }
    [[nodiscard]] auto peek2() const -> tok {
        return _pos + 1 < _toks.size() ? _toks[_pos + 1].kind : tok::eof;
    }
    void advance() {
        if (_toks[_pos].kind != tok::eof) {
            ++_pos;
        }
    }
    [[nodiscard]] auto current_text() const -> std::string {
        const auto& t = _toks[_pos];
        return std::string(_text.substr(t.begin, t.end - t.begin));
    }
    [[noreturn]] void fail() const { throw syntax_failure{_pos}; }
    void expect(tok k) {
        if (peek() != k) {
            fail();
        }
        advance();
    }
    [[nodiscard]] static auto is_clause(tok k) -> bool {
        return k == tok::kw_set || k == tok::kw_remove || k == tok::kw_add || k == tok::kw_delete;
    }
    void require_bool(const raw_node& n) const {
        if (n.cls == syn::operand) {
            fail();
        }
    }
    void require_operand(const raw_node& n) const {
        if (n.cls == syn::boolean) {
            fail();
        }
    }

    [[nodiscard]] auto parse_or() -> raw_node {
        auto left = parse_and();
        while (peek() == tok::kw_or) {
            require_bool(left);
            advance();
            auto right = parse_and();
            require_bool(right);
            raw_node n;
            n.kind = raw_kind::or_;
            n.cls = syn::boolean;
            n.args.push_back(std::move(left));
            n.args.push_back(std::move(right));
            left = std::move(n);
        }
        return left;
    }

    [[nodiscard]] auto parse_and() -> raw_node {
        auto left = parse_not();
        while (peek() == tok::kw_and) {
            require_bool(left);
            advance();
            auto right = parse_not();
            require_bool(right);
            raw_node n;
            n.kind = raw_kind::and_;
            n.cls = syn::boolean;
            n.args.push_back(std::move(left));
            n.args.push_back(std::move(right));
            left = std::move(n);
        }
        return left;
    }

    [[nodiscard]] auto parse_not() -> raw_node {
        if (peek() == tok::kw_not) {
            advance();
            auto inner = parse_not();
            require_bool(inner);
            raw_node n;
            n.kind = raw_kind::not_;
            n.cls = syn::boolean;
            n.args.push_back(std::move(inner));
            return n;
        }
        return parse_primary();
    }

    [[nodiscard]] static auto comparator(tok k) -> std::optional<expr_op> {
        switch (k) {
            case tok::eq:
                return expr_op::eq;
            case tok::ne:
                return expr_op::ne;
            case tok::lt:
                return expr_op::lt;
            case tok::le:
                return expr_op::le;
            case tok::gt:
                return expr_op::gt;
            case tok::ge:
                return expr_op::ge;
            default:
                return std::nullopt;
        }
    }

    [[nodiscard]] auto parse_primary() -> raw_node {
        auto left = parse_condition_atom();
        if (const auto op = comparator(peek())) {
            require_operand(left);
            raw_node n;
            n.kind = raw_kind::compare;
            n.cls = syn::boolean;
            n.op = *op;
            n.text = current_text();
            advance();
            n.args.push_back(std::move(left));
            n.args.push_back(parse_operand());
            return n;
        }
        if (peek() == tok::kw_between) {
            require_operand(left);
            advance();
            raw_node n;
            n.kind = raw_kind::between;
            n.cls = syn::boolean;
            n.args.push_back(std::move(left));
            n.args.push_back(parse_operand());
            expect(tok::kw_and);
            n.args.push_back(parse_operand());
            return n;
        }
        if (peek() == tok::kw_in) {
            require_operand(left);
            advance();
            expect(tok::lparen);
            raw_node n;
            n.kind = raw_kind::in;
            n.cls = syn::boolean;
            n.args.push_back(std::move(left));
            n.args.push_back(parse_operand());
            while (peek() == tok::comma) {
                advance();
                n.args.push_back(parse_operand());
            }
            expect(tok::rparen);
            return n;
        }
        return left;
    }

    // In condition position '(' opens either a condition or an operand;
    // what follows decides, so parse the union and let the callers check.
    [[nodiscard]] auto parse_condition_atom() -> raw_node {
        if (peek() == tok::lparen) {
            advance();
            auto inner = parse_or();
            expect(tok::rparen);
            raw_node n;
            n.kind = raw_kind::paren;
            n.cls = inner.cls;
            n.args.push_back(std::move(inner));
            return n;
        }
        return parse_term();
    }

    [[nodiscard]] auto parse_operand() -> raw_node {
        if (peek() == tok::lparen) {
            advance();
            auto inner = parse_operand();
            expect(tok::rparen);
            raw_node n;
            n.kind = raw_kind::paren;
            n.cls = syn::operand;
            n.args.push_back(std::move(inner));
            return n;
        }
        auto n = parse_term();
        if (n.cls == syn::either) {
            n.cls = syn::operand;
        }
        return n;
    }

    // SET's right-hand side: an operand, or two joined by + or -, where
    // parentheses may hold a whole right-hand side.
    // A SET value is an operand, or two joined by `+` or `-`, or a whole
    // value in parentheses. Parenthesised arithmetic cannot be continued,
    // and the operand after `+` or `-` cannot itself hold arithmetic.
    [[nodiscard]] auto parse_set_value() -> raw_node {
        raw_node left;
        if (peek() == tok::lparen) {
            advance();
            auto inner = parse_set_value();
            expect(tok::rparen);
            const bool arithmetic = inner.kind == raw_kind::plus || inner.kind == raw_kind::minus;
            left.kind = raw_kind::paren;
            left.args.push_back(std::move(inner));
            if (arithmetic) {
                return left;
            }
        } else {
            left = parse_term();
            left.cls = syn::operand;
        }
        if (peek() == tok::plus || peek() == tok::minus) {
            raw_node n;
            n.kind = peek() == tok::plus ? raw_kind::plus : raw_kind::minus;
            n.text = current_text();
            advance();
            n.args.push_back(std::move(left));
            n.args.push_back(parse_set_operand());
            return n;
        }
        return left;
    }

    [[nodiscard]] auto parse_set_operand() -> raw_node {
        if (peek() == tok::lparen) {
            advance();
            auto inner = parse_set_operand();
            expect(tok::rparen);
            raw_node n;
            n.kind = raw_kind::paren;
            n.args.push_back(std::move(inner));
            return n;
        }
        auto n = parse_term();
        n.cls = syn::operand;
        return n;
    }

    [[nodiscard]] auto parse_term() -> raw_node {
        if (peek() == tok::ident && peek2() == tok::lparen) {
            raw_node n;
            n.kind = raw_kind::func;
            n.cls = syn::either;
            n.text = current_text();
            advance();
            advance();
            n.args.push_back(parse_operand());
            while (peek() == tok::comma) {
                advance();
                n.args.push_back(parse_operand());
            }
            expect(tok::rparen);
            return n;
        }
        if (peek() == tok::value_ph) {
            raw_node n;
            n.kind = raw_kind::value;
            n.text = current_text();
            advance();
            return n;
        }
        raw_node n;
        n.kind = raw_kind::path;
        n.path = parse_path();
        return n;
    }

    [[nodiscard]] auto parse_path() -> std::vector<raw_step> {
        std::vector<raw_step> out;
        out.push_back(parse_name());
        for (;;) {
            if (peek() == tok::dot) {
                advance();
                // After a dot DynamoDB also reads `:word` as a name
                // placeholder (one ExpressionAttributeNames can never hold).
                if (peek() == tok::value_ph) {
                    raw_step s;
                    s.placeholder = true;
                    s.text = current_text();
                    advance();
                    out.push_back(std::move(s));
                    continue;
                }
                out.push_back(parse_name());
            } else if (peek() == tok::lbracket) {
                advance();
                if (peek() != tok::number) {
                    fail();
                }
                raw_step s;
                s.is_index = true;
                s.text = current_text();
                advance();
                expect(tok::rbracket);
                out.push_back(std::move(s));
            } else {
                return out;
            }
        }
    }

    [[nodiscard]] auto parse_name() -> raw_step {
        if (peek() != tok::ident && peek() != tok::name_ph) {
            fail();
        }
        raw_step s;
        s.placeholder = peek() == tok::name_ph;
        s.text = current_text();
        advance();
        return s;
    }
};

// ---------------------------------------------------------------------------
// Checks
// ---------------------------------------------------------------------------

struct compile_failure {
    std::string message;  ///< without the "Invalid <Parameter>: " prefix
    bool bare = false;    ///< message stands alone, without the prefix
};

[[noreturn]] inline void reject(std::string message) {
    throw compile_failure{std::move(message), false};
}
[[noreturn]] inline void reject_bare(std::string message) {
    throw compile_failure{std::move(message), true};
}

[[nodiscard]] inline auto is_condition_function(std::string_view f) -> bool {
    return f == "attribute_exists" || f == "attribute_not_exists" || f == "attribute_type" ||
           f == "begins_with" || f == "contains" || f == "size";
}
[[nodiscard]] inline auto is_update_function(std::string_view f) -> bool {
    return f == "if_not_exists" || f == "list_append";
}

inline void check_redundant_parens(const raw_node& n) {
    if (n.kind == raw_kind::paren && n.args.front().kind == raw_kind::paren) {
        reject("The expression has redundant parentheses;");
    }
    for (const auto& a : n.args) {
        check_redundant_parens(a);
    }
}

inline void check_function_names(const raw_node& n, bool update) {
    if (n.kind == raw_kind::func) {
        if (update) {
            if (is_condition_function(n.text)) {
                reject("The function is not allowed in an update expression; function: " + n.text);
            }
            if (!is_update_function(n.text)) {
                reject("Invalid function name; function: " + n.text);
            }
        } else if (!is_condition_function(n.text)) {
            if (is_update_function(n.text)) {
                reject("The function is not allowed in a condition expression; function: " +
                       n.text);
            }
            reject("Invalid function name; function: " + n.text);
        }
    }
    for (const auto& a : n.args) {
        check_function_names(a, update);
    }
}

inline void check_path_words(const std::vector<raw_step>& path) {
    for (const auto& s : path) {
        if (s.is_index) {
            // Digits only, so compare by length then text.
            const std::string max = std::to_string(k_max_list_index);
            if (s.text.size() > max.size() || (s.text.size() == max.size() && s.text > max)) {
                reject("List index is not within the allowable range; index: [" + s.text + "]");
            }
        } else if (!s.placeholder && is_reserved_word(s.text)) {
            reject("Attribute name is a reserved keyword; reserved keyword: " + s.text);
        }
    }
}

inline void check_words(const raw_node& n) {
    if (n.kind == raw_kind::path) {
        check_path_words(n.path);
    }
    for (const auto& a : n.args) {
        check_words(a);
    }
}

inline void check_depth(const std::vector<raw_step>& path) {
    if (path.size() > k_max_path_depth) {
        reject("The document path has too many nesting levels; nesting levels: " +
               std::to_string(path.size()));
    }
}

// Path depth, IN operand count and operator count, in text order. The
// operator count stops at the first operator over the limit, as DynamoDB's
// does, so its message always says 301.
inline void check_limits(const raw_node& n, std::size_t& operators) {
    const auto count = [&] {
        if (++operators > k_max_operators) {
            reject("The expression contains too many operators; operator count: " +
                   std::to_string(operators));
        }
    };
    switch (n.kind) {
        case raw_kind::path:
            check_depth(n.path);
            return;
        case raw_kind::value:
            return;
        case raw_kind::in:
            if (n.args.size() - 1 > k_max_in_operands) {
                reject("The IN operator is provided with too many operands; number of operands: " +
                       std::to_string(n.args.size() - 1));
            }
            count();
            break;
        case raw_kind::compare:
        case raw_kind::between:
        case raw_kind::and_:
        case raw_kind::or_:
        case raw_kind::not_:
        case raw_kind::func:
            count();
            break;
        case raw_kind::paren:
        case raw_kind::plus:
        case raw_kind::minus:
            break;
    }
    for (const auto& a : n.args) {
        check_limits(a, operators);
    }
}

[[nodiscard]] inline auto strip(const raw_node& n) -> const raw_node& {
    const raw_node* p = &n;
    while (p->kind == raw_kind::paren) {
        p = &p->args.front();
    }
    return *p;
}

// The type a compiled operand is known to have before any item is read.
[[nodiscard]] inline auto known_type(const expr_node& n) -> std::optional<attribute_type> {
    switch (n.op) {
        case expr_op::value:
            return n.value.type;
        case expr_op::size:
        case expr_op::plus:
        case expr_op::minus:
            return attribute_type::N;
        case expr_op::list_append:
            return attribute_type::L;
        default:
            // if_not_exists included: its type is only known once the item
            // is read.
            return std::nullopt;
    }
}

[[nodiscard]] inline auto update_type_name(attribute_type t) -> std::string_view {
    switch (t) {
        case attribute_type::S:
            return "STRING";
        case attribute_type::N:
            return "NUMBER";
        case attribute_type::B:
            return "BINARY";
        case attribute_type::BOOL:
            return "BOOLEAN";
        case attribute_type::NULL_:
            return "NULL";
        case attribute_type::M:
            return "MAP";
        case attribute_type::L:
            return "LIST";
        case attribute_type::SS:
            return "STRING_SET";
        case attribute_type::NS:
            return "NUMBER_SET";
        case attribute_type::BS:
            return "BINARY_SET";
    }
    return "";
}

[[nodiscard]] inline auto is_scalar_ordered(attribute_type t) -> bool {
    return t == attribute_type::S || t == attribute_type::N || t == attribute_type::B;
}

[[nodiscard]] inline auto is_set(attribute_type t) -> bool {
    return t == attribute_type::SS || t == attribute_type::NS || t == attribute_type::BS;
}

[[nodiscard]] inline auto incorrect_type(std::string_view what, attribute_type t) -> std::string {
    return "Incorrect operand type for operator or function; operator or function: " +
           std::string(what) + ", operand type: " + std::string(type_name(t));
}

// Byte-wise (S, B) or numeric (N) order of two values of one scalar type.
[[nodiscard]] inline auto compare_scalars(const attribute_value& a, const attribute_value& b)
    -> int {
    if (a.type == attribute_type::N) {
        const auto c = a.num <=> b.num;
        return c < 0 ? -1 : (c > 0 ? 1 : 0);
    }
    // Bytes compare unsigned.
    const auto n = std::min(a.str.size(), b.str.size());
    for (std::size_t i = 0; i < n; ++i) {
        const auto x = static_cast<unsigned char>(a.str[i]);
        const auto y = static_cast<unsigned char>(b.str[i]);
        if (x != y) {
            return x < y ? -1 : 1;
        }
    }
    return a.str.size() < b.str.size() ? -1 : (a.str.size() > b.str.size() ? 1 : 0);
}

// Java's String.hashCode over UTF-16 units.
[[nodiscard]] inline auto java_string_hash(std::string_view s) -> std::int32_t {
    std::uint32_t h = 0;
    std::size_t i = 0;
    while (i < s.size()) {
        const auto c = static_cast<unsigned char>(s[i]);
        std::uint32_t cp = c;
        std::size_t len = utf8_length(c);
        if (len > s.size() - i) {
            len = 1;
        }
        if (len == 2) {
            cp = ((c & 0x1FU) << 6U) | (static_cast<unsigned char>(s[i + 1]) & 0x3FU);
        } else if (len == 3) {
            cp = ((c & 0x0FU) << 12U) | ((static_cast<unsigned char>(s[i + 1]) & 0x3FU) << 6U) |
                 (static_cast<unsigned char>(s[i + 2]) & 0x3FU);
        } else if (len == 4) {
            cp = ((c & 0x07U) << 18U) | ((static_cast<unsigned char>(s[i + 1]) & 0x3FU) << 12U) |
                 ((static_cast<unsigned char>(s[i + 2]) & 0x3FU) << 6U) |
                 (static_cast<unsigned char>(s[i + 3]) & 0x3FU);
        }
        if (cp >= 0x10000U) {
            const auto v = cp - 0x10000U;
            h = h * 31U + (0xD800U + (v >> 10U));
            h = h * 31U + (0xDC00U + (v & 0x3FFU));
        } else {
            h = h * 31U + cp;
        }
        i += len;
    }
    return static_cast<std::int32_t>(h);
}

// The order a java.util.HashSet built from `total` keys iterates `keys`
// (given in insertion order): by bucket of the spread hash, then insertion
// order within a bucket. DynamoDB lists unused placeholders this way.
[[nodiscard]] inline auto java_hash_order(const std::vector<std::string>& keys, std::size_t total)
    -> std::vector<std::string> {
    std::size_t capacity = 16;
    const auto wanted = static_cast<std::size_t>(static_cast<double>(total) / 0.75) + 1;
    while (capacity < wanted) {
        capacity <<= 1U;
    }
    std::vector<std::pair<std::size_t, std::size_t>> slots;  // (bucket, position)
    slots.reserve(keys.size());
    for (std::size_t i = 0; i < keys.size(); ++i) {
        const auto h = static_cast<std::uint32_t>(java_string_hash(keys[i]));
        const auto spread = h ^ (h >> 16U);
        slots.emplace_back(spread & (capacity - 1), i);
    }
    std::sort(slots.begin(), slots.end());
    std::vector<std::string> out;
    out.reserve(keys.size());
    for (const auto& [bucket, i] : slots) {
        out.push_back(keys[i]);
    }
    return out;
}

}  // namespace expr_detail

/// Compiles one request's expressions against its placeholder maps.
class expression_compiler {
public:
    /// Checks the request's `ExpressionAttributeNames` and
    /// `ExpressionAttributeValues` (either may be absent).
    [[nodiscard]] static auto make(const boost::json::object* names,
                                   const boost::json::object* values)
        -> aws_json::result<expression_compiler> {
        expression_compiler c;
        if (names != nullptr) {
            if (names->empty()) {
                return validation_error("ExpressionAttributeNames must not be empty");
            }
            for (const auto& kv : *names) {
                const std::string key(kv.key());
                if (auto bad = key_problem(key, '#')) {
                    return validation_error("ExpressionAttributeNames contains invalid key: " +
                                            *bad);
                }
                const std::string_view value =
                    kv.value().is_string() ? std::string_view(kv.value().get_string()) : "";
                if (value.empty()) {
                    return validation_error(
                        "ExpressionAttributeNames contains invalid value: Empty attribute name "
                        "for key " +
                        key);
                }
                if (value.size() > k_max_attribute_name_bytes) {
                    return validation_error(
                        "ExpressionAttributeNames contains invalid value: Attribute name is too "
                        "large, must be less than 65536 bytes for key " +
                        key);
                }
                c._names.emplace(key, std::string(value));
                c._name_order.push_back(key);
            }
        }
        if (values != nullptr) {
            if (values->empty()) {
                return validation_error("ExpressionAttributeValues must not be empty");
            }
            for (const auto& kv : *values) {
                const std::string key(kv.key());
                if (auto bad = key_problem(key, ':')) {
                    return validation_error("ExpressionAttributeValues contains invalid key: " +
                                            *bad);
                }
                auto v = attribute_from_json(kv.value());
                if (!v) {
                    return validation_error("ExpressionAttributeValues contains invalid value: " +
                                            v.error().message.value_or("") + " for key " + key);
                }
                c._values.emplace(key, std::move(*v));
                c._raw_values.emplace(key, kv.value());
                c._value_order.push_back(key);
            }
        }
        return c;
    }

    /// A condition or filter expression (`kind` names it in messages).
    [[nodiscard]] auto condition(std::string_view text, expr_kind kind = expr_kind::condition)
        -> aws_json::result<expr_node> {
        return run(kind, [&]() -> expr_node {
            check_text(text);
            expr_detail::parser p(text, false);
            auto raw = parse(p, [&] { return p.condition(); });
            expr_detail::check_redundant_parens(raw);
            expr_detail::check_function_names(raw, false);
            expr_detail::check_words(raw);
            std::size_t operators = 0;
            expr_detail::check_limits(raw, operators);
            return compile(raw, true);
        });
    }

    /// A Query's filter: as `condition`, and no key attribute of `keys`
    /// (the table's or the queried index's) may appear.
    [[nodiscard]] auto filter(std::string_view text, const key_schema* keys)
        -> aws_json::result<expr_node> {
        auto n = condition(text, expr_kind::filter);
        if (!n || keys == nullptr) {
            return n;
        }
        if (auto e = filter_key_error(*n, *keys, false)) {
            return *e;
        }
        return n;
    }

    [[nodiscard]] auto update(std::string_view text) -> aws_json::result<update_expr> {
        return run(expr_kind::update, [&]() -> update_expr {
            check_text(text, false);
            expr_detail::parser p(text, true);
            auto raw = parse(p, [&] { return p.update(); });
            for (const auto& a : raw.actions) {
                if (a.kind == update_kind::set) {
                    expr_detail::check_redundant_parens(a.value);
                }
            }
            for (const auto& a : raw.actions) {
                if (a.kind == update_kind::set) {
                    expr_detail::check_function_names(a.value, true);
                }
            }
            for (const auto& a : raw.actions) {
                expr_detail::check_path_words(a.path);
                if (a.kind == update_kind::set) {
                    expr_detail::check_words(a.value);
                }
            }
            check_clauses(raw);
            for (const auto& a : raw.actions) {
                expr_detail::check_depth(a.path);
                if (a.kind == update_kind::set) {
                    std::size_t operators = 0;
                    expr_detail::check_limits(a.value, operators);
                }
            }
            // DynamoDB walks the actions last to first: every target path
            // (names substituted, overlaps checked against the paths after
            // it), then every action's operands.
            update_expr out;
            out.actions.resize(raw.actions.size());
            std::vector<document_path> paths(raw.actions.size());
            for (std::size_t j = raw.actions.size(); j-- > 0;) {
                paths[j] = substitute(raw.actions[j].path);
                check_overlap_with_later(paths, j);
            }
            for (std::size_t j = raw.actions.size(); j-- > 0;) {
                const auto& a = raw.actions[j];
                auto& act = out.actions[j];
                act.kind = a.kind;
                act.path = paths[j];
                if (a.kind == update_kind::set) {
                    act.value = compile_update_value(a.value, false);
                } else if (a.kind != update_kind::remove) {
                    act.value = compile(a.value, false);
                    check_add_delete(a.kind, act.value);
                }
            }
            return out;
        });
    }

    [[nodiscard]] auto projection(std::string_view text) -> aws_json::result<projection_expr> {
        return run(expr_kind::projection, [&]() -> projection_expr {
            check_text(text, false);
            expr_detail::parser p(text, true);
            auto raw = parse(p, [&] { return p.projection(); });
            for (const auto& path : raw) {
                expr_detail::check_path_words(path);
            }
            for (const auto& path : raw) {
                expr_detail::check_depth(path);
            }
            projection_expr out;
            out.paths.resize(raw.size());
            for (std::size_t j = raw.size(); j-- > 0;) {
                out.paths[j] = substitute(raw[j]);
                check_overlap_with_later(out.paths, j);
            }
            return out;
        });
    }

    /// A Query's key condition against the table's or index's key schema.
    [[nodiscard]] auto key(std::string_view text, const key_schema& schema)
        -> aws_json::result<key_condition> {
        auto tree = condition(text, expr_kind::key_condition);
        if (!tree) {
            return tree.error();
        }
        return key_from_tree(*tree, schema);
    }

    /// The unused-placeholder rule, once every expression has compiled.
    [[nodiscard]] auto finish() const -> std::optional<aws_json::error> {
        if (auto e = unused("ExpressionAttributeNames", _names, _used_names, _name_order)) {
            return e;
        }
        return unused("ExpressionAttributeValues", _values, _used_values, _value_order);
    }

    /// The key-schema rules of a key condition, on its tree (shared with
    /// the legacy `KeyConditions` translation).
    [[nodiscard]] static auto key_from_tree(const expr_node& tree, const key_schema& schema)
        -> aws_json::result<key_condition> {
        std::vector<const expr_node*> leaves;
        if (auto e = key_leaves(tree, leaves)) {
            return *e;
        }
        struct leaf {
            std::string name;
            expr_op op;
            std::vector<attribute_value> values;
        };
        std::vector<leaf> parsed;
        for (const auto* n : leaves) {
            leaf l;
            l.op = n->op;
            const expr_node* path = nullptr;
            bool flipped = false;
            for (std::size_t i = 0; i < n->args.size(); ++i) {
                const auto& a = n->args[i];
                if (a.op == expr_op::path) {
                    if (path != nullptr) {
                        return validation_error(
                            "Invalid condition in KeyConditionExpression: Multiple attribute "
                            "names used in one condition");
                    }
                    path = &a;
                    flipped = i != 0;
                } else if (a.op == expr_op::value) {
                    l.values.push_back(a.value);
                } else {
                    return validation_error(
                        "KeyConditionExpressions cannot contain nested operations");
                }
            }
            if (path == nullptr) {
                return validation_error(
                    "Invalid condition in KeyConditionExpression: No key attribute specified");
            }
            if (path->path.size() != 1) {
                const auto& top = path->path.front().name;
                if (top != schema.partition_name &&
                    (!schema.sort_name || top != *schema.sort_name)) {
                    return validation_error(
                        "KeyConditionExpressions cannot have conditions on nested attributes");
                }
                return validation_error(
                    "Key attributes must be scalars; list random access '[]' and map lookup '.' "
                    "are not allowed: Key: " +
                    top);
            }
            l.name = path->path.front().name;
            if (flipped) {
                l.op = flip(l.op);
            }
            parsed.push_back(std::move(l));
        }
        for (std::size_t i = 0; i < parsed.size(); ++i) {
            for (std::size_t j = i + 1; j < parsed.size(); ++j) {
                if (parsed[i].name == parsed[j].name) {
                    return validation_error(
                        "KeyConditionExpressions must only contain one condition per key");
                }
            }
        }
        if (parsed.size() > 2) {
            return validation_error("Conditions can be of length 1 or 2 only");
        }
        key_condition out;
        const leaf* pk = nullptr;
        const leaf* sk = nullptr;
        for (const auto& l : parsed) {
            if (l.name == schema.partition_name) {
                pk = &l;
            } else if (schema.sort_name && l.name == *schema.sort_name) {
                sk = &l;
            } else {
                return validation_error("Query condition missed key schema element");
            }
        }
        if (pk == nullptr) {
            return validation_error("Query condition missed key schema element");
        }
        if (pk->op != expr_op::eq) {
            return validation_error("Query key condition not supported");
        }
        const auto mismatch = [] {
            return validation_error(std::string(k_invalid_parameters) +
                                    "Condition parameter type does not match schema type");
        };
        if (pk->values.front().type != schema.partition_type) {
            return mismatch();
        }
        out.partition_name = pk->name;
        out.partition_value = pk->values.front();
        if (sk != nullptr) {
            for (const auto& v : sk->values) {
                if (v.type != schema.sort_type) {
                    return mismatch();
                }
            }
            key_condition::sort_condition s;
            s.name = sk->name;
            s.op = sk->op;
            s.values = sk->values;
            out.sort = std::move(s);
        }
        return out;
    }

    /// The Query-filter rule: no key attribute of `keys` in the filter.
    /// DynamoDB checks a nested path on the partition key, then on the sort
    /// key, then any reference to the partition key, then to the sort key.
    [[nodiscard]] static auto filter_key_error(const expr_node& n, const key_schema& keys,
                                               bool legacy) -> std::optional<aws_json::error> {
        std::vector<std::string> names{keys.partition_name};
        if (keys.sort_name) {
            names.push_back(*keys.sort_name);
        }
        for (const bool nested : {true, false}) {
            for (const auto& key : names) {
                if (!references(n, key, nested)) {
                    continue;
                }
                if (nested) {
                    return validation_error(
                        "Key attributes must be scalars; list random access '[]' and map "
                        "lookup '.' are not allowed: Key: " +
                        key);
                }
                return validation_error(std::string(legacy ? "QueryFilter" : "Filter Expression") +
                                        " can only contain non-primary key attributes: Primary "
                                        "key attribute: " +
                                        key);
            }
        }
        return std::nullopt;
    }

    // Whether `n` names attribute `top` (by a path of more than one step,
    // when `nested`).
    [[nodiscard]] static auto references(const expr_node& n, const std::string& top, bool nested)
        -> bool {
        if (n.op == expr_op::path) {
            return n.path.front().name == top && (!nested || n.path.size() > 1);
        }
        return std::any_of(n.args.begin(), n.args.end(),
                           [&](const expr_node& a) { return references(a, top, nested); });
    }

    /// "Cannot update attribute pk": an update may not touch a key
    /// attribute, the partition key checked first.
    [[nodiscard]] static auto check_key_updates(const update_expr& u, const key_schema& keys)
        -> std::optional<aws_json::error> {
        std::vector<std::string> names{keys.partition_name};
        if (keys.sort_name) {
            names.push_back(*keys.sort_name);
        }
        for (const auto& key : names) {
            for (const auto& a : u.actions) {
                if (a.path.front().name == key) {
                    return validation_error(std::string(k_invalid_parameters) +
                                            "Cannot update attribute " + key +
                                            ". This attribute is part of the key");
                }
            }
        }
        return std::nullopt;
    }

private:
    std::map<std::string, std::string, std::less<>> _names;
    std::map<std::string, attribute_value, std::less<>> _values;
    std::map<std::string, boost::json::value, std::less<>> _raw_values;
    std::set<std::string, std::less<>> _used_names;
    std::set<std::string, std::less<>> _used_values;
    std::vector<std::string> _name_order;  // as the request listed them
    std::vector<std::string> _value_order;

    template<typename F> auto run(expr_kind kind, F&& body) -> aws_json::result<decltype(body())> {
        try {
            return body();
        } catch (const expr_detail::compile_failure& f) {
            if (f.bare) {
                return validation_error(f.message);
            }
            return validation_error("Invalid " + std::string(expr_parameter(kind)) + ": " +
                                    f.message);
        }
    }

    // Update and projection expressions leave the size out of the message.
    static void check_text(std::string_view text, bool show_size = true) {
        if (text.empty()) {
            expr_detail::reject("The expression can not be empty;");
        }
        // Counted in UTF-16 units, as the Java service measures a string.
        const auto units = expr_detail::utf16_length(text);
        if (units > k_max_expression_bytes) {
            std::string m = "Expression size has exceeded the maximum allowed size;";
            if (show_size) {
                m += " expression size: " + std::to_string(units);
            }
            expr_detail::reject(m);
        }
    }

    template<typename F> static auto parse(const expr_detail::parser& p, F&& f) -> decltype(f()) {
        try {
            return f();
        } catch (const expr_detail::syntax_failure& s) {
            expr_detail::reject(p.syntax_message(s.token));
        }
    }

    // "Syntax error; key: "v"" for a key that is not a placeholder, or the
    // too-long message.
    [[nodiscard]] static auto key_problem(const std::string& key, char sigil)
        -> std::optional<std::string> {
        const bool shaped =
            key.size() >= 2 && key.front() == sigil &&
            std::all_of(key.begin() + 1, key.end(), [](char c) { return expr_detail::is_word(c); });
        if (!shaped) {
            return "Syntax error; key: \"" + key + "\"";
        }
        if (key.size() > k_max_placeholder_bytes) {
            std::string m = "The expression attribute map contains a key that is too long;";
            if (sigil == '#') {
                m += " size of key: " + std::to_string(key.size());
            }
            return m;
        }
        return std::nullopt;
    }

    static void check_clauses(const expr_detail::raw_update& u) {
        std::set<update_kind> seen;
        for (const auto k : u.clauses) {
            if (!seen.insert(k).second) {
                const char* name = k == update_kind::set      ? "SET"
                                   : k == update_kind::remove ? "REMOVE"
                                   : k == update_kind::add    ? "ADD"
                                                              : "DELETE";
                expr_detail::reject("The \"" + std::string(name) +
                                    "\" section can only be used once in an update expression;");
            }
        }
    }

    [[nodiscard]] auto substitute(const std::vector<expr_detail::raw_step>& raw) -> document_path {
        document_path out;
        out.reserve(raw.size());
        for (const auto& s : raw) {
            path_step step;
            if (s.is_index) {
                step.index = std::stoll(s.text);
            } else if (s.placeholder) {
                const auto it = _names.find(s.text);
                if (it == _names.end()) {
                    expr_detail::reject(
                        "An expression attribute name used in the document path is not defined; "
                        "attribute name: " +
                        s.text);
                }
                _used_names.insert(s.text);
                step.name = it->second;
            } else {
                step.name = s.text;
            }
            out.push_back(std::move(step));
        }
        return out;
    }

    [[nodiscard]] auto value_of(const std::string& placeholder) -> attribute_value {
        const auto it = _values.find(placeholder);
        if (it == _values.end()) {
            expr_detail::reject(
                "An expression attribute value used in expression is not defined; attribute "
                "value: " +
                placeholder);
        }
        _used_values.insert(placeholder);
        return it->second;
    }

    // "AttributeValue: {N:5}", from the value as the request wrote it.
    [[nodiscard]] auto render_value(const expr_detail::raw_node& raw) const -> std::string {
        const auto& r = expr_detail::strip(raw);
        const auto it = _raw_values.find(r.text);
        std::string body;
        if (it != _raw_values.end() && it->second.is_object()) {
            for (const auto& kv : it->second.get_object()) {
                if (kv.value().is_null()) {
                    continue;
                }
                body = std::string(kv.key()) + ":";
                if (kv.key() == "B" && kv.value().is_string()) {
                    // The Java service prints its ByteBuffer, whose
                    // length is the decoded byte count.
                    const std::string_view b64 = kv.value().get_string();
                    std::size_t bytes = b64.size() / 4 * 3;
                    for (std::size_t i = b64.size(); i > 0 && b64[i - 1] == '=' && bytes > 0; --i) {
                        --bytes;
                    }
                    const auto n = std::to_string(bytes);
                    body += "java.nio.HeapByteBuffer[pos=0 lim=" + n + " cap=" + n + "]";
                } else if (kv.value().is_string()) {
                    body += std::string_view(kv.value().get_string());
                } else {
                    body += boost::json::serialize(kv.value());
                }
                break;
            }
        }
        return "AttributeValue: {" + body + "}";
    }

    [[nodiscard]] static auto function_op(std::string_view f) -> expr_op {
        if (f == "attribute_exists") {
            return expr_op::attribute_exists;
        }
        if (f == "attribute_not_exists") {
            return expr_op::attribute_not_exists;
        }
        if (f == "attribute_type") {
            return expr_op::attribute_type;
        }
        if (f == "begins_with") {
            return expr_op::begins_with;
        }
        if (f == "contains") {
            return expr_op::contains;
        }
        if (f == "size") {
            return expr_op::size;
        }
        if (f == "if_not_exists") {
            return expr_op::if_not_exists;
        }
        return expr_op::list_append;
    }

    [[nodiscard]] static auto arity(expr_op op) -> std::size_t {
        switch (op) {
            case expr_op::attribute_exists:
            case expr_op::attribute_not_exists:
            case expr_op::size:
                return 1;
            default:
                return 2;
        }
    }

    [[nodiscard]] static auto op_text(const expr_detail::raw_node& raw) -> std::string {
        switch (raw.kind) {
            case expr_detail::raw_kind::compare:
            case expr_detail::raw_kind::func:
            case expr_detail::raw_kind::plus:
            case expr_detail::raw_kind::minus:
                return raw.text;
            case expr_detail::raw_kind::between:
                return "BETWEEN";
            case expr_detail::raw_kind::in:
                return "IN";
            default:
                return "";
        }
    }

    // The first operand, if a path, must not appear again among the others.
    static void check_distinct(const expr_detail::raw_node& raw,
                               const std::vector<expr_node>& args) {
        if (args.empty() || args.front().op != expr_op::path) {
            return;
        }
        for (std::size_t i = 1; i < args.size(); ++i) {
            if (args[i].op == expr_op::path && args[i].path == args.front().path) {
                expr_detail::reject(
                    "The first operand must be distinct from the remaining operands for this "
                    "operator or function; operator: " +
                    op_text(raw) + ", first operand: " + render_path(args.front().path));
            }
        }
    }

    [[nodiscard]] static auto is_boolean_function(expr_op op) -> bool {
        return op != expr_op::size && op != expr_op::if_not_exists && op != expr_op::list_append;
    }

    // Condition-side compile: `want_bool` is the position's requirement.
    [[nodiscard]] auto compile(const expr_detail::raw_node& raw, bool want_bool) -> expr_node {
        using expr_detail::raw_kind;
        switch (raw.kind) {
            case raw_kind::paren:
                return compile(raw.args.front(), want_bool);
            case raw_kind::path:
                return expr_node::of_path(substitute(raw.path));
            case raw_kind::value:
                return expr_node::of_value(value_of(raw.text));
            case raw_kind::and_:
            case raw_kind::or_:
            case raw_kind::not_: {
                std::vector<expr_node> args;
                for (const auto& a : raw.args) {
                    args.push_back(compile(a, true));
                }
                const auto op = raw.kind == raw_kind::and_  ? expr_op::and_
                                : raw.kind == raw_kind::or_ ? expr_op::or_
                                                            : expr_op::not_;
                return expr_node::of(op, std::move(args));
            }
            case raw_kind::compare: {
                std::vector<expr_node> args;
                for (const auto& a : raw.args) {
                    args.push_back(compile(a, false));
                }
                if (raw.op != expr_op::eq && raw.op != expr_op::ne) {
                    for (const auto& a : args) {
                        if (const auto t = expr_detail::known_type(a);
                            t && !expr_detail::is_scalar_ordered(*t)) {
                            expr_detail::reject(expr_detail::incorrect_type(raw.text, *t));
                        }
                    }
                }
                check_distinct(raw, args);
                return expr_node::of(raw.op, std::move(args));
            }
            case raw_kind::between: {
                std::vector<expr_node> args;
                for (const auto& a : raw.args) {
                    args.push_back(compile(a, false));
                }
                for (const auto& a : args) {
                    if (const auto t = expr_detail::known_type(a);
                        t && !expr_detail::is_scalar_ordered(*t)) {
                        expr_detail::reject(expr_detail::incorrect_type("BETWEEN", *t));
                    }
                }
                check_distinct(raw, args);
                if (args[1].op == expr_op::value && args[2].op == expr_op::value) {
                    const auto& lo = args[1].value;
                    const auto& hi = args[2].value;
                    if (lo.type != hi.type) {
                        expr_detail::reject(
                            "The BETWEEN operator requires same data type for lower and upper "
                            "bounds; lower bound operand: " +
                            render_value(raw.args[1]) +
                            ", upper bound operand: " + render_value(raw.args[2]));
                    }
                    if (expr_detail::compare_scalars(lo, hi) > 0) {
                        expr_detail::reject(
                            "The BETWEEN operator requires upper bound to be greater than or "
                            "equal to lower bound; lower bound operand: " +
                            render_value(raw.args[1]) +
                            ", upper bound operand: " + render_value(raw.args[2]));
                    }
                }
                return expr_node::of(expr_op::between, std::move(args));
            }
            case raw_kind::in: {
                std::vector<expr_node> args;
                for (const auto& a : raw.args) {
                    args.push_back(compile(a, false));
                }
                check_distinct(raw, args);
                return expr_node::of(expr_op::in, std::move(args));
            }
            case raw_kind::func:
                return compile_condition_function(raw, want_bool);
            case raw_kind::plus:
            case raw_kind::minus:
                break;
        }
        expr_detail::reject("Syntax error; token: \"+\"");
    }

    [[nodiscard]] auto compile_condition_function(const expr_detail::raw_node& raw, bool want_bool)
        -> expr_node {
        std::vector<expr_node> args;
        for (const auto& a : raw.args) {
            args.push_back(compile(a, false));
        }
        const auto op = function_op(raw.text);
        if (want_bool != is_boolean_function(op)) {
            expr_detail::reject(
                "The function is not allowed to be used this way in an expression; function: " +
                raw.text);
        }
        if (args.size() != arity(op)) {
            expr_detail::reject(
                "Incorrect number of operands for operator or function; operator or function: " +
                raw.text + ", number of operands: " + std::to_string(args.size()));
        }
        switch (op) {
            case expr_op::attribute_exists:
            case expr_op::attribute_not_exists:
                if (args.front().op != expr_op::path) {
                    expr_detail::reject(
                        "Operator or function requires a document path; operator or function: " +
                        raw.text);
                }
                break;
            case expr_op::attribute_type: {
                const auto& t = args[1];
                if (t.op == expr_op::path) {
                    expr_detail::reject(
                        "Incorrect operand type for operator or function; operator or function: "
                        "attribute_type, operand type: "
                        "{DICT,HD,NS,DOUBLESET,HDS,L,DOUBLE,NULL,B,SS,S,INTSET,FLOAT,INT,DECIMAL,"
                        "DECIMALSET,FS,N,BOOL,BS,M}");
                }
                if (const auto kt = expr_detail::known_type(t); kt && *kt != attribute_type::S) {
                    expr_detail::reject(expr_detail::incorrect_type("attribute_type", *kt));
                }
                if (t.op == expr_op::value && !valid_type_name(t.value.str)) {
                    expr_detail::reject("Invalid attribute type name found; type: " + t.value.str +
                                        ", valid types: {N,BS,L,B,NULL,M,S,SS,NS,BOOL}");
                }
                break;
            }
            case expr_op::begins_with:
                for (const auto& a : args) {
                    if (const auto t = expr_detail::known_type(a);
                        t && *t != attribute_type::S && *t != attribute_type::B) {
                        expr_detail::reject(expr_detail::incorrect_type("begins_with", *t));
                    }
                }
                check_distinct(raw, args);
                break;
            case expr_op::contains:
                check_distinct(raw, args);
                break;
            case expr_op::size:
                if (const auto t = expr_detail::known_type(args.front());
                    t && (*t == attribute_type::N || *t == attribute_type::BOOL ||
                          *t == attribute_type::NULL_)) {
                    expr_detail::reject(expr_detail::incorrect_type("size", *t));
                }
                break;
            default:
                break;
        }
        return expr_node::of(op, std::move(args));
    }

    [[nodiscard]] static auto valid_type_name(std::string_view t) -> bool {
        return t == "S" || t == "N" || t == "B" || t == "BOOL" || t == "NULL" || t == "M" ||
               t == "L" || t == "SS" || t == "NS" || t == "BS";
    }

    // SET's right-hand side. `nested` is true under a function or +/-.
    [[nodiscard]] auto compile_update_value(const expr_detail::raw_node& raw, bool nested)
        -> expr_node {
        using expr_detail::raw_kind;
        switch (raw.kind) {
            case raw_kind::paren:
                return compile_update_value(raw.args.front(), nested);
            case raw_kind::path:
            case raw_kind::value:
                return compile(raw, false);
            case raw_kind::plus:
            case raw_kind::minus: {
                std::vector<expr_node> args;
                for (const auto& a : raw.args) {
                    args.push_back(compile_update_value(a, true));
                }
                for (const auto& a : args) {
                    if (const auto t = expr_detail::known_type(a); t && *t != attribute_type::N) {
                        expr_detail::reject(expr_detail::incorrect_type(raw.text, *t));
                    }
                }
                return expr_node::of(raw.kind == raw_kind::plus ? expr_op::plus : expr_op::minus,
                                     std::move(args));
            }
            case raw_kind::func: {
                std::vector<expr_node> args;
                for (const auto& a : raw.args) {
                    args.push_back(compile_update_value(a, true));
                }
                const auto op = function_op(raw.text);
                if (args.size() != 2) {
                    expr_detail::reject(
                        "Incorrect number of operands for operator or function; operator or "
                        "function: " +
                        raw.text + ", number of operands: " + std::to_string(args.size()));
                }
                if (op == expr_op::if_not_exists) {
                    if (args.front().op != expr_op::path) {
                        expr_detail::reject(
                            "Operator or function requires a document path; operator or "
                            "function: if_not_exists");
                    }
                } else {
                    if (nested) {
                        expr_detail::reject(
                            "The function is not allowed to be used this way in an expression; "
                            "function: list_append");
                    }
                    for (const auto& a : args) {
                        if (const auto t = expr_detail::known_type(a);
                            t && *t != attribute_type::L) {
                            expr_detail::reject(expr_detail::incorrect_type("list_append", *t));
                        }
                    }
                }
                return expr_node::of(op, std::move(args));
            }
            default:
                break;
        }
        expr_detail::reject("Syntax error");
    }

    static void check_add_delete(update_kind kind, const expr_node& value) {
        const auto t = value.value.type;
        const bool ok = kind == update_kind::add
                            ? (t == attribute_type::N || expr_detail::is_set(t))
                            : expr_detail::is_set(t);
        if (!ok) {
            const bool add = kind == update_kind::add;
            expr_detail::reject(
                "Incorrect operand type for operator or function; operator: " +
                std::string(add ? "ADD" : "DELETE") +
                ", operand type: " + std::string(expr_detail::update_type_name(t)) + ", typeSet: " +
                std::string(add ? "ALLOWED_FOR_ADD_OPERAND" : "ALLOWED_FOR_DELETE_OPERAND"));
        }
    }

    // Two paths overlap when one is a prefix of the other, and conflict when
    // they first differ at a step that is a name in one and an index in the
    // other. Path `j` is checked against every path after it, the walk
    // running last to first as DynamoDB's does, so the kind reported
    // (overlap or conflict) matches. With three or more paths DynamoDB Local
    // sometimes names a different, unrelated path as "path two"; Kythira
    // names the pair that actually clashes.
    static void check_overlap_with_later(const std::vector<document_path>& paths, std::size_t j) {
        const auto& a = paths[j];
        for (std::size_t k = j + 1; k < paths.size(); ++k) {
            const auto& b = paths[k];
            const auto n = std::min(a.size(), b.size());
            bool independent = false;
            for (std::size_t i = 0; i < n; ++i) {
                if (a[i] == b[i]) {
                    continue;
                }
                if (a[i].is_index() != b[i].is_index()) {
                    expr_detail::reject(
                        "Two document paths conflict with each other; must remove or "
                        "rewrite one of these paths; path one: " +
                        render_path(a) + ", path two: " + render_path(b));
                }
                independent = true;
                break;
            }
            if (!independent) {
                expr_detail::reject(
                    "Two document paths overlap with each other; must remove or rewrite one "
                    "of these paths; path one: " +
                    render_path(a) + ", path two: " + render_path(b));
            }
        }
    }

    [[nodiscard]] static auto key_operator_name(expr_op op) -> std::string_view {
        switch (op) {
            case expr_op::or_:
                return "OR";
            case expr_op::not_:
                return "NOT";
            case expr_op::ne:
                return "<>";
            case expr_op::in:
                return "IN";
            case expr_op::attribute_exists:
                return "attribute_exists";
            case expr_op::attribute_not_exists:
                return "attribute_not_exists";
            case expr_op::attribute_type:
                return "attribute_type";
            case expr_op::contains:
                return "contains";
            case expr_op::size:
                return "size";
            default:
                return "";
        }
    }

    [[nodiscard]] static auto key_leaves(const expr_node& n, std::vector<const expr_node*>& out)
        -> std::optional<aws_json::error> {
        switch (n.op) {
            case expr_op::and_:
                for (const auto& a : n.args) {
                    if (auto e = key_leaves(a, out)) {
                        return e;
                    }
                }
                return std::nullopt;
            case expr_op::eq:
            case expr_op::lt:
            case expr_op::le:
            case expr_op::gt:
            case expr_op::ge:
            case expr_op::between:
            case expr_op::begins_with:
                out.push_back(&n);
                return std::nullopt;
            default:
                return validation_error("Invalid operator used in KeyConditionExpression: " +
                                        std::string(key_operator_name(n.op)));
        }
    }

    [[nodiscard]] static auto flip(expr_op op) -> expr_op {
        switch (op) {
            case expr_op::lt:
                return expr_op::gt;
            case expr_op::le:
                return expr_op::ge;
            case expr_op::gt:
                return expr_op::lt;
            case expr_op::ge:
                return expr_op::le;
            default:
                return op;
        }
    }

    template<typename Map>
    [[nodiscard]] auto unused(std::string_view parameter, const Map& defined,
                              const std::set<std::string, std::less<>>& used,
                              const std::vector<std::string>& order) const
        -> std::optional<aws_json::error> {
        std::vector<std::string> left;
        for (const auto& k : order) {
            if (defined.contains(k) && !used.contains(k)) {
                left.push_back(k);
            }
        }
        std::string keys;
        for (const auto& k : expr_detail::java_hash_order(left, defined.size())) {
            if (!keys.empty()) {
                keys += ", ";
            }
            keys += k;
        }
        if (keys.empty()) {
            return std::nullopt;
        }
        return validation_error("Value provided in " + std::string(parameter) +
                                " unused in expressions: keys: {" + keys + "}");
    }
};

}  // namespace kythira::dynamodb
