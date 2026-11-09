// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file iam_sts_token.hpp
/// @brief STS session tokens (`.kiro/specs/iam-compatible-auth/` design
///        Component 9, Requirements 10.3-10.4; task 17).
///
/// A session token carries everything a member needs to check an `ASIA`
/// signature with no stored per-session state:
///
///   token = base64url( 0x01 | kid8 | nonce (12) | tag (16) | ciphertext )
///   ciphertext = AES-256-GCM(CBOR(session)), associated data
///                "kythira-sts-session-v1" | kid8
///
/// `kid8` is the first 8 bytes of SHA-256 of the STS key's id, so a member
/// finds the key among the few in the state without a lookup table. The
/// payload is CBOR (RFC 8949) of a JSON-shaped map, written by the small
/// codec below: the Raft RPC serializer's primitives are private to it.
///
/// A token whose key is no longer in the state (retired past its window)
/// or whose `exp` has passed is `ExpiredToken`; anything that does not
/// open is `InvalidClientTokenId`.

#include <kythira/iam/iam_seal.hpp>
#include <kythira/iam/iam_state_machine.hpp>

#include <raft/aws_request_authorizer.hpp>

#include <raft/jwt.hpp>  // kythira::jose::base64url_*

#include <boost/json.hpp>

#include <openssl/crypto.h>
#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kythira::iam::sts {

// ── A JSON-shaped CBOR codec ────────────────────────────────────────────────

namespace cbor {

namespace detail {

inline void head(std::string& out, std::uint8_t major, std::uint64_t v) {
    const auto m = static_cast<char>(major << 5U);
    if (v < 24) {
        out += static_cast<char>(m | static_cast<char>(v));
        return;
    }
    int bytes = 8;
    char info = 27;
    if (v <= 0xFFU) {
        bytes = 1;
        info = 24;
    } else if (v <= 0xFFFFU) {
        bytes = 2;
        info = 25;
    } else if (v <= 0xFFFFFFFFU) {
        bytes = 4;
        info = 26;
    }
    out += static_cast<char>(m | info);
    for (int i = bytes - 1; i >= 0; --i) {
        out += static_cast<char>((v >> (8U * static_cast<unsigned>(i))) & 0xFFU);
    }
}

/// Deep enough for a session's maps and lists; deeper input is refused.
inline constexpr int max_depth = 8;

struct reader {
    std::string_view in;
    std::size_t pos = 0;

    auto byte() -> std::optional<std::uint8_t> {
        if (pos >= in.size()) {
            return std::nullopt;
        }
        return static_cast<std::uint8_t>(in[pos++]);
    }

    /// The major type and argument of the next item.
    auto item_head() -> std::optional<std::pair<std::uint8_t, std::uint64_t>> {
        const auto b = byte();
        if (!b) {
            return std::nullopt;
        }
        const std::uint8_t major = *b >> 5U;
        const std::uint8_t info = *b & 0x1FU;
        if (info < 24) {
            return std::pair{major, std::uint64_t{info}};
        }
        if (info > 27) {
            return std::nullopt;  // indefinite lengths and reserved values
        }
        const int bytes = 1 << (info - 24);
        std::uint64_t v = 0;
        for (int i = 0; i < bytes; ++i) {
            const auto x = byte();
            if (!x) {
                return std::nullopt;
            }
            v = (v << 8U) | *x;
        }
        return std::pair{major, v};
    }

    auto value(int depth) -> std::optional<boost::json::value> {
        if (depth > max_depth) {
            return std::nullopt;
        }
        const auto h = item_head();
        if (!h) {
            return std::nullopt;
        }
        const auto [major, arg] = *h;
        switch (major) {
            case 0:
                if (arg > static_cast<std::uint64_t>(INT64_MAX)) {
                    return std::nullopt;
                }
                return boost::json::value(static_cast<std::int64_t>(arg));
            case 1:
                if (arg > static_cast<std::uint64_t>(INT64_MAX)) {
                    return std::nullopt;
                }
                return boost::json::value(-1 - static_cast<std::int64_t>(arg));
            case 3: {
                if (arg > in.size() - pos) {
                    return std::nullopt;
                }
                boost::json::value v(boost::json::string(in.substr(pos, arg)));
                pos += arg;
                return v;
            }
            case 4: {
                if (arg > in.size() - pos) {  // each element takes a byte
                    return std::nullopt;
                }
                boost::json::array a;
                for (std::uint64_t i = 0; i < arg; ++i) {
                    auto e = value(depth + 1);
                    if (!e) {
                        return std::nullopt;
                    }
                    a.push_back(std::move(*e));
                }
                return boost::json::value(std::move(a));
            }
            case 5: {
                if (arg > (in.size() - pos) / 2) {
                    return std::nullopt;
                }
                boost::json::object o;
                for (std::uint64_t i = 0; i < arg; ++i) {
                    auto k = value(depth + 1);
                    if (!k || !k->is_string()) {
                        return std::nullopt;
                    }
                    auto v = value(depth + 1);
                    if (!v) {
                        return std::nullopt;
                    }
                    o[k->as_string()] = std::move(*v);
                }
                return boost::json::value(std::move(o));
            }
            case 7:
                if (arg == 20) {
                    return boost::json::value(false);
                }
                if (arg == 21) {
                    return boost::json::value(true);
                }
                if (arg == 22) {
                    return boost::json::value(nullptr);
                }
                return std::nullopt;
            default:
                return std::nullopt;  // byte strings, tags and floats are not used
        }
    }
};

}  // namespace detail

/// Writes `v`: integers, strings, booleans, null, arrays and objects. A
/// double is refused, since no session field is one.
[[nodiscard]] inline auto encode(const boost::json::value& v) -> std::string {
    std::string out;
    const std::function<void(const boost::json::value&)> put = [&](const boost::json::value& x) {
        switch (x.kind()) {
            case boost::json::kind::int64: {
                const auto n = x.as_int64();
                if (n >= 0) {
                    detail::head(out, 0, static_cast<std::uint64_t>(n));
                } else {
                    detail::head(out, 1, static_cast<std::uint64_t>(-1 - n));
                }
                break;
            }
            case boost::json::kind::uint64:
                detail::head(out, 0, x.as_uint64());
                break;
            case boost::json::kind::string: {
                const auto& s = x.as_string();
                detail::head(out, 3, s.size());
                out.append(s.data(), s.size());
                break;
            }
            case boost::json::kind::bool_:
                out += static_cast<char>(x.as_bool() ? 0xF5 : 0xF4);
                break;
            case boost::json::kind::null:
                out += static_cast<char>(0xF6);
                break;
            case boost::json::kind::array:
                detail::head(out, 4, x.as_array().size());
                for (const auto& e : x.as_array()) {
                    put(e);
                }
                break;
            case boost::json::kind::object:
                detail::head(out, 5, x.as_object().size());
                for (const auto& [k, e] : x.as_object()) {
                    detail::head(out, 3, k.size());
                    out.append(k.data(), k.size());
                    put(e);
                }
                break;
            case boost::json::kind::double_:
                throw std::invalid_argument("sts cbor: doubles are not encoded");
        }
    };
    put(v);
    return out;
}

/// Reads one item that fills `in` exactly, or nothing.
[[nodiscard]] inline auto decode(std::string_view in) -> std::optional<boost::json::value> {
    detail::reader r{in};
    auto v = r.value(0);
    if (!v || r.pos != in.size()) {
        return std::nullopt;
    }
    return v;
}

}  // namespace cbor

// ── The session a token carries ─────────────────────────────────────────────

/// Who issued the credentials (design Component 9's `issuer_kind`).
inline constexpr std::string_view k_issuer_role = "role";
inline constexpr std::string_view k_issuer_federated = "federated";
inline constexpr std::string_view k_issuer_session_token = "session_token";

struct session {
    std::string account;
    std::string principal_arn;  // assumed-role/... , federated-user/... or the user
    std::string userid;         // aws:userid
    std::string issuer_kind;    // one of the k_issuer_* values
    // Role sessions.
    std::string role_arn;
    std::string role_name;
    std::string session_name;
    // GetSessionToken and GetFederationToken: the IAM user (empty: root).
    std::string user_arn;
    std::string user_name;
    std::string source_identity;
    std::int64_t iat = 0;
    std::int64_t exp = 0;
    std::string access_key_id;
    std::string secret_access_key;
    std::vector<std::string> session_policies;  // policy JSON
    std::vector<std::pair<std::string, std::string>> tags;
    std::vector<std::string> transitive_tag_keys;
    bool chained = false;

    auto operator==(const session&) const -> bool = default;
};

[[nodiscard]] inline auto to_json(const session& s) -> boost::json::object {
    boost::json::array policies;
    for (const auto& p : s.session_policies) {
        policies.emplace_back(p);
    }
    boost::json::array tags;
    for (const auto& [k, v] : s.tags) {
        tags.push_back(boost::json::array{boost::json::string(k), boost::json::string(v)});
    }
    boost::json::array transitive;
    for (const auto& k : s.transitive_tag_keys) {
        transitive.emplace_back(k);
    }
    return {{"v", 1},
            {"acct", s.account},
            {"principal_arn", s.principal_arn},
            {"userid", s.userid},
            {"issuer_kind", s.issuer_kind},
            {"role_arn", s.role_arn},
            {"role_name", s.role_name},
            {"session_name", s.session_name},
            {"user_arn", s.user_arn},
            {"user_name", s.user_name},
            {"source_identity", s.source_identity},
            {"iat", s.iat},
            {"exp", s.exp},
            {"access_key_id", s.access_key_id},
            {"secret_access_key", s.secret_access_key},
            {"session_policies", std::move(policies)},
            {"tags", std::move(tags)},
            {"transitive_tag_keys", std::move(transitive)},
            {"chained", s.chained},
            {"mfa", false}};
}

/// The session in `o`, or nothing when a field is missing or mistyped.
[[nodiscard]] inline auto from_json(const boost::json::object& o) -> std::optional<session> {
    try {
        if (o.at("v").as_int64() != 1) {
            return std::nullopt;
        }
        const auto str = [&](std::string_view k) { return std::string(o.at(k).as_string()); };
        session s;
        s.account = str("acct");
        s.principal_arn = str("principal_arn");
        s.userid = str("userid");
        s.issuer_kind = str("issuer_kind");
        s.role_arn = str("role_arn");
        s.role_name = str("role_name");
        s.session_name = str("session_name");
        s.user_arn = str("user_arn");
        s.user_name = str("user_name");
        s.source_identity = str("source_identity");
        s.iat = o.at("iat").as_int64();
        s.exp = o.at("exp").as_int64();
        s.access_key_id = str("access_key_id");
        s.secret_access_key = str("secret_access_key");
        for (const auto& p : o.at("session_policies").as_array()) {
            s.session_policies.emplace_back(p.as_string());
        }
        for (const auto& t : o.at("tags").as_array()) {
            const auto& pair = t.as_array();
            s.tags.emplace_back(std::string(pair.at(0).as_string()),
                                std::string(pair.at(1).as_string()));
        }
        for (const auto& k : o.at("transitive_tag_keys").as_array()) {
            s.transitive_tag_keys.emplace_back(k.as_string());
        }
        s.chained = o.at("chained").as_bool();
        return s;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

// ── Sealing ─────────────────────────────────────────────────────────────────

inline constexpr std::uint8_t k_token_version = 0x01;
inline constexpr std::size_t k_kid_len = 8;

using kid8 = std::array<unsigned char, k_kid_len>;

/// The 8 bytes a token names its STS key by.
[[nodiscard]] inline auto kid_digest(std::string_view kid) -> kid8 {
    std::array<unsigned char, EVP_MAX_MD_SIZE> md{};
    unsigned int len = 0;
    if (EVP_Digest(kid.data(), kid.size(), md.data(), &len, EVP_sha256(), nullptr) != 1) {
        throw std::runtime_error("sts: SHA-256 failed");
    }
    kid8 out{};
    std::copy_n(md.begin(), k_kid_len, out.begin());
    return out;
}

[[nodiscard]] inline auto associated_data(const kid8& kid) -> std::string {
    std::string ad = "kythira-sts-session-v1";
    ad.append(reinterpret_cast<const char*>(kid.data()), kid.size());  // NOLINT
    return ad;
}

/// One STS key, opened from the state.
struct sts_key {
    std::string kid;
    seal_key key;
};

/// Seals `s` under `key`.
[[nodiscard]] inline auto seal(const sts_key& key, const session& s) -> std::string {
    const auto kid = kid_digest(key.kid);
    auto payload = cbor::encode(to_json(s));
    const auto sealed = seal_secret(key.key, payload, associated_data(kid));
    OPENSSL_cleanse(payload.data(), payload.size());
    const auto blob = jose::base64url_decode(sealed);
    if (!blob) {
        throw std::logic_error("sts: seal_secret wrote invalid base64url");
    }
    std::vector<unsigned char> out;
    out.reserve(1 + k_kid_len + blob->size());
    out.push_back(k_token_version);
    out.insert(out.end(), kid.begin(), kid.end());
    out.insert(out.end(), blob->begin(), blob->end());
    return jose::base64url_encode(out);
}

/// Why a token does not open.
enum class token_error : std::uint8_t {
    invalid,  // not ours, damaged or forged: InvalidClientTokenId
    expired,  // past `exp`, or its key has been retired: ExpiredToken
};

/// A session, or why there is none.
class opened {
public:
    opened(session s) : _s(std::move(s)) {}  // NOLINT(google-explicit-constructor)
    opened(token_error e) : _e(e) {}         // NOLINT(google-explicit-constructor)
    [[nodiscard]] auto has_value() const noexcept -> bool { return _s.has_value(); }
    [[nodiscard]] auto value() const -> const session& { return _s.value(); }
    [[nodiscard]] auto error() const noexcept -> token_error { return _e; }

private:
    std::optional<session> _s;
    token_error _e = token_error::invalid;
};

/// Opens `token` with whichever of `keys` it names, and checks it has not
/// expired at `now` (seconds since the epoch).
[[nodiscard]] inline auto open(std::string_view token, const std::vector<sts_key>& keys,
                               std::int64_t now) -> opened {
    const auto raw = jose::base64url_decode(token);
    if (!raw || raw->size() < 1 + k_kid_len || (*raw)[0] != k_token_version) {
        return token_error::invalid;
    }
    kid8 kid{};
    std::copy_n(raw->begin() + 1, k_kid_len, kid.begin());
    const sts_key* key = nullptr;
    for (const auto& k : keys) {
        if (kid_digest(k.kid) == kid) {
            key = &k;
            break;
        }
    }
    if (key == nullptr) {
        // Well formed but naming no key we hold: one retired past its
        // window (Requirement 10.4), so the token has outlived it.
        return token_error::expired;
    }
    const std::vector<unsigned char> blob(raw->begin() + 1 + k_kid_len, raw->end());
    auto payload = open_secret(key->key, jose::base64url_encode(blob), associated_data(kid));
    if (!payload) {
        return token_error::invalid;
    }
    const auto v = cbor::decode(*payload);
    OPENSSL_cleanse(payload->data(), payload->size());
    if (!v || !v->is_object()) {
        return token_error::invalid;
    }
    auto s = from_json(v->as_object());
    if (!s) {
        return token_error::invalid;
    }
    if (now >= s->exp) {
        return token_error::expired;
    }
    return std::move(*s);
}

// ── Keys and principals from the state ───────────────────────────────────────

/// The STS keys in `sm`, opened under the KEK, newest first. A key that does
/// not open (the wrong KEK) is left out, so its tokens fail closed.
[[nodiscard]] inline auto keys_of(const iam_state_machine& sm, const seal_key& kek)
    -> std::vector<sts_key> {
    std::vector<std::pair<std::int64_t, sts_key>> found;
    for (const auto& r : sm.sts_keys()) {
        auto text = open_secret(kek, r.sealed_key, r.kid);
        if (!text) {
            continue;
        }
        auto raw = jose::base64url_decode(*text);
        OPENSSL_cleanse(text->data(), text->size());
        if (!raw || raw->size() != seal_key::size) {
            continue;
        }
        std::array<unsigned char, seal_key::size> b{};
        std::copy(raw->begin(), raw->end(), b.begin());
        OPENSSL_cleanse(raw->data(), raw->size());
        found.emplace_back(r.retire_after ? 0 : r.create_date, sts_key{r.kid, seal_key(b)});
        OPENSSL_cleanse(b.data(), b.size());
    }
    std::ranges::stable_sort(found, std::ranges::greater{}, &decltype(found)::value_type::first);
    std::vector<sts_key> out;
    for (auto& [_, k] : found) {
        out.push_back(std::move(k));
    }
    return out;
}

/// The key new tokens are sealed with: the newest one not retiring.
[[nodiscard]] inline auto signing_key(const iam_state_machine& sm, const seal_key& kek)
    -> std::optional<sts_key> {
    std::optional<sts_key> out;
    std::int64_t newest = -1;
    for (const auto& r : sm.sts_keys()) {
        if (r.retire_after || r.create_date <= newest) {
            continue;
        }
        auto keys = keys_of(sm, kek);
        for (auto& k : keys) {
            if (k.kid == r.kid) {
                newest = r.create_date;
                out = std::move(k);
            }
        }
    }
    return out;
}

/// The caller a session's credentials sign as.
[[nodiscard]] inline auto principal_of(const session& s) -> aws_auth::principal_context {
    aws_auth::principal_context p;
    p.account = s.account;
    p.arn = s.principal_arn;
    p.userid = s.userid;
    p.access_key_id = s.access_key_id;
    p.source_identity = s.source_identity;
    p.session_policies = s.session_policies;
    p.tags = s.tags;
    p.token_issue_time = std::chrono::system_clock::time_point(std::chrono::seconds(s.iat));
    p.expires = std::chrono::system_clock::time_point(std::chrono::seconds(s.exp));
    if (s.issuer_kind == k_issuer_role) {
        p.type = aws_auth::principal_type::assumed_role;
        p.role_arn = s.role_arn;
        p.role_name = s.role_name;
        p.session_name = s.session_name;
    } else if (s.issuer_kind == k_issuer_federated) {
        p.type = aws_auth::principal_type::federated_user;
        p.user_arn = s.user_arn;
        p.session_name = s.session_name;
    } else {
        p.type =
            s.user_name.empty() ? aws_auth::principal_type::root : aws_auth::principal_type::user;
        p.username = s.user_name;
        p.session_token = true;
    }
    return p;
}

}  // namespace kythira::iam::sts
