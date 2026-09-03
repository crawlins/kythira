// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file redis_acl.hpp
/// @brief Users, secrets, roles and key scopes for the Redis-compatible
///        gateway (.kiro/specs/redis-compatible-kv/ design Component 6).
///
/// The threat model is a build farm: every sccache client shares one
/// password and it lives in `SCCACHE_REDIS_ENDPOINT` on hundreds of
/// machines. So the ACL assumes the secret will leak eventually and limits
/// what it buys: a role (a read-only build farm cannot poison the cache),
/// a key prefix (one farm cannot see another's artefacts) and a KDF slow
/// enough that a copy of the ACL file is not a copy of the password.
///
/// ACL file format, one user per line, `#` comments and blank lines ignored:
///
///     user <name> <secret-record|nopass> <role> [prefix ...] [cert=<subject>]
///
/// where `<secret-record>` is `pbkdf2-sha256$<iterations>$<b64 salt>$<b64 dk>`
/// as produced by `redis_acl::hash_secret` (and the daemon's `--hash-secret`
/// subcommand). A user with no prefix can reach no keys: scope is granted,
/// never assumed. A `cert=` subject lets an mTLS client authenticate without
/// an AUTH command.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

namespace kythira {

enum class redis_role : std::uint8_t {
    read_only,
    read_write,
    admin
};

[[nodiscard]] inline auto redis_role_name(redis_role r) -> std::string_view {
    switch (r) {
        case redis_role::read_only:
            return "read_only";
        case redis_role::read_write:
            return "read_write";
        case redis_role::admin:
            return "admin";
    }
    return "unknown";
}

[[nodiscard]] inline auto parse_redis_role(std::string_view s) -> std::optional<redis_role> {
    if (s == "read_only" || s == "read-only" || s == "ro") {
        return redis_role::read_only;
    }
    if (s == "read_write" || s == "read-write" || s == "rw") {
        return redis_role::read_write;
    }
    if (s == "admin") {
        return redis_role::admin;
    }
    return std::nullopt;
}

struct redis_acl_user {
    std::string _name;
    /// `pbkdf2-sha256$<iters>$<b64 salt>$<b64 dk>`, or empty for `nopass`.
    std::string _secret;
    bool _enabled = true;
    redis_role _role = redis_role::read_only;
    /// Key prefixes this user may touch. Empty means NO keys.
    std::vector<std::string> _key_prefixes;
    /// TLS client-certificate subjects that authenticate as this user.
    std::vector<std::string> _cert_subjects;
};

/// What a connection carries after authentication succeeds.
struct redis_identity {
    std::string _user;
    redis_role _role = redis_role::read_only;
    std::vector<std::string> _key_prefixes;
};

enum class acl_decision : std::uint8_t {
    allow,
    deny_command,
    deny_key
};

/// Which role a command needs. Commands outside the closure are refused by
/// the gateway before authorization runs, so this only has to know the
/// closure plus the diagnostics.
[[nodiscard]] inline auto redis_command_min_role(std::string_view upper_name)
    -> std::optional<redis_role> {
    if (upper_name == "GET" || upper_name == "EXISTS" || upper_name == "STRLEN" ||
        upper_name == "GETRANGE" || upper_name == "TTL" || upper_name == "PING" ||
        upper_name == "ECHO" || upper_name == "SELECT" || upper_name == "CLIENT" ||
        upper_name == "COMMAND" || upper_name == "QUIT" || upper_name == "RESET" ||
        upper_name == "AUTH" || upper_name == "HELLO" || upper_name == "DBSIZE") {
        return redis_role::read_only;
    }
    if (upper_name == "SET" || upper_name == "SETEX" || upper_name == "DEL") {
        return redis_role::read_write;
    }
    if (upper_name == "INFO") {
        return redis_role::admin;
    }
    return std::nullopt;
}

class redis_acl_parse_error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

namespace redis_acl_detail {

inline constexpr std::string_view b64_alphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

[[nodiscard]] inline auto b64_encode(const std::vector<unsigned char>& in) -> std::string {
    std::string out;
    out.reserve(((in.size() + 2) / 3) * 4);
    std::size_t i = 0;
    while (i + 2 < in.size()) {
        std::uint32_t v = (static_cast<std::uint32_t>(in[i]) << 16) |
                          (static_cast<std::uint32_t>(in[i + 1]) << 8) |
                          static_cast<std::uint32_t>(in[i + 2]);
        out += b64_alphabet[(v >> 18) & 63];
        out += b64_alphabet[(v >> 12) & 63];
        out += b64_alphabet[(v >> 6) & 63];
        out += b64_alphabet[v & 63];
        i += 3;
    }
    if (i + 1 == in.size()) {
        std::uint32_t v = static_cast<std::uint32_t>(in[i]) << 16;
        out += b64_alphabet[(v >> 18) & 63];
        out += b64_alphabet[(v >> 12) & 63];
        out += "==";
    } else if (i + 2 == in.size()) {
        std::uint32_t v = (static_cast<std::uint32_t>(in[i]) << 16) |
                          (static_cast<std::uint32_t>(in[i + 1]) << 8);
        out += b64_alphabet[(v >> 18) & 63];
        out += b64_alphabet[(v >> 12) & 63];
        out += b64_alphabet[(v >> 6) & 63];
        out += '=';
    }
    return out;
}

[[nodiscard]] inline auto b64_decode(std::string_view in)
    -> std::optional<std::vector<unsigned char>> {
    if (in.size() % 4 != 0) {
        return std::nullopt;
    }
    std::vector<unsigned char> out;
    out.reserve(in.size() / 4 * 3);
    std::uint32_t acc = 0;
    int bits = 0;
    std::size_t pad = 0;
    for (char c : in) {
        if (c == '=') {
            ++pad;
            continue;
        }
        if (pad != 0) {
            return std::nullopt;
        }
        auto pos = b64_alphabet.find(c);
        if (pos == std::string_view::npos) {
            return std::nullopt;
        }
        acc = (acc << 6) | static_cast<std::uint32_t>(pos);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<unsigned char>((acc >> bits) & 0xFF));
        }
    }
    if (pad > 2) {
        return std::nullopt;
    }
    return out;
}

inline auto split_ws(std::string_view line) -> std::vector<std::string> {
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) {
            ++i;
        }
        std::size_t start = i;
        while (i < line.size() && line[i] != ' ' && line[i] != '\t') {
            ++i;
        }
        if (i > start) {
            out.emplace_back(line.substr(start, i - start));
        }
    }
    return out;
}

struct secret_record {
    std::uint32_t _iterations = 0;
    std::vector<unsigned char> _salt;
    std::vector<unsigned char> _dk;
};

[[nodiscard]] inline auto parse_secret_record(std::string_view s) -> std::optional<secret_record> {
    constexpr std::string_view prefix = "pbkdf2-sha256$";
    if (s.substr(0, prefix.size()) != prefix) {
        return std::nullopt;
    }
    s.remove_prefix(prefix.size());
    auto d1 = s.find('$');
    if (d1 == std::string_view::npos) {
        return std::nullopt;
    }
    auto d2 = s.find('$', d1 + 1);
    if (d2 == std::string_view::npos) {
        return std::nullopt;
    }
    secret_record r;
    auto iters = s.substr(0, d1);
    if (iters.empty() || iters.size() > 9) {
        return std::nullopt;
    }
    std::uint32_t v = 0;
    for (char c : iters) {
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        v = v * 10 + static_cast<std::uint32_t>(c - '0');
    }
    if (v == 0) {
        return std::nullopt;
    }
    r._iterations = v;
    auto salt = b64_decode(s.substr(d1 + 1, d2 - d1 - 1));
    auto dk = b64_decode(s.substr(d2 + 1));
    if (!salt || !dk || salt->empty() || dk->empty()) {
        return std::nullopt;
    }
    r._salt = std::move(*salt);
    r._dk = std::move(*dk);
    return r;
}

[[nodiscard]] inline auto derive(std::string_view secret, const std::vector<unsigned char>& salt,
                                 std::uint32_t iterations, std::size_t dk_len)
    -> std::vector<unsigned char> {
    std::vector<unsigned char> dk(dk_len);
    if (PKCS5_PBKDF2_HMAC(secret.data(), static_cast<int>(secret.size()), salt.data(),
                          static_cast<int>(salt.size()), static_cast<int>(iterations), EVP_sha256(),
                          static_cast<int>(dk.size()), dk.data()) != 1) {
        throw std::runtime_error("PKCS5_PBKDF2_HMAC failed");
    }
    return dk;
}

}  // namespace redis_acl_detail

/// Default PBKDF2 iteration count for newly hashed secrets: OWASP's 2023
/// recommendation for PBKDF2-HMAC-SHA256. An AUTH costs this many HMACs,
/// which is why auth failures are rate-limited per source address.
inline constexpr std::uint32_t redis_acl_default_iterations = 600000;

/// Immutable snapshot of the users table. `redis_acl` swaps the whole thing
/// on reload so a connection mid-authentication sees one consistent table.
class redis_acl_table {
public:
    redis_acl_table() : redis_acl_table(std::vector<redis_acl_user>{}) {}
    explicit redis_acl_table(std::vector<redis_acl_user> users) : _users(std::move(users)) {
        // The decoy record an unknown user is verified against uses the
        // largest iteration count in the table, so a probe for a username
        // costs at least as much as a probe against a real one. Built here,
        // once per reload, rather than per failed AUTH.
        std::uint32_t iterations = 1000;
        for (const auto& u : _users) {
            if (auto rec = redis_acl_detail::parse_secret_record(u._secret)) {
                iterations = std::max(iterations, rec->_iterations);
            }
        }
        std::vector<unsigned char> salt(16, 0x5a);
        auto dk = redis_acl_detail::derive("kythira-decoy", salt, iterations, 32);
        _decoy_record = "pbkdf2-sha256$" + std::to_string(iterations) + "$" +
                        redis_acl_detail::b64_encode(salt) + "$" + redis_acl_detail::b64_encode(dk);
    }

    [[nodiscard]] auto decoy_record() const -> const std::string& { return _decoy_record; }

    [[nodiscard]] auto find(std::string_view name) const -> const redis_acl_user* {
        for (const auto& u : _users) {
            if (u._name == name) {
                return &u;
            }
        }
        return nullptr;
    }

    [[nodiscard]] auto find_by_cert_subject(std::string_view subject) const
        -> const redis_acl_user* {
        for (const auto& u : _users) {
            for (const auto& s : u._cert_subjects) {
                if (s == subject) {
                    return &u;
                }
            }
        }
        return nullptr;
    }

    [[nodiscard]] auto users() const -> const std::vector<redis_acl_user>& { return _users; }
    [[nodiscard]] auto empty() const noexcept -> bool { return _users.empty(); }

private:
    std::vector<redis_acl_user> _users;
    std::string _decoy_record;
};

class redis_acl {
public:
    redis_acl() : _table(std::make_shared<const redis_acl_table>()) {}
    explicit redis_acl(std::vector<redis_acl_user> users)
        : _table(std::make_shared<const redis_acl_table>(std::move(users))) {}

    // ---- secrets -----------------------------------------------------------

    /// Produce a `pbkdf2-sha256$...` record for `secret` with a fresh 16-byte
    /// salt. This is the only place secrets are hashed, so the daemon's
    /// `--hash-secret` and the tests agree on the format by construction.
    [[nodiscard]] static auto hash_secret(std::string_view secret,
                                          std::uint32_t iterations = redis_acl_default_iterations)
        -> std::string {
        std::vector<unsigned char> salt(16);
        if (RAND_bytes(salt.data(), static_cast<int>(salt.size())) != 1) {
            throw std::runtime_error("RAND_bytes failed");
        }
        auto dk = redis_acl_detail::derive(secret, salt, iterations, 32);
        return "pbkdf2-sha256$" + std::to_string(iterations) + "$" +
               redis_acl_detail::b64_encode(salt) + "$" + redis_acl_detail::b64_encode(dk);
    }

    /// Constant-time verification of `secret` against a record. Public so a
    /// unit test can check the record format without an ACL table.
    [[nodiscard]] static auto verify_secret(std::string_view record, std::string_view secret)
        -> bool {
        auto parsed = redis_acl_detail::parse_secret_record(record);
        if (!parsed) {
            return false;
        }
        auto dk = redis_acl_detail::derive(secret, parsed->_salt, parsed->_iterations,
                                           parsed->_dk.size());
        return CRYPTO_memcmp(dk.data(), parsed->_dk.data(), dk.size()) == 0;
    }

    // ---- authentication ----------------------------------------------------

    /// Returns an identity or nullopt. Unknown user, disabled user and wrong
    /// secret are indistinguishable to the caller — same result, and the
    /// same KDF work, so timing does not reveal which usernames exist.
    [[nodiscard]] auto authenticate(std::string_view user, std::string_view secret) const
        -> std::optional<redis_identity> {
        auto table = snapshot();
        const auto* u = table->find(user);
        bool ok = false;
        if (u != nullptr && u->_enabled) {
            if (u->_secret.empty()) {
                ok = true;  // nopass
            } else {
                ok = verify_secret(u->_secret, secret);
            }
        } else {
            // Burn the same KDF work as a real verification would.
            (void)verify_secret(table->decoy_record(), secret);
        }
        if (!ok) {
            return std::nullopt;
        }
        return redis_identity{u->_name, u->_role, u->_key_prefixes};
    }

    /// mTLS: map a verified client-certificate subject to a user.
    [[nodiscard]] auto authenticate_certificate(std::string_view subject) const
        -> std::optional<redis_identity> {
        auto table = snapshot();
        const auto* u = table->find_by_cert_subject(subject);
        if (u == nullptr || !u->_enabled) {
            return std::nullopt;
        }
        return redis_identity{u->_name, u->_role, u->_key_prefixes};
    }

    // ---- authorization -----------------------------------------------------

    /// `command` is the upper-cased command name; `keys` are every key the
    /// command touches (empty for key-less commands). A command the closure
    /// does not know is a command denial, which the gateway never reaches
    /// because it rejects unknown commands first.
    [[nodiscard]] static auto authorize(const redis_identity& id, std::string_view command,
                                        const std::vector<std::string_view>& keys) -> acl_decision {
        auto needed = redis_command_min_role(command);
        if (!needed.has_value() || static_cast<int>(id._role) < static_cast<int>(*needed)) {
            return acl_decision::deny_command;
        }
        for (auto key : keys) {
            if (!key_in_scope(id, key)) {
                return acl_decision::deny_key;
            }
        }
        return acl_decision::allow;
    }

    [[nodiscard]] static auto key_in_scope(const redis_identity& id, std::string_view key) -> bool {
        for (const auto& prefix : id._key_prefixes) {
            if (key.size() >= prefix.size() && key.substr(0, prefix.size()) == prefix) {
                return true;
            }
        }
        return false;
    }

    // ---- table management --------------------------------------------------

    /// Parse the ACL file format. Throws `redis_acl_parse_error` with the
    /// line number on any malformed line; nothing is installed on failure.
    [[nodiscard]] static auto parse(std::string_view text) -> std::vector<redis_acl_user> {
        std::vector<redis_acl_user> users;
        std::size_t line_no = 0;
        std::size_t pos = 0;
        while (pos <= text.size()) {
            auto nl = text.find('\n', pos);
            auto line =
                text.substr(pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos);
            pos = nl == std::string_view::npos ? text.size() + 1 : nl + 1;
            ++line_no;
            if (!line.empty() && line.back() == '\r') {
                line.remove_suffix(1);
            }
            auto words = redis_acl_detail::split_ws(line);
            if (words.empty() || words[0][0] == '#') {
                continue;
            }
            auto fail = [&](const std::string& why) {
                throw redis_acl_parse_error("acl line " + std::to_string(line_no) + ": " + why);
            };
            if (words[0] != "user") {
                fail("expected 'user'");
            }
            if (words.size() < 4) {
                fail("expected: user <name> <secret|nopass> <role> [prefix ...] [cert=<subject>]");
            }
            redis_acl_user u;
            u._name = words[1];
            if (u._name.empty() || u._name == "nopass") {
                fail("invalid user name");
            }
            for (const auto& existing : users) {
                if (existing._name == u._name) {
                    fail("duplicate user '" + u._name + "'");
                }
            }
            if (words[2] == "nopass") {
                u._secret.clear();
            } else if (words[2] == "disabled") {
                u._enabled = false;
            } else if (redis_acl_detail::parse_secret_record(words[2]).has_value()) {
                u._secret = words[2];
            } else {
                fail("secret must be 'nopass', 'disabled' or a pbkdf2-sha256$... record");
            }
            auto role = parse_redis_role(words[3]);
            if (!role.has_value()) {
                fail("unknown role '" + words[3] + "' (read_only|read_write|admin)");
            }
            u._role = *role;
            for (std::size_t i = 4; i < words.size(); ++i) {
                const auto& w = words[i];
                if (w.rfind("cert=", 0) == 0) {
                    auto subject = w.substr(5);
                    if (subject.empty()) {
                        fail("empty cert= subject");
                    }
                    u._cert_subjects.push_back(subject);
                } else if (w == "*") {
                    u._key_prefixes.emplace_back("");
                } else {
                    u._key_prefixes.push_back(w);
                }
            }
            users.push_back(std::move(u));
        }
        return users;
    }

    /// Replace the table atomically. A parse failure throws and keeps the
    /// prior table in force, so a bad edit cannot lock everyone out.
    auto reload(std::string_view text) -> std::size_t {
        auto users = parse(text);
        auto fresh = std::make_shared<const redis_acl_table>(std::move(users));
        std::lock_guard<std::mutex> lock(_mutex);
        _table = std::move(fresh);
        return _table->users().size();
    }

    auto reload_file(const std::string& path) -> std::size_t {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            throw redis_acl_parse_error("cannot open acl file '" + path + "'");
        }
        std::ostringstream ss;
        ss << in.rdbuf();
        return reload(ss.str());
    }

    [[nodiscard]] auto snapshot() const -> std::shared_ptr<const redis_acl_table> {
        std::lock_guard<std::mutex> lock(_mutex);
        return _table;
    }

    [[nodiscard]] auto empty() const -> bool { return snapshot()->empty(); }

private:
    mutable std::mutex _mutex;
    std::shared_ptr<const redis_acl_table> _table;
};

}  // namespace kythira
