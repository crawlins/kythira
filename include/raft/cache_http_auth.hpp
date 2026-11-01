// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file cache_http_auth.hpp
/// @brief HTTP credentials for the cache front end, checked against the RESP
///        listener's `redis_acl`, with a cache of successes
///        (.kiro/specs/disk-backed-build-cache/ design Component 7,
///        Requirement 8).
///
/// ACL secrets are PBKDF2 records at 600,000 iterations. RESP pays that once
/// per connection; HTTP clients send credentials on every request, and
/// sccache opens a connection per compile. So a success is remembered for
/// `ttl` under `HMAC-SHA256(process key, scheme ‖ user ‖ secret)`. The key
/// is random per process and the secret itself is never stored, so a heap
/// dump of the cache gives an attacker nothing to replay. Failures are not
/// cached; the caller rate-limits them with `redis_auth_limiter`.
///
/// The cache remembers which ACL table each entry was checked against and
/// empties itself the first time it sees a different one, so `reload()` of
/// the ACL file (a revoked user, a rotated secret) takes effect on the next
/// request rather than after `ttl`.

#include <raft/redis_acl.hpp>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#include <array>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace kythira {

/// A parsed `Authorization` header.
struct cache_http_credentials {
    enum class scheme : std::uint8_t {
        basic,
        bearer
    };
    scheme _scheme = scheme::basic;
    std::string _user;  ///< empty for a bearer token
    std::string _secret;
};

/// `Basic <base64(user:secret)>` or `Bearer <token>`; `nullopt` for anything
/// else, including an empty or malformed header. Scheme names are
/// case-insensitive (RFC 9110 section 11.1).
[[nodiscard]] inline auto parse_cache_http_authorization(std::string_view header)
    -> std::optional<cache_http_credentials> {
    auto space = header.find(' ');
    if (space == std::string_view::npos) {
        return std::nullopt;
    }
    std::string scheme(header.substr(0, space));
    for (auto& c : scheme) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    auto rest = header.substr(space + 1);
    while (!rest.empty() && rest.front() == ' ') {
        rest.remove_prefix(1);
    }
    if (rest.empty()) {
        return std::nullopt;
    }
    if (scheme == "bearer") {
        return cache_http_credentials{
            cache_http_credentials::scheme::bearer, {}, std::string(rest)};
    }
    if (scheme != "basic") {
        return std::nullopt;
    }
    auto decoded = redis_acl_detail::b64_decode(rest);
    if (!decoded) {
        return std::nullopt;
    }
    std::string text(decoded->begin(), decoded->end());
    auto colon = text.find(':');
    if (colon == std::string::npos || colon == 0) {
        return std::nullopt;
    }
    return cache_http_credentials{cache_http_credentials::scheme::basic, text.substr(0, colon),
                                  text.substr(colon + 1)};
}

/// Constant-time string equality, for comparing presented secrets.
[[nodiscard]] inline auto cache_http_secret_equal(std::string_view a, std::string_view b) -> bool {
    return a.size() == b.size() && (a.empty() || CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0);
}

class cache_http_auth_cache {
public:
    using clock = std::chrono::steady_clock;
    using digest = std::array<unsigned char, 32>;

    cache_http_auth_cache(std::chrono::seconds ttl, std::size_t max_entries)
        : _ttl(ttl), _max_entries(max_entries == 0 ? 1 : max_entries) {
        if (RAND_bytes(_key.data(), static_cast<int>(_key.size())) != 1) {
            throw std::runtime_error("cache_http_auth_cache: RAND_bytes failed");
        }
    }

    /// Authenticate `creds` against `acl`, from the cache when it can. On a
    /// miss, `verify` is told and must return the identity or `nullopt`; it
    /// is where the caller runs the KDF under its rate limiter. A `verify`
    /// that throws leaves the cache unchanged.
    template<typename Verify>
    auto authenticate(const redis_acl& acl, const cache_http_credentials& creds, Verify&& verify,
                      clock::time_point now = clock::now()) -> std::optional<redis_identity> {
        const auto table = acl.snapshot();
        const auto key = digest_of(creds);
        if (auto hit = lookup(table, key, now)) {
            return hit;
        }
        std::optional<redis_identity> id = verify();
        if (id) {
            insert(table, key, *id, now);
        }
        return id;
    }

    [[nodiscard]] auto size() const -> std::size_t {
        const std::lock_guard lock(_mu);
        return _entries.size();
    }
    [[nodiscard]] auto hits() const -> std::uint64_t {
        const std::lock_guard lock(_mu);
        return _hits;
    }
    [[nodiscard]] auto flushes() const -> std::uint64_t {
        const std::lock_guard lock(_mu);
        return _flushes;
    }

    /// Bearer tokens name no user, so the ACL is searched: each enabled
    /// user's record is tried in turn. That is one KDF per user, which is why
    /// Basic is the better choice with more than a handful of users.
    [[nodiscard]] static auto verify_bearer(const redis_acl& acl, std::string_view token)
        -> std::optional<redis_identity> {
        const auto table = acl.snapshot();
        for (const auto& u : table->users()) {
            if (u._enabled && !u._secret.empty() && redis_acl::verify_secret(u._secret, token)) {
                return redis_identity{u._name, u._role, u._key_prefixes};
            }
        }
        // Pay one more derivation so a miss costs no less than a late match.
        (void)redis_acl::verify_secret(table->decoy_record(), token);
        return std::nullopt;
    }

private:
    struct entry {
        redis_identity _id;
        clock::time_point _expires;
    };

    [[nodiscard]] auto digest_of(const cache_http_credentials& creds) const -> digest {
        // Length-prefixed fields, so ("ab", "c") and ("a", "bc") differ.
        std::string material;
        material.push_back(creds._scheme == cache_http_credentials::scheme::basic ? 'B' : 'T');
        auto put = [&material](std::string_view s) {
            const auto n = static_cast<std::uint32_t>(s.size());
            for (int shift = 24; shift >= 0; shift -= 8) {
                material.push_back(static_cast<char>((n >> static_cast<unsigned>(shift)) & 0xffU));
            }
            material.append(s);
        };
        put(creds._user);
        put(creds._secret);
        digest out{};
        unsigned int len = 0;
        if (HMAC(EVP_sha256(), _key.data(), static_cast<int>(_key.size()),
                 reinterpret_cast<const unsigned char*>(material.data()), material.size(),
                 out.data(), &len) == nullptr ||
            len != out.size()) {
            OPENSSL_cleanse(material.data(), material.size());
            throw std::runtime_error("cache_http_auth_cache: HMAC failed");
        }
        OPENSSL_cleanse(material.data(), material.size());
        return out;
    }

    using table_ptr = std::shared_ptr<const redis_acl_table>;

    auto lookup(const table_ptr& table, const digest& key, clock::time_point now)
        -> std::optional<redis_identity> {
        const std::lock_guard lock(_mu);
        check_table(table);
        auto it = _entries.find(key);
        if (it == _entries.end()) {
            return std::nullopt;
        }
        if (it->second._expires <= now) {
            _entries.erase(it);
            return std::nullopt;
        }
        ++_hits;
        return it->second._id;
    }

    void insert(const table_ptr& table, const digest& key, const redis_identity& id,
                clock::time_point now) {
        const std::lock_guard lock(_mu);
        check_table(table);
        if (_entries.size() >= _max_entries && !_entries.contains(key)) {
            // Drop the expired first, then the entry closest to expiring.
            std::erase_if(_entries, [now](const auto& e) { return e.second._expires <= now; });
            if (_entries.size() >= _max_entries) {
                auto oldest = _entries.begin();
                for (auto it = _entries.begin(); it != _entries.end(); ++it) {
                    if (it->second._expires < oldest->second._expires) {
                        oldest = it;
                    }
                }
                _entries.erase(oldest);
            }
        }
        _entries[key] = entry{id, now + _ttl};
    }

    /// Called under `_mu`. The table changes only on `reload()`. Holding
    /// the one the entries were checked against keeps its address from
    /// being reused by a later table, which would hide a reload.
    void check_table(const table_ptr& table) {
        if (table != _table) {
            if (_table != nullptr) {
                ++_flushes;
            }
            _entries.clear();
            _table = table;
        }
    }

    std::chrono::seconds _ttl;
    std::size_t _max_entries;
    std::array<unsigned char, 32> _key{};
    mutable std::mutex _mu;
    table_ptr _table;
    std::map<digest, entry> _entries;
    std::uint64_t _hits = 0;
    std::uint64_t _flushes = 0;
};

}  // namespace kythira
