// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file iam_state_machine.hpp
/// @brief The IAM Raft group's `state_machine`
///        (`.kiro/specs/iam-compatible-auth/` design Component 4,
///        Requirement 4).
///
/// Modelled on `ca_state_machine`: each command is a Boost.JSON object whose
/// `"type"` is an `iam_command`. The leader validates a request (names,
/// policy grammar, referenced entities), then stamps everything that is not
/// deterministic into the command before proposing it: the time (`"now"`),
/// generated unique ids, and sealed secrets. `apply()` only records those
/// facts. It re-checks the preconditions two concurrent requests can race on
/// (name uniqueness, `DeleteConflict`, quota counts) and answers an error
/// rather than mutating, so every replica reaches the same state from the
/// same log.
///
/// Every command returns JSON result bytes: `{"error": <AWS code>,
/// "message": ...}` on a refusal, otherwise an object describing what was
/// created or changed, which the Query handlers (task 14) render as XML.
///
/// Entities live in ordered maps keyed by lower-cased name, so names are
/// unique case-insensitively (as on AWS) and `get_state()` is byte-for-byte
/// identical on every replica. The access-key index, the authentication hot
/// path, is a hash map rebuilt on restore.
///
/// No plaintext secret ever enters a command or a snapshot: access-key and
/// STS-key secrets arrive sealed (`iam_seal.hpp`), with the record's id as
/// associated data.

#include <kythira/iam/aws_managed_policies_data.hpp>
#include <kythira/iam/iam_policy.hpp>
#include <kythira/iam/iam_seal.hpp>
#include <raft/types.hpp>

#include <boost/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace kythira::iam {

// ── Commands ────────────────────────────────────────────────────────────────

/// The command set of design Component 4. Values are part of the log format:
/// append only, never renumber.
enum class iam_command : std::uint8_t {
    noop = 0,
    // Accounts
    create_account = 1,
    set_account_alias = 2,
    // Users
    create_user = 10,
    update_user = 11,
    delete_user = 12,
    tag_entity = 13,
    untag_entity = 14,
    // User groups
    create_group = 20,
    update_group = 21,
    delete_group = 22,
    add_user_to_group = 23,
    remove_user_from_group = 24,
    // Roles
    create_role = 30,
    update_role = 31,
    update_trust_policy = 32,
    delete_role = 33,
    // Instance profiles
    create_instance_profile = 40,
    delete_instance_profile = 41,
    add_role_to_instance_profile = 42,
    remove_role_from_instance_profile = 43,
    // Policies
    create_policy = 50,
    create_policy_version = 51,
    delete_policy_version = 52,
    set_default_version = 53,
    delete_policy = 54,
    attach_policy = 55,
    detach_policy = 56,
    put_inline_policy = 57,
    delete_inline_policy = 58,
    // Permissions boundaries
    put_boundary = 60,
    delete_boundary = 61,
    // Access keys
    create_access_key = 70,
    update_access_key = 71,
    delete_access_key = 72,
    record_key_last_used = 73,
    // OIDC providers
    create_oidc_provider = 80,
    update_oidc_provider = 81,
    delete_oidc_provider = 82,
    // STS sealing keys
    record_sts_key = 90,
    retire_sts_key = 91,
    // Read replicas (iam_node `replica-add`): where a learner's Raft RPC
    // listener is, so every member can reach it once it leads.
    register_replica = 100,
    remove_replica = 101,
};

/// AWS's IAM error codes, as the Query API returns them.
namespace iam_errors {
inline constexpr std::string_view no_such_entity = "NoSuchEntity";
inline constexpr std::string_view entity_already_exists = "EntityAlreadyExists";
inline constexpr std::string_view delete_conflict = "DeleteConflict";
inline constexpr std::string_view limit_exceeded = "LimitExceeded";
inline constexpr std::string_view malformed_policy = "MalformedPolicyDocument";
inline constexpr std::string_view invalid_input = "InvalidInput";
}  // namespace iam_errors

/// Quotas (Requirement 4.4). AWS's defaults; configurable upward. Every
/// replica must run with the same values, since `apply()` enforces them.
struct iam_limits {
    std::size_t managed_policy_size = 6144;
    std::size_t trust_policy_size = 2048;
    std::size_t user_inline_total = 2048;
    std::size_t group_inline_total = 5120;
    std::size_t role_inline_total = 10240;
    std::size_t attached_per_entity = 10;
    std::size_t policy_versions = 5;
    std::size_t access_keys_per_user = 2;
    std::size_t groups_per_user = 10;
    std::size_t roles_per_instance_profile = 1;
    std::size_t tags_per_entity = 50;
    std::size_t client_ids_per_oidc_provider = 100;
    std::size_t thumbprints_per_oidc_provider = 5;
};

struct iam_config {
    std::string partition = "aws";
    iam_limits limits{};
};

// ── State ───────────────────────────────────────────────────────────────────

using tag_map = std::map<std::string, std::string>;

struct inline_policy {
    std::string name;
    std::string document;
};

struct iam_user {
    std::string name;
    std::string path = "/";
    std::string id;
    std::int64_t create_date = 0;
    tag_map tags;
    std::map<std::string, inline_policy> inline_policies;  // by lower-cased name
    std::set<std::string> attached;                        // policy ARNs
    std::optional<std::string> boundary;                   // policy ARN
    std::set<std::string> groups;                          // lower-cased group names
};

struct iam_group {
    std::string name;
    std::string path = "/";
    std::string id;
    std::int64_t create_date = 0;
    std::map<std::string, inline_policy> inline_policies;
    std::set<std::string> attached;
    std::set<std::string> members;  // lower-cased user names
};

struct iam_role {
    std::string name;
    std::string path = "/";
    std::string id;
    std::int64_t create_date = 0;
    std::string trust_policy;
    std::string description;
    std::int64_t max_session_duration = 3600;
    tag_map tags;
    std::map<std::string, inline_policy> inline_policies;
    std::set<std::string> attached;
    std::optional<std::string> boundary;
    std::set<std::string> instance_profiles;  // lower-cased names
};

struct iam_instance_profile {
    std::string name;
    std::string path = "/";
    std::string id;
    std::int64_t create_date = 0;
    tag_map tags;
    std::set<std::string> roles;  // lower-cased role names
};

struct policy_version {
    std::string document;
    std::int64_t create_date = 0;
};

struct iam_managed_policy {
    std::string name;
    std::string path = "/";
    std::string id;
    std::int64_t create_date = 0;
    std::int64_t update_date = 0;
    std::string description;
    tag_map tags;
    int default_version = 1;
    int next_version = 2;
    std::map<int, policy_version> versions;
    std::size_t attachment_count = 0;
    std::size_t boundary_usage_count = 0;
};

struct key_last_used {
    std::int64_t time = 0;
    std::string service;
    std::string region;
};

struct iam_access_key {
    std::string id;
    std::string owner;  // lower-cased user name; empty for the account root
    std::string sealed_secret;
    bool active = true;
    std::int64_t create_date = 0;
    std::optional<key_last_used> last_used;
};

struct iam_oidc_provider {
    std::string url;  // without the scheme, as in the ARN
    std::vector<std::string> client_ids;
    std::vector<std::string> thumbprints;
    std::int64_t create_date = 0;
    tag_map tags;
};

struct iam_account {
    std::string id;  // 12 digits
    std::string alias;
    std::int64_t create_date = 0;
    std::map<std::string, iam_user> users;
    std::map<std::string, iam_group> groups;
    std::map<std::string, iam_role> roles;
    std::map<std::string, iam_instance_profile> instance_profiles;
    std::map<std::string, iam_managed_policy> policies;
    std::map<std::string, iam_access_key> access_keys;        // by key id (case kept)
    std::map<std::string, iam_oidc_provider> oidc_providers;  // by url
};

/// A read replica: a Raft learner and the address of its RPC listener. The
/// node id is kept as its canonical text, whatever the build's id type.
struct replica_record {
    std::string node_id;
    std::string rpc_host;
    std::uint16_t rpc_port = 0;
    std::int64_t create_date = 0;
    auto operator==(const replica_record&) const -> bool = default;
};

struct sts_key_record {
    std::string kid;
    std::string sealed_key;
    std::int64_t create_date = 0;
    std::optional<std::int64_t> retire_after;
};

/// Where an access key lives: the authentication hot path's index entry.
struct access_key_location {
    std::string account;
    std::string key_id;
};

// ── Helpers shared by the leader and apply() ─────────────────────────────────

namespace iam_detail {

[[nodiscard]] inline auto lower(std::string_view s) -> std::string {
    std::string out(s);
    std::ranges::transform(out, out.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

[[nodiscard]] inline auto bytes_of(const boost::json::value& v) -> std::vector<std::byte> {
    const auto s = boost::json::serialize(v);
    std::vector<std::byte> out(s.size());
    std::ranges::transform(s, out.begin(), [](char c) { return static_cast<std::byte>(c); });
    return out;
}

[[nodiscard]] inline auto parse_bytes(const std::vector<std::byte>& bytes) -> boost::json::value {
    const std::string_view text(
        reinterpret_cast<const char*>(
            bytes.data()),  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
        bytes.size());
    return boost::json::parse(text);
}

[[nodiscard]] inline auto str(const boost::json::object& o, std::string_view key) -> std::string {
    return std::string(o.at(key).as_string());
}

[[nodiscard]] inline auto opt_str(const boost::json::object& o, std::string_view key)
    -> std::optional<std::string> {
    if (const auto* v = o.if_contains(key); v != nullptr && !v->is_null()) {
        return std::string(v->as_string());
    }
    return std::nullopt;
}

[[nodiscard]] inline auto i64(const boost::json::object& o, std::string_view key) -> std::int64_t {
    return o.at(key).to_number<std::int64_t>();
}

[[nodiscard]] inline auto tags_from(const boost::json::value* v) -> tag_map {
    tag_map out;
    if (v == nullptr) {
        return out;
    }
    for (const auto& t : v->as_array()) {
        const auto& o = t.as_object();
        out[str(o, "Key")] = str(o, "Value");
    }
    return out;
}

[[nodiscard]] inline auto tags_to(const tag_map& tags) -> boost::json::array {
    boost::json::array out;
    for (const auto& [k, v] : tags) {
        out.push_back(boost::json::object{{"Key", k}, {"Value", v}});
    }
    return out;
}

template<class Set> [[nodiscard]] auto strings_to(const Set& s) -> boost::json::array {
    boost::json::array out;
    for (const auto& e : s) {
        out.emplace_back(e);
    }
    return out;
}

[[nodiscard]] inline auto string_set(const boost::json::value* v) -> std::set<std::string> {
    std::set<std::string> out;
    if (v != nullptr) {
        for (const auto& e : v->as_array()) {
            out.emplace(e.as_string());
        }
    }
    return out;
}

[[nodiscard]] inline auto string_vec(const boost::json::value* v) -> std::vector<std::string> {
    std::vector<std::string> out;
    if (v != nullptr) {
        for (const auto& e : v->as_array()) {
            out.emplace_back(e.as_string());
        }
    }
    return out;
}

inline auto inline_to(const std::map<std::string, inline_policy>& m) -> boost::json::array {
    boost::json::array out;
    for (const auto& [_, p] : m) {
        out.push_back(boost::json::object{{"name", p.name}, {"document", p.document}});
    }
    return out;
}

inline auto inline_from(const boost::json::value* v) -> std::map<std::string, inline_policy> {
    std::map<std::string, inline_policy> out;
    if (v != nullptr) {
        for (const auto& e : v->as_array()) {
            const auto& o = e.as_object();
            inline_policy p{str(o, "name"), str(o, "document")};
            out.emplace(lower(p.name), std::move(p));
        }
    }
    return out;
}

[[nodiscard]] inline auto inline_total(const std::map<std::string, inline_policy>& m)
    -> std::size_t {
    std::size_t n = 0;
    for (const auto& [_, p] : m) {
        n += detail::policy_size(p.document);
    }
    return n;
}

/// The policy's version id as AWS spells it, "v<n>".
[[nodiscard]] inline auto version_id(int n) -> std::string {
    return "v" + std::to_string(n);
}

[[nodiscard]] inline auto parse_version_id(std::string_view s) -> std::optional<int> {
    if (s.size() < 2 || (s[0] != 'v' && s[0] != 'V') || s.size() > 6) {
        return std::nullopt;
    }
    int n = 0;
    for (char c : s.substr(1)) {
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        n = n * 10 + (c - '0');
    }
    return n > 0 ? std::optional<int>(n) : std::nullopt;
}

}  // namespace iam_detail

/// The bundled AWS-managed policies (Requirement 4.3), read-only.
struct aws_managed_policy {
    std::string_view name;
    std::string_view path;
    std::string_view version_id;
    std::string_view document;
};

[[nodiscard]] inline auto find_aws_managed_policy(std::string_view name)
    -> std::optional<aws_managed_policy> {
    for (const auto& e : aws_managed_policy_data) {
        if (iam_detail::lower(e.name) == iam_detail::lower(name)) {
            return aws_managed_policy{e.name, e.path, e.version_id, e.document};
        }
    }
    return std::nullopt;
}

/// A digest of the bundled AWS-managed policy set (names, versions and
/// documents), recorded in every snapshot (design Component 4, "Its version
/// is part of the snapshot header"). Members built from different
/// `aws_managed_policies.json` files would evaluate different text; a
/// restored snapshot whose digest differs is accepted, so a rolling upgrade
/// can proceed, and reported through `aws_managed_mismatch()` for the node
/// to alarm on.
[[nodiscard]] inline auto aws_managed_digest() -> const std::string& {
    static const std::string digest = [] {
        std::string text;
        for (const auto& e : aws_managed_policy_data) {
            for (auto part : {e.name, e.path, e.version_id, e.document}) {
                text += part;
                text += '\0';
            }
        }
        std::array<unsigned char, EVP_MAX_MD_SIZE> md{};
        unsigned int len = 0;
        if (EVP_Digest(text.data(), text.size(), md.data(), &len, EVP_sha256(), nullptr) != 1) {
            throw std::runtime_error("iam: SHA-256 failed");
        }
        static constexpr std::string_view hex = "0123456789abcdef";
        std::string out;
        for (unsigned int i = 0; i < len; ++i) {
            out += hex[md[i] >> 4U];
            out += hex[md[i] & 0x0FU];
        }
        return out;
    }();
    return digest;
}

// ── The state machine ───────────────────────────────────────────────────────

class iam_state_machine {
public:
    /// The snapshot format this build writes. Older formats it can read are
    /// listed in `restore_from_snapshot()`; a newer one is refused.
    static constexpr int snapshot_format = 1;

    explicit iam_state_machine(iam_config config = {}) : _config(std::move(config)) {}

    // Copyable and movable, so a read can hand out a restored copy. The
    // parsed-policy cache comes along under the source's lock: its entries
    // are immutable and keyed by ARN and version, so sharing them is safe.
    iam_state_machine(const iam_state_machine& o) { copy_from(o); }
    iam_state_machine(iam_state_machine&& o) noexcept { move_from(std::move(o)); }
    auto operator=(const iam_state_machine& o) -> iam_state_machine& {
        if (this != &o) {
            copy_from(o);
        }
        return *this;
    }
    auto operator=(iam_state_machine&& o) noexcept -> iam_state_machine& {
        if (this != &o) {
            move_from(std::move(o));
        }
        return *this;
    }
    ~iam_state_machine() = default;

    auto apply(const std::vector<std::byte>& command, std::uint64_t index)
        -> std::vector<std::byte> {
        _last_applied = index;
        boost::json::object cmd;
        try {
            cmd = iam_detail::parse_bytes(command).as_object();
        } catch (const std::exception& e) {
            throw std::invalid_argument(std::string("iam_state_machine: malformed command: ") +
                                        e.what());
        }
        const auto* type = cmd.if_contains("type");
        if (type == nullptr) {
            throw std::invalid_argument("iam_state_machine: command missing \"type\"");
        }
        try {
            return dispatch(static_cast<iam_command>(type->to_number<int>()), cmd);
        } catch (const std::invalid_argument&) {
            throw;
        } catch (const std::exception& e) {
            // A missing or mistyped field: the leader never proposes one.
            throw std::invalid_argument(std::string("iam_state_machine: bad command body: ") +
                                        e.what());
        }
    }

    [[nodiscard]] auto get_state() const -> std::vector<std::byte> {
        boost::json::object o;
        o["format"] = snapshot_format;
        o["partition"] = _config.partition;
        o["kek_salt"] = _kek_salt;
        boost::json::array accounts;
        for (const auto& [_, a] : _accounts) {
            accounts.push_back(account_to_json(a));
        }
        o["accounts"] = std::move(accounts);
        boost::json::array sts;
        for (const auto& k : _sts_keys) {
            boost::json::object e{
                {"kid", k.kid}, {"sealed_key", k.sealed_key}, {"create_date", k.create_date}};
            if (k.retire_after) {
                e["retire_after"] = *k.retire_after;
            }
            sts.push_back(std::move(e));
        }
        o["sts_keys"] = std::move(sts);
        o["sts_clock"] = _sts_clock;
        o["aws_managed"] = aws_managed_digest();
        boost::json::array replicas;
        for (const auto& [_, r] : _replicas) {
            replicas.push_back(boost::json::object{{"node_id", r.node_id},
                                                   {"rpc_host", r.rpc_host},
                                                   {"rpc_port", r.rpc_port},
                                                   {"create_date", r.create_date}});
        }
        o["replicas"] = std::move(replicas);
        return iam_detail::bytes_of(o);
    }

    /// Replaces this state with a snapshot's. The snapshot is parsed in full
    /// into fresh structures first, so a malformed or unknown-format
    /// snapshot throws `std::invalid_argument` and leaves the current state
    /// untouched. Every structural invariant `apply()` maintains (the
    /// access-key index, group membership on both sides, attachment counts)
    /// is rebuilt or checked here rather than trusted.
    auto restore_from_snapshot(const std::vector<std::byte>& snapshot, std::uint64_t index)
        -> void {
        std::map<std::string, iam_account> accounts;
        std::string salt;
        std::vector<sts_key_record> sts;
        std::int64_t clock = 0;
        std::string managed;
        std::map<std::string, replica_record> replicas;
        if (!snapshot.empty()) {
            try {
                const auto o = iam_detail::parse_bytes(snapshot).as_object();
                const auto format = o.at("format").to_number<int>();
                if (format != snapshot_format) {
                    throw std::invalid_argument("unsupported snapshot format " +
                                                std::to_string(format));
                }
                if (iam_detail::str(o, "partition") != _config.partition) {
                    throw std::invalid_argument("snapshot is for another partition");
                }
                salt = iam_detail::str(o, "kek_salt");
                for (const auto& a : o.at("accounts").as_array()) {
                    auto acct = account_from_json(a.as_object());
                    auto id = acct.id;
                    if (!accounts.emplace(id, std::move(acct)).second) {
                        throw std::invalid_argument("duplicate account " + id);
                    }
                }
                for (const auto& e : o.at("sts_keys").as_array()) {
                    const auto& k = e.as_object();
                    sts_key_record r{iam_detail::str(k, "kid"), iam_detail::str(k, "sealed_key"),
                                     iam_detail::i64(k, "create_date"), std::nullopt};
                    if (const auto* ra = k.if_contains("retire_after")) {
                        r.retire_after = ra->to_number<std::int64_t>();
                    }
                    sts.push_back(std::move(r));
                }
                clock = iam_detail::i64(o, "sts_clock");
                managed = iam_detail::str(o, "aws_managed");
                // Absent from snapshots written before replicas existed.
                if (const auto* rs = o.if_contains("replicas")) {
                    for (const auto& e : rs->as_array()) {
                        auto r = replica_from_json(e.as_object());
                        auto id = r.node_id;
                        if (!replicas.emplace(id, std::move(r)).second) {
                            throw std::invalid_argument("duplicate replica " + id);
                        }
                    }
                }
                check_invariants(accounts);
            } catch (const std::invalid_argument& e) {
                throw std::invalid_argument(std::string("iam_state_machine: snapshot: ") +
                                            e.what());
            } catch (const std::exception& e) {
                throw std::invalid_argument(std::string("iam_state_machine: snapshot: ") +
                                            e.what());
            }
        }
        _accounts = std::move(accounts);
        _kek_salt = std::move(salt);
        _sts_keys = std::move(sts);
        _sts_clock = clock;
        _replicas = std::move(replicas);
        _aws_managed_mismatch = !snapshot.empty() && managed != aws_managed_digest();
        rebuild_key_index();
        {
            const std::lock_guard lock(_compiled_mutex);
            _compiled.clear();
        }
        _last_applied = index;
    }

    // ── Read access (front ends, the Query handlers, the evaluator) ─────────

    [[nodiscard]] auto config() const noexcept -> const iam_config& { return _config; }
    [[nodiscard]] auto kek_salt() const noexcept -> const std::string& { return _kek_salt; }
    /// Whether the last restored snapshot came from a member bundling a
    /// different AWS-managed policy set (see `aws_managed_digest()`).
    [[nodiscard]] auto aws_managed_mismatch() const noexcept -> bool {
        return _aws_managed_mismatch;
    }
    [[nodiscard]] auto last_applied() const noexcept -> std::uint64_t { return _last_applied; }

    [[nodiscard]] auto account(std::string_view id) const -> const iam_account* {
        const auto it = _accounts.find(std::string(id));
        return it == _accounts.end() ? nullptr : &it->second;
    }

    [[nodiscard]] auto accounts() const noexcept -> const std::map<std::string, iam_account>& {
        return _accounts;
    }

    [[nodiscard]] auto user(std::string_view acct, std::string_view name) const -> const iam_user* {
        return find_in(acct, &iam_account::users, name);
    }
    [[nodiscard]] auto group(std::string_view acct, std::string_view name) const
        -> const iam_group* {
        return find_in(acct, &iam_account::groups, name);
    }
    [[nodiscard]] auto role(std::string_view acct, std::string_view name) const -> const iam_role* {
        return find_in(acct, &iam_account::roles, name);
    }
    [[nodiscard]] auto instance_profile(std::string_view acct, std::string_view name) const
        -> const iam_instance_profile* {
        return find_in(acct, &iam_account::instance_profiles, name);
    }
    [[nodiscard]] auto managed_policy(std::string_view acct, std::string_view name) const
        -> const iam_managed_policy* {
        return find_in(acct, &iam_account::policies, name);
    }

    /// The access key with this id, and its account: one hash lookup.
    [[nodiscard]] auto access_key(std::string_view key_id) const
        -> std::pair<const iam_account*, const iam_access_key*> {
        const auto it = _key_index.find(std::string(key_id));
        if (it == _key_index.end()) {
            return {nullptr, nullptr};
        }
        const auto& acct = _accounts.at(it->second);
        return {&acct, &acct.access_keys.at(std::string(key_id))};
    }

    [[nodiscard]] auto sts_keys() const noexcept -> const std::vector<sts_key_record>& {
        return _sts_keys;
    }

    /// Registered read replicas, by node id text.
    [[nodiscard]] auto replicas() const noexcept -> const std::map<std::string, replica_record>& {
        return _replicas;
    }

    /// The parsed form of a managed policy version (customer or AWS
    /// managed), cached beside its JSON and never serialised. Returns null
    /// for an unknown ARN or version.
    [[nodiscard]] auto compiled_policy(std::string_view arn, int version = 0) const
        -> std::shared_ptr<const policy_document> {
        const auto doc = policy_text(arn, version);
        if (!doc) {
            return nullptr;
        }
        const auto key = std::string(arn) + "#" + std::to_string(doc->second);
        const std::lock_guard lock(_compiled_mutex);
        if (const auto it = _compiled.find(key); it != _compiled.end()) {
            return it->second;
        }
        auto parsed = std::make_shared<const policy_document>(parse_policy(
            doc->first, policy_type::identity,
            {.max_size =
                 std::max(_config.limits.managed_policy_size, detail::policy_size(doc->first))}));
        _compiled.emplace(key, parsed);
        return parsed;
    }

    /// ARNs, built the way AWS spells them.
    [[nodiscard]] auto arn(std::string_view acct, std::string_view kind, std::string_view path,
                           std::string_view name) const -> std::string {
        return "arn:" + _config.partition + ":iam::" + std::string(acct) + ":" + std::string(kind) +
               std::string(path) + std::string(name);
    }

private:
    using result = std::vector<std::byte>;

    iam_config _config;
    std::map<std::string, iam_account> _accounts;
    std::string _kek_salt;
    std::vector<sts_key_record> _sts_keys;
    std::int64_t _sts_clock = 0;
    std::map<std::string, replica_record> _replicas;
    bool _aws_managed_mismatch = false;
    std::unordered_map<std::string, std::string> _key_index;  // key id → account id
    std::uint64_t _last_applied = 0;
    mutable std::mutex _compiled_mutex;
    mutable std::unordered_map<std::string, std::shared_ptr<const policy_document>> _compiled;

    void copy_from(const iam_state_machine& o) {
        _config = o._config;
        _accounts = o._accounts;
        _kek_salt = o._kek_salt;
        _sts_keys = o._sts_keys;
        _sts_clock = o._sts_clock;
        _replicas = o._replicas;
        _aws_managed_mismatch = o._aws_managed_mismatch;
        _key_index = o._key_index;
        _last_applied = o._last_applied;
        std::unordered_map<std::string, std::shared_ptr<const policy_document>> compiled;
        {
            const std::lock_guard lock(o._compiled_mutex);
            compiled = o._compiled;
        }
        const std::lock_guard lock(_compiled_mutex);
        _compiled = std::move(compiled);
    }

    void move_from(iam_state_machine&& o) noexcept {
        _config = std::move(o._config);
        _accounts = std::move(o._accounts);
        _kek_salt = std::move(o._kek_salt);
        _sts_keys = std::move(o._sts_keys);
        _sts_clock = o._sts_clock;
        _replicas = std::move(o._replicas);
        _aws_managed_mismatch = o._aws_managed_mismatch;
        _key_index = std::move(o._key_index);
        _last_applied = o._last_applied;
        std::unordered_map<std::string, std::shared_ptr<const policy_document>> compiled;
        {
            const std::lock_guard lock(o._compiled_mutex);
            compiled = std::move(o._compiled);
        }
        const std::lock_guard lock(_compiled_mutex);
        _compiled = std::move(compiled);
    }

    // ── results ──

    [[nodiscard]] static auto error(std::string_view code, const std::string& message) -> result {
        return iam_detail::bytes_of(boost::json::object{{"error", code}, {"message", message}});
    }

    [[nodiscard]] static auto ok(boost::json::object o = {}) -> result {
        return iam_detail::bytes_of(o);
    }

    [[nodiscard]] static auto no_such(std::string_view what, std::string_view name) -> result {
        return error(iam_errors::no_such_entity, "The " + std::string(what) + " with name " +
                                                     std::string(name) + " cannot be found.");
    }

    // ── lookups ──

    template<class Map>
    [[nodiscard]] auto find_in(std::string_view acct, Map iam_account::* member,
                               std::string_view name) const -> const typename Map::mapped_type* {
        const auto* a = account(acct);
        if (a == nullptr) {
            return nullptr;
        }
        const auto& m = a->*member;
        const auto it = m.find(iam_detail::lower(name));
        return it == m.end() ? nullptr : &it->second;
    }

    [[nodiscard]] auto account_mut(const boost::json::object& cmd) -> iam_account* {
        const auto it = _accounts.find(iam_detail::str(cmd, "account"));
        return it == _accounts.end() ? nullptr : &it->second;
    }

    struct parsed_arn {
        std::string account;  // "aws" for AWS-managed
        std::string path;
        std::string name;
    };

    /// Splits `arn:<partition>:iam::<account>:policy<path><name>`.
    [[nodiscard]] auto split_policy_arn(std::string_view arn) const -> std::optional<parsed_arn> {
        const std::string head = "arn:" + _config.partition + ":iam::";
        if (!arn.starts_with(head)) {
            return std::nullopt;
        }
        auto rest = arn.substr(head.size());
        const auto colon = rest.find(':');
        if (colon == std::string_view::npos) {
            return std::nullopt;
        }
        parsed_arn out;
        out.account = std::string(rest.substr(0, colon));
        rest = rest.substr(colon + 1);
        if (!rest.starts_with("policy/")) {
            return std::nullopt;
        }
        rest = rest.substr(6);  // keep the leading '/'
        const auto slash = rest.rfind('/');
        out.path = std::string(rest.substr(0, slash + 1));
        out.name = std::string(rest.substr(slash + 1));
        if (out.name.empty()) {
            return std::nullopt;
        }
        return out;
    }

    /// The document text and version number of a managed policy version
    /// (`version` 0 means the default).
    [[nodiscard]] auto policy_text(std::string_view arn, int version) const
        -> std::optional<std::pair<std::string, int>> {
        const auto p = split_policy_arn(arn);
        if (!p) {
            return std::nullopt;
        }
        if (p->account == "aws") {
            const auto m = find_aws_managed_policy(p->name);
            if (!m || m->path != p->path) {
                return std::nullopt;
            }
            const auto v = iam_detail::parse_version_id(m->version_id).value_or(1);
            if (version != 0 && version != v) {
                return std::nullopt;
            }
            return std::pair{std::string(m->document), v};
        }
        const auto* mp = managed_policy(p->account, p->name);
        if (mp == nullptr || mp->path != p->path) {
            return std::nullopt;
        }
        const int v = version == 0 ? mp->default_version : version;
        const auto it = mp->versions.find(v);
        if (it == mp->versions.end()) {
            return std::nullopt;
        }
        return std::pair{it->second.document, v};
    }

    /// The managed policy an attach/boundary command names: a customer
    /// policy in this account, or a bundled AWS-managed one (null then).
    /// Returns false when the ARN names nothing.
    [[nodiscard]] auto resolve_policy(iam_account& acct, std::string_view arn,
                                      iam_managed_policy** out) const -> bool {
        *out = nullptr;
        const auto p = split_policy_arn(arn);
        if (!p) {
            return false;
        }
        if (p->account == "aws") {
            const auto m = find_aws_managed_policy(p->name);
            return m && m->path == p->path;
        }
        if (p->account != acct.id) {
            return false;
        }
        const auto it = acct.policies.find(iam_detail::lower(p->name));
        if (it == acct.policies.end() || it->second.path != p->path) {
            return false;
        }
        *out = &it->second;
        return true;
    }

    void rebuild_key_index() {
        _key_index.clear();
        for (const auto& [acct_id, a] : _accounts) {
            for (const auto& [kid, _] : a.access_keys) {
                _key_index.emplace(kid, acct_id);
            }
        }
    }

    void forget_compiled(std::string_view arn) {
        const std::lock_guard lock(_compiled_mutex);
        const std::string prefix = std::string(arn) + "#";
        std::erase_if(_compiled, [&](const auto& kv) { return kv.first.starts_with(prefix); });
    }

    // ── dispatch ──

    auto dispatch(iam_command type, const boost::json::object& cmd) -> result {
        using enum iam_command;
        switch (type) {
            case noop:
                return ok();
            case create_account:
                return do_create_account(cmd);
            case set_account_alias:
                return do_set_account_alias(cmd);
            case create_user:
                return do_create_user(cmd);
            case update_user:
                return do_update_user(cmd);
            case delete_user:
                return do_delete_user(cmd);
            case tag_entity:
                return do_tag(cmd, true);
            case untag_entity:
                return do_tag(cmd, false);
            case create_group:
                return do_create_group(cmd);
            case update_group:
                return do_update_group(cmd);
            case delete_group:
                return do_delete_group(cmd);
            case add_user_to_group:
                return do_group_membership(cmd, true);
            case remove_user_from_group:
                return do_group_membership(cmd, false);
            case create_role:
                return do_create_role(cmd);
            case update_role:
                return do_update_role(cmd);
            case update_trust_policy:
                return do_update_trust_policy(cmd);
            case delete_role:
                return do_delete_role(cmd);
            case create_instance_profile:
                return do_create_instance_profile(cmd);
            case delete_instance_profile:
                return do_delete_instance_profile(cmd);
            case add_role_to_instance_profile:
                return do_instance_profile_role(cmd, true);
            case remove_role_from_instance_profile:
                return do_instance_profile_role(cmd, false);
            case create_policy:
                return do_create_policy(cmd);
            case create_policy_version:
                return do_create_policy_version(cmd);
            case delete_policy_version:
                return do_delete_policy_version(cmd);
            case set_default_version:
                return do_set_default_version(cmd);
            case delete_policy:
                return do_delete_policy(cmd);
            case attach_policy:
                return do_attach(cmd, true);
            case detach_policy:
                return do_attach(cmd, false);
            case put_inline_policy:
                return do_put_inline(cmd);
            case delete_inline_policy:
                return do_delete_inline(cmd);
            case put_boundary:
                return do_boundary(cmd, true);
            case delete_boundary:
                return do_boundary(cmd, false);
            case create_access_key:
                return do_create_access_key(cmd);
            case update_access_key:
                return do_update_access_key(cmd);
            case delete_access_key:
                return do_delete_access_key(cmd);
            case record_key_last_used:
                return do_record_last_used(cmd);
            case create_oidc_provider:
                return do_create_oidc_provider(cmd);
            case update_oidc_provider:
                return do_update_oidc_provider(cmd);
            case delete_oidc_provider:
                return do_delete_oidc_provider(cmd);
            case record_sts_key:
                return do_record_sts_key(cmd);
            case retire_sts_key:
                return do_retire_sts_key(cmd);
            case register_replica:
                return do_register_replica(cmd);
            case remove_replica:
                return do_remove_replica(cmd);
        }
        throw std::invalid_argument("iam_state_machine: unknown command type " +
                                    std::to_string(static_cast<int>(type)));
    }

    // ── accounts ──

    auto do_create_account(const boost::json::object& cmd) -> result {
        const auto id = iam_detail::str(cmd, "account");
        if (id.size() != 12 ||
            !std::ranges::all_of(id, [](char c) { return c >= '0' && c <= '9'; })) {
            throw std::invalid_argument("account id must be 12 digits");
        }
        if (_accounts.contains(id)) {
            return error(iam_errors::entity_already_exists, "Account " + id + " already exists.");
        }
        const auto salt = iam_detail::opt_str(cmd, "kek_salt");
        if (_kek_salt.empty()) {
            if (!salt || salt->empty()) {
                throw std::invalid_argument("the first create_account must carry kek_salt");
            }
            _kek_salt = *salt;
        } else if (salt && *salt != _kek_salt) {
            return error(iam_errors::invalid_input, "The cluster's KEK salt is already set.");
        }
        iam_account a;
        a.id = id;
        a.create_date = iam_detail::i64(cmd, "now");
        if (const auto alias = iam_detail::opt_str(cmd, "alias"); alias && !alias->empty()) {
            if (alias_taken(*alias)) {
                return error(iam_errors::entity_already_exists,
                             "The account alias " + *alias + " already exists.");
            }
            a.alias = *alias;
        }
        _accounts.emplace(id, std::move(a));
        return ok({{"account", id}});
    }

    [[nodiscard]] auto alias_taken(std::string_view alias) const -> bool {
        return std::ranges::any_of(_accounts,
                                   [&](const auto& kv) { return kv.second.alias == alias; });
    }

    auto do_set_account_alias(const boost::json::object& cmd) -> result {
        auto* a = account_mut(cmd);
        if (a == nullptr) {
            return no_such("account", iam_detail::str(cmd, "account"));
        }
        const auto alias = iam_detail::str(cmd, "alias");
        if (!alias.empty() && alias != a->alias && alias_taken(alias)) {
            return error(iam_errors::entity_already_exists,
                         "The account alias " + alias + " already exists.");
        }
        a->alias = alias;
        return ok({{"alias", alias}});
    }

    // ── users ──

    auto do_create_user(const boost::json::object& cmd) -> result {
        auto* a = account_mut(cmd);
        if (a == nullptr) {
            return no_such("account", iam_detail::str(cmd, "account"));
        }
        iam_user u;
        u.name = iam_detail::str(cmd, "name");
        if (a->users.contains(iam_detail::lower(u.name))) {
            return error(iam_errors::entity_already_exists,
                         "User with name " + u.name + " already exists.");
        }
        u.path = iam_detail::opt_str(cmd, "path").value_or("/");
        u.id = iam_detail::str(cmd, "id");
        u.create_date = iam_detail::i64(cmd, "now");
        u.tags = iam_detail::tags_from(cmd.if_contains("tags"));
        if (u.tags.size() > _config.limits.tags_per_entity) {
            return error(iam_errors::limit_exceeded, "Cannot exceed quota for TagsPerUser.");
        }
        if (const auto b = iam_detail::opt_str(cmd, "boundary")) {
            iam_managed_policy* mp = nullptr;
            if (!resolve_policy(*a, *b, &mp)) {
                return error(iam_errors::no_such_entity, "Policy " + *b + " does not exist.");
            }
            if (mp != nullptr) {
                ++mp->boundary_usage_count;
            }
            u.boundary = *b;
        }
        auto out = boost::json::object{{"name", u.name},
                                       {"id", u.id},
                                       {"path", u.path},
                                       {"arn", arn(a->id, "user", u.path, u.name)},
                                       {"create_date", u.create_date}};
        a->users.emplace(iam_detail::lower(u.name), std::move(u));
        return ok(std::move(out));
    }

    auto do_update_user(const boost::json::object& cmd) -> result {
        auto* a = account_mut(cmd);
        if (a == nullptr) {
            return no_such("account", iam_detail::str(cmd, "account"));
        }
        const auto name = iam_detail::str(cmd, "name");
        const auto it = a->users.find(iam_detail::lower(name));
        if (it == a->users.end()) {
            return no_such("user", name);
        }
        const auto n = iam_detail::opt_str(cmd, "new_name");
        if (n && iam_detail::lower(*n) != it->first && a->users.contains(iam_detail::lower(*n))) {
            return error(iam_errors::entity_already_exists,
                         "User with name " + *n + " already exists.");
        }
        if (const auto p = iam_detail::opt_str(cmd, "new_path")) {
            it->second.path = *p;
        }
        if (n && iam_detail::lower(*n) != it->first) {
            auto node = a->users.extract(it);
            const auto old_key = node.key();
            node.key() = iam_detail::lower(*n);
            node.mapped().name = *n;
            for (const auto& g : node.mapped().groups) {
                auto& members = a->groups.at(g).members;
                members.erase(old_key);
                members.insert(node.key());
            }
            for (auto& [_, k] : a->access_keys) {
                if (k.owner == old_key) {
                    k.owner = node.key();
                }
            }
            a->users.insert(std::move(node));
        } else if (n) {
            it->second.name = *n;  // a change of case only
        }
        return ok();
    }

    auto do_delete_user(const boost::json::object& cmd) -> result {
        auto* a = account_mut(cmd);
        if (a == nullptr) {
            return no_such("account", iam_detail::str(cmd, "account"));
        }
        const auto name = iam_detail::str(cmd, "name");
        const auto key = iam_detail::lower(name);
        const auto it = a->users.find(key);
        if (it == a->users.end()) {
            return no_such("user", name);
        }
        const auto& u = it->second;
        const bool has_keys = std::ranges::any_of(
            a->access_keys, [&](const auto& kv) { return kv.second.owner == key; });
        if (has_keys || !u.inline_policies.empty() || !u.attached.empty() || !u.groups.empty()) {
            return error(iam_errors::delete_conflict,
                         "Cannot delete entity, must remove access keys, inline policies, "
                         "attached policies and group memberships first.");
        }
        release_boundary(*a, u.boundary);
        a->users.erase(it);
        return ok();
    }

    void release_boundary(iam_account& a, const std::optional<std::string>& boundary) {
        if (!boundary) {
            return;
        }
        iam_managed_policy* mp = nullptr;
        if (resolve_policy(a, *boundary, &mp) && mp != nullptr && mp->boundary_usage_count > 0) {
            --mp->boundary_usage_count;
        }
    }

    // ── tags ──

    [[nodiscard]] auto tags_of(iam_account& a, std::string_view kind, std::string_view name)
        -> tag_map* {
        const auto key = iam_detail::lower(name);
        if (kind == "user") {
            const auto it = a.users.find(key);
            return it == a.users.end() ? nullptr : &it->second.tags;
        }
        if (kind == "role") {
            const auto it = a.roles.find(key);
            return it == a.roles.end() ? nullptr : &it->second.tags;
        }
        if (kind == "policy") {
            const auto it = a.policies.find(key);
            return it == a.policies.end() ? nullptr : &it->second.tags;
        }
        if (kind == "instance-profile") {
            const auto it = a.instance_profiles.find(key);
            return it == a.instance_profiles.end() ? nullptr : &it->second.tags;
        }
        if (kind == "oidc-provider") {
            const auto it = a.oidc_providers.find(std::string(name));
            return it == a.oidc_providers.end() ? nullptr : &it->second.tags;
        }
        throw std::invalid_argument("unknown taggable kind " + std::string(kind));
    }

    auto do_tag(const boost::json::object& cmd, bool add) -> result {
        auto* a = account_mut(cmd);
        if (a == nullptr) {
            return no_such("account", iam_detail::str(cmd, "account"));
        }
        const auto kind = iam_detail::str(cmd, "kind");
        const auto name = iam_detail::str(cmd, "name");
        auto* tags = tags_of(*a, kind, name);
        if (tags == nullptr) {
            return no_such(kind, name);
        }
        if (add) {
            auto merged = *tags;
            for (auto& [k, v] : iam_detail::tags_from(cmd.if_contains("tags"))) {
                merged[k] = v;
            }
            if (merged.size() > _config.limits.tags_per_entity) {
                return error(iam_errors::limit_exceeded, "Cannot exceed quota for TagsPerEntity.");
            }
            *tags = std::move(merged);
        } else {
            for (const auto& k : iam_detail::string_vec(cmd.if_contains("keys"))) {
                tags->erase(k);
            }
        }
        return ok();
    }

    // ── groups ──

    auto do_create_group(const boost::json::object& cmd) -> result {
        auto* a = account_mut(cmd);
        if (a == nullptr) {
            return no_such("account", iam_detail::str(cmd, "account"));
        }
        iam_group g;
        g.name = iam_detail::str(cmd, "name");
        if (a->groups.contains(iam_detail::lower(g.name))) {
            return error(iam_errors::entity_already_exists,
                         "Group with name " + g.name + " already exists.");
        }
        g.path = iam_detail::opt_str(cmd, "path").value_or("/");
        g.id = iam_detail::str(cmd, "id");
        g.create_date = iam_detail::i64(cmd, "now");
        auto out = boost::json::object{{"name", g.name},
                                       {"id", g.id},
                                       {"path", g.path},
                                       {"arn", arn(a->id, "group", g.path, g.name)},
                                       {"create_date", g.create_date}};
        a->groups.emplace(iam_detail::lower(g.name), std::move(g));
        return ok(std::move(out));
    }

    auto do_update_group(const boost::json::object& cmd) -> result {
        auto* a = account_mut(cmd);
        if (a == nullptr) {
            return no_such("account", iam_detail::str(cmd, "account"));
        }
        const auto name = iam_detail::str(cmd, "name");
        const auto it = a->groups.find(iam_detail::lower(name));
        if (it == a->groups.end()) {
            return no_such("group", name);
        }
        const auto n = iam_detail::opt_str(cmd, "new_name");
        if (n && iam_detail::lower(*n) != it->first && a->groups.contains(iam_detail::lower(*n))) {
            return error(iam_errors::entity_already_exists,
                         "Group with name " + *n + " already exists.");
        }
        if (const auto p = iam_detail::opt_str(cmd, "new_path")) {
            it->second.path = *p;
        }
        if (n && iam_detail::lower(*n) != it->first) {
            auto node = a->groups.extract(it);
            const auto old_key = node.key();
            node.key() = iam_detail::lower(*n);
            node.mapped().name = *n;
            for (const auto& m : node.mapped().members) {
                auto& groups = a->users.at(m).groups;
                groups.erase(old_key);
                groups.insert(node.key());
            }
            a->groups.insert(std::move(node));
        } else if (n) {
            it->second.name = *n;
        }
        return ok();
    }

    auto do_delete_group(const boost::json::object& cmd) -> result {
        auto* a = account_mut(cmd);
        if (a == nullptr) {
            return no_such("account", iam_detail::str(cmd, "account"));
        }
        const auto name = iam_detail::str(cmd, "name");
        const auto it = a->groups.find(iam_detail::lower(name));
        if (it == a->groups.end()) {
            return no_such("group", name);
        }
        const auto& g = it->second;
        if (!g.members.empty() || !g.inline_policies.empty() || !g.attached.empty()) {
            return error(iam_errors::delete_conflict,
                         "Cannot delete entity, must remove users, inline policies and "
                         "attached policies first.");
        }
        a->groups.erase(it);
        return ok();
    }

    auto do_group_membership(const boost::json::object& cmd, bool add) -> result {
        auto* a = account_mut(cmd);
        if (a == nullptr) {
            return no_such("account", iam_detail::str(cmd, "account"));
        }
        const auto gname = iam_detail::str(cmd, "group");
        const auto uname = iam_detail::str(cmd, "user");
        const auto git = a->groups.find(iam_detail::lower(gname));
        if (git == a->groups.end()) {
            return no_such("group", gname);
        }
        const auto uit = a->users.find(iam_detail::lower(uname));
        if (uit == a->users.end()) {
            return no_such("user", uname);
        }
        if (add) {
            if (!uit->second.groups.contains(git->first) &&
                uit->second.groups.size() >= _config.limits.groups_per_user) {
                return error(iam_errors::limit_exceeded,
                             "Cannot exceed quota for GroupsPerUser: " +
                                 std::to_string(_config.limits.groups_per_user));
            }
            uit->second.groups.insert(git->first);
            git->second.members.insert(uit->first);
        } else {
            uit->second.groups.erase(git->first);
            git->second.members.erase(uit->first);
        }
        return ok();
    }

    // ── roles ──

    auto do_create_role(const boost::json::object& cmd) -> result {
        auto* a = account_mut(cmd);
        if (a == nullptr) {
            return no_such("account", iam_detail::str(cmd, "account"));
        }
        iam_role r;
        r.name = iam_detail::str(cmd, "name");
        if (a->roles.contains(iam_detail::lower(r.name))) {
            return error(iam_errors::entity_already_exists,
                         "Role with name " + r.name + " already exists.");
        }
        r.path = iam_detail::opt_str(cmd, "path").value_or("/");
        r.id = iam_detail::str(cmd, "id");
        r.create_date = iam_detail::i64(cmd, "now");
        r.trust_policy = iam_detail::str(cmd, "trust_policy");
        if (detail::policy_size(r.trust_policy) > _config.limits.trust_policy_size) {
            return error(iam_errors::limit_exceeded,
                         "Cannot exceed quota for ACLSizePerRole: " +
                             std::to_string(_config.limits.trust_policy_size));
        }
        r.description = iam_detail::opt_str(cmd, "description").value_or("");
        if (const auto* d = cmd.if_contains("max_session_duration")) {
            r.max_session_duration = d->to_number<std::int64_t>();
        }
        r.tags = iam_detail::tags_from(cmd.if_contains("tags"));
        if (r.tags.size() > _config.limits.tags_per_entity) {
            return error(iam_errors::limit_exceeded, "Cannot exceed quota for TagsPerRole.");
        }
        if (const auto b = iam_detail::opt_str(cmd, "boundary")) {
            iam_managed_policy* mp = nullptr;
            if (!resolve_policy(*a, *b, &mp)) {
                return error(iam_errors::no_such_entity, "Policy " + *b + " does not exist.");
            }
            if (mp != nullptr) {
                ++mp->boundary_usage_count;
            }
            r.boundary = *b;
        }
        auto out = boost::json::object{{"name", r.name},
                                       {"id", r.id},
                                       {"path", r.path},
                                       {"arn", arn(a->id, "role", r.path, r.name)},
                                       {"create_date", r.create_date}};
        a->roles.emplace(iam_detail::lower(r.name), std::move(r));
        return ok(std::move(out));
    }

    auto do_update_role(const boost::json::object& cmd) -> result {
        auto* a = account_mut(cmd);
        if (a == nullptr) {
            return no_such("account", iam_detail::str(cmd, "account"));
        }
        const auto name = iam_detail::str(cmd, "name");
        const auto it = a->roles.find(iam_detail::lower(name));
        if (it == a->roles.end()) {
            return no_such("role", name);
        }
        if (const auto d = iam_detail::opt_str(cmd, "description")) {
            it->second.description = *d;
        }
        if (const auto* m = cmd.if_contains("max_session_duration")) {
            it->second.max_session_duration = m->to_number<std::int64_t>();
        }
        return ok();
    }

    auto do_update_trust_policy(const boost::json::object& cmd) -> result {
        auto* a = account_mut(cmd);
        if (a == nullptr) {
            return no_such("account", iam_detail::str(cmd, "account"));
        }
        const auto name = iam_detail::str(cmd, "name");
        const auto it = a->roles.find(iam_detail::lower(name));
        if (it == a->roles.end()) {
            return no_such("role", name);
        }
        auto doc = iam_detail::str(cmd, "trust_policy");
        if (detail::policy_size(doc) > _config.limits.trust_policy_size) {
            return error(iam_errors::limit_exceeded,
                         "Cannot exceed quota for ACLSizePerRole: " +
                             std::to_string(_config.limits.trust_policy_size));
        }
        it->second.trust_policy = std::move(doc);
        return ok();
    }

    auto do_delete_role(const boost::json::object& cmd) -> result {
        auto* a = account_mut(cmd);
        if (a == nullptr) {
            return no_such("account", iam_detail::str(cmd, "account"));
        }
        const auto name = iam_detail::str(cmd, "name");
        const auto it = a->roles.find(iam_detail::lower(name));
        if (it == a->roles.end()) {
            return no_such("role", name);
        }
        const auto& r = it->second;
        if (!r.inline_policies.empty() || !r.attached.empty() || !r.instance_profiles.empty()) {
            return error(iam_errors::delete_conflict,
                         "Cannot delete entity, must remove inline policies, attached "
                         "policies and instance profiles first.");
        }
        release_boundary(*a, r.boundary);
        a->roles.erase(it);
        return ok();
    }

    // ── instance profiles ──

    auto do_create_instance_profile(const boost::json::object& cmd) -> result {
        auto* a = account_mut(cmd);
        if (a == nullptr) {
            return no_such("account", iam_detail::str(cmd, "account"));
        }
        iam_instance_profile p;
        p.name = iam_detail::str(cmd, "name");
        if (a->instance_profiles.contains(iam_detail::lower(p.name))) {
            return error(iam_errors::entity_already_exists,
                         "Instance Profile " + p.name + " already exists.");
        }
        p.path = iam_detail::opt_str(cmd, "path").value_or("/");
        p.id = iam_detail::str(cmd, "id");
        p.create_date = iam_detail::i64(cmd, "now");
        p.tags = iam_detail::tags_from(cmd.if_contains("tags"));
        auto out = boost::json::object{{"name", p.name},
                                       {"id", p.id},
                                       {"path", p.path},
                                       {"arn", arn(a->id, "instance-profile", p.path, p.name)},
                                       {"create_date", p.create_date}};
        a->instance_profiles.emplace(iam_detail::lower(p.name), std::move(p));
        return ok(std::move(out));
    }

    auto do_delete_instance_profile(const boost::json::object& cmd) -> result {
        auto* a = account_mut(cmd);
        if (a == nullptr) {
            return no_such("account", iam_detail::str(cmd, "account"));
        }
        const auto name = iam_detail::str(cmd, "name");
        const auto it = a->instance_profiles.find(iam_detail::lower(name));
        if (it == a->instance_profiles.end()) {
            return no_such("instance profile", name);
        }
        if (!it->second.roles.empty()) {
            return error(iam_errors::delete_conflict,
                         "Cannot delete entity, must remove roles from instance profile first.");
        }
        a->instance_profiles.erase(it);
        return ok();
    }

    auto do_instance_profile_role(const boost::json::object& cmd, bool add) -> result {
        auto* a = account_mut(cmd);
        if (a == nullptr) {
            return no_such("account", iam_detail::str(cmd, "account"));
        }
        const auto pname = iam_detail::str(cmd, "instance_profile");
        const auto rname = iam_detail::str(cmd, "role");
        const auto pit = a->instance_profiles.find(iam_detail::lower(pname));
        if (pit == a->instance_profiles.end()) {
            return no_such("instance profile", pname);
        }
        const auto rit = a->roles.find(iam_detail::lower(rname));
        if (rit == a->roles.end()) {
            return no_such("role", rname);
        }
        if (add) {
            if (pit->second.roles.contains(rit->first)) {
                return error(iam_errors::entity_already_exists,
                             "Role " + rname + " is already in instance profile " + pname + ".");
            }
            if (pit->second.roles.size() >= _config.limits.roles_per_instance_profile) {
                return error(iam_errors::limit_exceeded,
                             "Cannot exceed quota for InstanceSessionsPerInstanceProfile: " +
                                 std::to_string(_config.limits.roles_per_instance_profile));
            }
            pit->second.roles.insert(rit->first);
            rit->second.instance_profiles.insert(pit->first);
        } else {
            if (!pit->second.roles.contains(rit->first)) {
                return error(iam_errors::no_such_entity,
                             "Role " + rname + " is not in instance profile " + pname + ".");
            }
            pit->second.roles.erase(rit->first);
            rit->second.instance_profiles.erase(pit->first);
        }
        return ok();
    }

    // ── managed policies ──

    auto do_create_policy(const boost::json::object& cmd) -> result {
        auto* a = account_mut(cmd);
        if (a == nullptr) {
            return no_such("account", iam_detail::str(cmd, "account"));
        }
        iam_managed_policy p;
        p.name = iam_detail::str(cmd, "name");
        if (a->policies.contains(iam_detail::lower(p.name))) {
            return error(iam_errors::entity_already_exists,
                         "A policy called " + p.name +
                             " already exists. Duplicate names are "
                             "not allowed.");
        }
        p.path = iam_detail::opt_str(cmd, "path").value_or("/");
        p.id = iam_detail::str(cmd, "id");
        p.create_date = p.update_date = iam_detail::i64(cmd, "now");
        p.description = iam_detail::opt_str(cmd, "description").value_or("");
        p.tags = iam_detail::tags_from(cmd.if_contains("tags"));
        auto doc = iam_detail::str(cmd, "document");
        if (detail::policy_size(doc) > _config.limits.managed_policy_size) {
            return error(iam_errors::limit_exceeded,
                         "Cannot exceed quota for PolicySize: " +
                             std::to_string(_config.limits.managed_policy_size));
        }
        p.versions.emplace(1, policy_version{std::move(doc), p.create_date});
        auto out = boost::json::object{{"name", p.name},
                                       {"id", p.id},
                                       {"path", p.path},
                                       {"arn", arn(a->id, "policy", p.path, p.name)},
                                       {"default_version_id", "v1"},
                                       {"create_date", p.create_date}};
        a->policies.emplace(iam_detail::lower(p.name), std::move(p));
        return ok(std::move(out));
    }

    /// The customer policy a command's `"arn"` names, or an error result.
    [[nodiscard]] auto policy_by_arn(iam_account& a, const boost::json::object& cmd,
                                     iam_managed_policy** out, std::string* arn_out) -> result {
        *arn_out = iam_detail::str(cmd, "arn");
        const auto p = split_policy_arn(*arn_out);
        if (p && p->account == "aws") {
            return error(iam_errors::invalid_input, "AWS-managed policies are read-only.");
        }
        if (!p || p->account != a.id) {
            return error(iam_errors::no_such_entity, "Policy " + *arn_out + " does not exist.");
        }
        const auto it = a.policies.find(iam_detail::lower(p->name));
        if (it == a.policies.end() || it->second.path != p->path) {
            return error(iam_errors::no_such_entity, "Policy " + *arn_out + " does not exist.");
        }
        *out = &it->second;
        return {};
    }

    auto do_create_policy_version(const boost::json::object& cmd) -> result {
        auto* a = account_mut(cmd);
        if (a == nullptr) {
            return no_such("account", iam_detail::str(cmd, "account"));
        }
        iam_managed_policy* p = nullptr;
        std::string parn;
        if (auto e = policy_by_arn(*a, cmd, &p, &parn); !e.empty()) {
            return e;
        }
        if (p->versions.size() >= _config.limits.policy_versions) {
            return error(iam_errors::limit_exceeded,
                         "A managed policy can have up to " +
                             std::to_string(_config.limits.policy_versions) +
                             " versions. Before you create a new version, you must delete an "
                             "existing version.");
        }
        auto doc = iam_detail::str(cmd, "document");
        if (detail::policy_size(doc) > _config.limits.managed_policy_size) {
            return error(iam_errors::limit_exceeded,
                         "Cannot exceed quota for PolicySize: " +
                             std::to_string(_config.limits.managed_policy_size));
        }
        const auto now = iam_detail::i64(cmd, "now");
        const int v = p->next_version++;
        p->versions.emplace(v, policy_version{std::move(doc), now});
        p->update_date = now;
        const auto* set_default = cmd.if_contains("set_as_default");
        if (set_default != nullptr && set_default->as_bool()) {
            p->default_version = v;
        }
        return ok({{"version_id", iam_detail::version_id(v)},
                   {"is_default", p->default_version == v},
                   {"create_date", now}});
    }

    auto do_delete_policy_version(const boost::json::object& cmd) -> result {
        auto* a = account_mut(cmd);
        if (a == nullptr) {
            return no_such("account", iam_detail::str(cmd, "account"));
        }
        iam_managed_policy* p = nullptr;
        std::string parn;
        if (auto e = policy_by_arn(*a, cmd, &p, &parn); !e.empty()) {
            return e;
        }
        const auto vid = iam_detail::str(cmd, "version_id");
        const auto v = iam_detail::parse_version_id(vid);
        if (!v || !p->versions.contains(*v)) {
            return error(iam_errors::no_such_entity,
                         "Policy " + parn + " version " + vid + " does not exist.");
        }
        if (*v == p->default_version) {
            return error(iam_errors::delete_conflict,
                         "Cannot delete the default version of a policy.");
        }
        p->versions.erase(*v);
        forget_compiled(parn);
        return ok();
    }

    auto do_set_default_version(const boost::json::object& cmd) -> result {
        auto* a = account_mut(cmd);
        if (a == nullptr) {
            return no_such("account", iam_detail::str(cmd, "account"));
        }
        iam_managed_policy* p = nullptr;
        std::string parn;
        if (auto e = policy_by_arn(*a, cmd, &p, &parn); !e.empty()) {
            return e;
        }
        const auto vid = iam_detail::str(cmd, "version_id");
        const auto v = iam_detail::parse_version_id(vid);
        if (!v || !p->versions.contains(*v)) {
            return error(iam_errors::no_such_entity,
                         "Policy " + parn + " version " + vid + " does not exist.");
        }
        p->default_version = *v;
        return ok();
    }

    auto do_delete_policy(const boost::json::object& cmd) -> result {
        auto* a = account_mut(cmd);
        if (a == nullptr) {
            return no_such("account", iam_detail::str(cmd, "account"));
        }
        iam_managed_policy* p = nullptr;
        std::string parn;
        if (auto e = policy_by_arn(*a, cmd, &p, &parn); !e.empty()) {
            return e;
        }
        if (p->attachment_count > 0 || p->boundary_usage_count > 0) {
            return error(iam_errors::delete_conflict,
                         "Cannot delete a policy attached to entities.");
        }
        if (p->versions.size() > 1) {
            return error(iam_errors::delete_conflict,
                         "This policy has more than one version. Before you delete a policy, "
                         "you must delete the policy's versions. The default version is "
                         "deleted with the policy.");
        }
        a->policies.erase(iam_detail::lower(p->name));
        forget_compiled(parn);
        return ok();
    }

    /// The attachment target a command's `"kind"` and `"name"` name.
    struct principal_ref {
        std::set<std::string>* attached = nullptr;
        std::map<std::string, inline_policy>* inline_policies = nullptr;
        std::optional<std::string>* boundary = nullptr;  // null for groups
        std::size_t inline_limit = 0;
    };

    [[nodiscard]] auto principal(iam_account& a, std::string_view kind, std::string_view name)
        -> std::optional<principal_ref> {
        const auto key = iam_detail::lower(name);
        if (kind == "user") {
            const auto it = a.users.find(key);
            if (it == a.users.end()) {
                return std::nullopt;
            }
            return principal_ref{&it->second.attached, &it->second.inline_policies,
                                 &it->second.boundary, _config.limits.user_inline_total};
        }
        if (kind == "group") {
            const auto it = a.groups.find(key);
            if (it == a.groups.end()) {
                return std::nullopt;
            }
            return principal_ref{&it->second.attached, &it->second.inline_policies, nullptr,
                                 _config.limits.group_inline_total};
        }
        if (kind == "role") {
            const auto it = a.roles.find(key);
            if (it == a.roles.end()) {
                return std::nullopt;
            }
            return principal_ref{&it->second.attached, &it->second.inline_policies,
                                 &it->second.boundary, _config.limits.role_inline_total};
        }
        throw std::invalid_argument("unknown principal kind " + std::string(kind));
    }

    auto do_attach(const boost::json::object& cmd, bool attach) -> result {
        auto* a = account_mut(cmd);
        if (a == nullptr) {
            return no_such("account", iam_detail::str(cmd, "account"));
        }
        const auto kind = iam_detail::str(cmd, "kind");
        const auto name = iam_detail::str(cmd, "name");
        auto target = principal(*a, kind, name);
        if (!target) {
            return no_such(kind, name);
        }
        const auto parn = iam_detail::str(cmd, "arn");
        iam_managed_policy* mp = nullptr;
        if (attach) {
            if (!resolve_policy(*a, parn, &mp)) {
                return error(iam_errors::no_such_entity, "Policy " + parn + " does not exist.");
            }
            if (target->attached->contains(parn)) {
                return ok();  // AWS treats a repeat attach as success
            }
            if (target->attached->size() >= _config.limits.attached_per_entity) {
                return error(iam_errors::limit_exceeded,
                             "Cannot exceed quota for PoliciesPerEntity: " +
                                 std::to_string(_config.limits.attached_per_entity));
            }
            target->attached->insert(parn);
            if (mp != nullptr) {
                ++mp->attachment_count;
            }
        } else {
            if (!target->attached->contains(parn)) {
                return error(iam_errors::no_such_entity, "Policy " + parn + " was not found.");
            }
            target->attached->erase(parn);
            if (resolve_policy(*a, parn, &mp) && mp != nullptr && mp->attachment_count > 0) {
                --mp->attachment_count;
            }
        }
        return ok();
    }

    auto do_put_inline(const boost::json::object& cmd) -> result {
        auto* a = account_mut(cmd);
        if (a == nullptr) {
            return no_such("account", iam_detail::str(cmd, "account"));
        }
        const auto kind = iam_detail::str(cmd, "kind");
        const auto name = iam_detail::str(cmd, "name");
        auto target = principal(*a, kind, name);
        if (!target) {
            return no_such(kind, name);
        }
        inline_policy p{iam_detail::str(cmd, "policy_name"), iam_detail::str(cmd, "document")};
        auto updated = *target->inline_policies;
        updated[iam_detail::lower(p.name)] = p;
        if (iam_detail::inline_total(updated) > target->inline_limit) {
            return error(iam_errors::limit_exceeded,
                         "Maximum policy size of " + std::to_string(target->inline_limit) +
                             " bytes exceeded for " + kind + " " + name);
        }
        *target->inline_policies = std::move(updated);
        return ok();
    }

    auto do_delete_inline(const boost::json::object& cmd) -> result {
        auto* a = account_mut(cmd);
        if (a == nullptr) {
            return no_such("account", iam_detail::str(cmd, "account"));
        }
        const auto kind = iam_detail::str(cmd, "kind");
        const auto name = iam_detail::str(cmd, "name");
        auto target = principal(*a, kind, name);
        if (!target) {
            return no_such(kind, name);
        }
        const auto pname = iam_detail::str(cmd, "policy_name");
        if (target->inline_policies->erase(iam_detail::lower(pname)) == 0) {
            return error(iam_errors::no_such_entity,
                         "The " + kind + " policy with name " + pname + " cannot be found.");
        }
        return ok();
    }

    auto do_boundary(const boost::json::object& cmd, bool put) -> result {
        auto* a = account_mut(cmd);
        if (a == nullptr) {
            return no_such("account", iam_detail::str(cmd, "account"));
        }
        const auto kind = iam_detail::str(cmd, "kind");
        const auto name = iam_detail::str(cmd, "name");
        if (kind == "group") {
            throw std::invalid_argument("a group cannot have a permissions boundary");
        }
        auto target = principal(*a, kind, name);
        if (!target) {
            return no_such(kind, name);
        }
        if (put) {
            const auto parn = iam_detail::str(cmd, "arn");
            iam_managed_policy* mp = nullptr;
            if (!resolve_policy(*a, parn, &mp)) {
                return error(iam_errors::no_such_entity, "Policy " + parn + " does not exist.");
            }
            if (*target->boundary == parn) {
                return ok();
            }
            release_boundary(*a, *target->boundary);
            if (mp != nullptr) {
                ++mp->boundary_usage_count;
            }
            *target->boundary = parn;
        } else {
            if (!target->boundary->has_value()) {
                return error(iam_errors::no_such_entity,
                             "The " + kind + " " + name + " has no permissions boundary.");
            }
            release_boundary(*a, *target->boundary);
            target->boundary->reset();
        }
        return ok();
    }

    // ── access keys ──

    auto do_create_access_key(const boost::json::object& cmd) -> result {
        auto* a = account_mut(cmd);
        if (a == nullptr) {
            return no_such("account", iam_detail::str(cmd, "account"));
        }
        iam_access_key k;
        k.id = iam_detail::str(cmd, "key_id");
        k.owner = iam_detail::lower(iam_detail::str(cmd, "user"));
        k.sealed_secret = iam_detail::str(cmd, "sealed_secret");
        k.create_date = iam_detail::i64(cmd, "now");
        if (!k.owner.empty() && !a->users.contains(k.owner)) {
            return no_such("user", iam_detail::str(cmd, "user"));
        }
        if (_key_index.contains(k.id)) {
            return error(iam_errors::entity_already_exists,
                         "Access key " + k.id + " already exists.");
        }
        const auto owned = std::ranges::count_if(
            a->access_keys, [&](const auto& kv) { return kv.second.owner == k.owner; });
        if (static_cast<std::size_t>(owned) >= _config.limits.access_keys_per_user) {
            return error(iam_errors::limit_exceeded,
                         "Cannot exceed quota for AccessKeysPerUser: " +
                             std::to_string(_config.limits.access_keys_per_user));
        }
        auto out = boost::json::object{
            {"key_id", k.id}, {"user", k.owner}, {"create_date", k.create_date}};
        _key_index.emplace(k.id, a->id);
        a->access_keys.emplace(k.id, std::move(k));
        return ok(std::move(out));
    }

    [[nodiscard]] auto key_in(iam_account& a, const boost::json::object& cmd) -> iam_access_key* {
        const auto it = a.access_keys.find(iam_detail::str(cmd, "key_id"));
        return it == a.access_keys.end() ? nullptr : &it->second;
    }

    auto do_update_access_key(const boost::json::object& cmd) -> result {
        auto* a = account_mut(cmd);
        if (a == nullptr) {
            return no_such("account", iam_detail::str(cmd, "account"));
        }
        auto* k = key_in(*a, cmd);
        if (k == nullptr) {
            return no_such("access key", iam_detail::str(cmd, "key_id"));
        }
        const auto status = iam_detail::str(cmd, "status");
        if (status != "Active" && status != "Inactive") {
            throw std::invalid_argument("access key status must be Active or Inactive");
        }
        k->active = status == "Active";
        return ok();
    }

    auto do_delete_access_key(const boost::json::object& cmd) -> result {
        auto* a = account_mut(cmd);
        if (a == nullptr) {
            return no_such("account", iam_detail::str(cmd, "account"));
        }
        const auto kid = iam_detail::str(cmd, "key_id");
        if (a->access_keys.erase(kid) == 0) {
            return no_such("access key", kid);
        }
        _key_index.erase(kid);
        return ok();
    }

    auto do_record_last_used(const boost::json::object& cmd) -> result {
        for (const auto& e : cmd.at("entries").as_array()) {
            const auto& o = e.as_object();
            const auto kid = iam_detail::str(o, "key_id");
            const auto it = _key_index.find(kid);
            if (it == _key_index.end()) {
                continue;  // deleted since: nothing to record
            }
            auto& k = _accounts.at(it->second).access_keys.at(kid);
            key_last_used lu{iam_detail::i64(o, "time"), iam_detail::str(o, "service"),
                             iam_detail::str(o, "region")};
            if (!k.last_used || k.last_used->time < lu.time) {
                k.last_used = std::move(lu);
            }
        }
        return ok();
    }

    // ── OIDC providers ──

    auto do_create_oidc_provider(const boost::json::object& cmd) -> result {
        auto* a = account_mut(cmd);
        if (a == nullptr) {
            return no_such("account", iam_detail::str(cmd, "account"));
        }
        iam_oidc_provider p;
        p.url = iam_detail::str(cmd, "url");
        if (a->oidc_providers.contains(p.url)) {
            return error(iam_errors::entity_already_exists,
                         "Provider with url https://" + p.url + " already exists.");
        }
        p.client_ids = iam_detail::string_vec(cmd.if_contains("client_ids"));
        p.thumbprints = iam_detail::string_vec(cmd.if_contains("thumbprints"));
        p.create_date = iam_detail::i64(cmd, "now");
        p.tags = iam_detail::tags_from(cmd.if_contains("tags"));
        if (p.client_ids.size() > _config.limits.client_ids_per_oidc_provider ||
            p.thumbprints.size() > _config.limits.thumbprints_per_oidc_provider) {
            return error(iam_errors::limit_exceeded,
                         "Cannot exceed quota for ClientIDs or Thumbprints per provider.");
        }
        auto out = boost::json::object{{"arn", arn(a->id, "oidc-provider/", "", p.url)}};
        a->oidc_providers.emplace(p.url, std::move(p));
        return ok(std::move(out));
    }

    auto do_update_oidc_provider(const boost::json::object& cmd) -> result {
        auto* a = account_mut(cmd);
        if (a == nullptr) {
            return no_such("account", iam_detail::str(cmd, "account"));
        }
        const auto url = iam_detail::str(cmd, "url");
        const auto it = a->oidc_providers.find(url);
        if (it == a->oidc_providers.end()) {
            return no_such("OpenIDConnect provider", url);
        }
        auto updated = it->second;
        if (const auto* t = cmd.if_contains("thumbprints")) {
            updated.thumbprints = iam_detail::string_vec(t);
        }
        if (const auto add = iam_detail::opt_str(cmd, "add_client_id");
            add && std::ranges::find(updated.client_ids, *add) == updated.client_ids.end()) {
            updated.client_ids.push_back(*add);
        }
        if (const auto rm = iam_detail::opt_str(cmd, "remove_client_id")) {
            std::erase(updated.client_ids, *rm);
        }
        if (updated.client_ids.size() > _config.limits.client_ids_per_oidc_provider ||
            updated.thumbprints.size() > _config.limits.thumbprints_per_oidc_provider) {
            return error(iam_errors::limit_exceeded,
                         "Cannot exceed quota for ClientIDs or Thumbprints per provider.");
        }
        it->second = std::move(updated);
        return ok();
    }

    auto do_delete_oidc_provider(const boost::json::object& cmd) -> result {
        auto* a = account_mut(cmd);
        if (a == nullptr) {
            return no_such("account", iam_detail::str(cmd, "account"));
        }
        const auto url = iam_detail::str(cmd, "url");
        if (a->oidc_providers.erase(url) == 0) {
            return no_such("OpenIDConnect provider", url);
        }
        return ok();
    }

    // ── STS keys ──

    /// Every STS-key command carries the leader's "now"; apply() first drops
    /// each retiring key whose retire_after has passed, so expiry follows the
    /// log rather than a replica's clock (as ca_state_machine does for OIDC).
    void advance_sts_clock(std::int64_t now) {
        _sts_clock = std::max(_sts_clock, now);
        std::erase_if(_sts_keys, [&](const sts_key_record& k) {
            return k.retire_after && *k.retire_after <= _sts_clock;
        });
    }

    auto do_record_sts_key(const boost::json::object& cmd) -> result {
        advance_sts_clock(iam_detail::i64(cmd, "now"));
        sts_key_record k{iam_detail::str(cmd, "kid"), iam_detail::str(cmd, "sealed_key"),
                         iam_detail::i64(cmd, "now"), std::nullopt};
        if (std::ranges::any_of(_sts_keys, [&](const auto& e) { return e.kid == k.kid; })) {
            return error(iam_errors::entity_already_exists, "STS key " + k.kid + " exists.");
        }
        _sts_keys.push_back(std::move(k));
        return ok();
    }

    auto do_retire_sts_key(const boost::json::object& cmd) -> result {
        advance_sts_clock(iam_detail::i64(cmd, "now"));
        const auto kid = iam_detail::str(cmd, "kid");
        const auto it = std::ranges::find(_sts_keys, kid, &sts_key_record::kid);
        if (it == _sts_keys.end()) {
            return error(iam_errors::no_such_entity, "STS key " + kid + " does not exist.");
        }
        if (it->retire_after) {
            return error(iam_errors::invalid_input, "STS key " + kid + " is already retiring.");
        }
        it->retire_after = iam_detail::i64(cmd, "retire_after");
        return ok();
    }

    // ── read replicas ──

    static auto replica_from_json(const boost::json::object& o) -> replica_record {
        replica_record r;
        r.node_id = iam_detail::str(o, "node_id");
        r.rpc_host = iam_detail::str(o, "rpc_host");
        const auto port = iam_detail::i64(o, "rpc_port");
        if (r.node_id.empty() || r.rpc_host.empty() || port <= 0 || port > 65535) {
            throw std::invalid_argument("replica needs a node id, host and port");
        }
        r.rpc_port = static_cast<std::uint16_t>(port);
        r.create_date = o.contains("create_date") ? iam_detail::i64(o, "create_date")
                                                  : iam_detail::i64(o, "now");
        return r;
    }

    /// Registering an id again moves it to the new address: a replica
    /// rebuilt on another host keeps its id.
    auto do_register_replica(const boost::json::object& cmd) -> result {
        auto r = replica_from_json(cmd);
        auto id = r.node_id;
        _replicas.insert_or_assign(std::move(id), std::move(r));
        return ok();
    }

    auto do_remove_replica(const boost::json::object& cmd) -> result {
        const auto id = iam_detail::str(cmd, "node_id");
        if (_replicas.erase(id) == 0) {
            return error(iam_errors::no_such_entity, "Replica " + id + " is not registered.");
        }
        return ok();
    }

    // ── snapshot (de)serialisation ──

    static void principal_fields(boost::json::object& o, const std::string& name,
                                 const std::string& path, const std::string& id,
                                 std::int64_t created) {
        o["name"] = name;
        o["path"] = path;
        o["id"] = id;
        o["create_date"] = created;
    }

    [[nodiscard]] static auto account_to_json(const iam_account& a) -> boost::json::object {
        using namespace iam_detail;
        boost::json::object o{{"id", a.id}, {"alias", a.alias}, {"create_date", a.create_date}};
        boost::json::array users;
        for (const auto& [_, u] : a.users) {
            boost::json::object e;
            principal_fields(e, u.name, u.path, u.id, u.create_date);
            e["tags"] = tags_to(u.tags);
            e["inline"] = inline_to(u.inline_policies);
            e["attached"] = strings_to(u.attached);
            if (u.boundary) {
                e["boundary"] = *u.boundary;
            }
            e["groups"] = strings_to(u.groups);
            users.push_back(std::move(e));
        }
        o["users"] = std::move(users);
        boost::json::array groups;
        for (const auto& [_, g] : a.groups) {
            boost::json::object e;
            principal_fields(e, g.name, g.path, g.id, g.create_date);
            e["inline"] = inline_to(g.inline_policies);
            e["attached"] = strings_to(g.attached);
            e["members"] = strings_to(g.members);
            groups.push_back(std::move(e));
        }
        o["groups"] = std::move(groups);
        boost::json::array roles;
        for (const auto& [_, r] : a.roles) {
            boost::json::object e;
            principal_fields(e, r.name, r.path, r.id, r.create_date);
            e["trust_policy"] = r.trust_policy;
            e["description"] = r.description;
            e["max_session_duration"] = r.max_session_duration;
            e["tags"] = tags_to(r.tags);
            e["inline"] = inline_to(r.inline_policies);
            e["attached"] = strings_to(r.attached);
            if (r.boundary) {
                e["boundary"] = *r.boundary;
            }
            e["instance_profiles"] = strings_to(r.instance_profiles);
            roles.push_back(std::move(e));
        }
        o["roles"] = std::move(roles);
        boost::json::array profiles;
        for (const auto& [_, p] : a.instance_profiles) {
            boost::json::object e;
            principal_fields(e, p.name, p.path, p.id, p.create_date);
            e["tags"] = tags_to(p.tags);
            e["roles"] = strings_to(p.roles);
            profiles.push_back(std::move(e));
        }
        o["instance_profiles"] = std::move(profiles);
        boost::json::array policies;
        for (const auto& [_, p] : a.policies) {
            boost::json::object e;
            principal_fields(e, p.name, p.path, p.id, p.create_date);
            e["update_date"] = p.update_date;
            e["description"] = p.description;
            e["tags"] = tags_to(p.tags);
            e["default_version"] = p.default_version;
            e["next_version"] = p.next_version;
            boost::json::array versions;
            for (const auto& [n, v] : p.versions) {
                versions.push_back(boost::json::object{
                    {"n", n}, {"document", v.document}, {"create_date", v.create_date}});
            }
            e["versions"] = std::move(versions);
            policies.push_back(std::move(e));
        }
        o["policies"] = std::move(policies);
        boost::json::array keys;
        for (const auto& [_, k] : a.access_keys) {
            boost::json::object e{{"id", k.id},
                                  {"owner", k.owner},
                                  {"sealed_secret", k.sealed_secret},
                                  {"active", k.active},
                                  {"create_date", k.create_date}};
            if (k.last_used) {
                e["last_used"] = boost::json::object{{"time", k.last_used->time},
                                                     {"service", k.last_used->service},
                                                     {"region", k.last_used->region}};
            }
            keys.push_back(std::move(e));
        }
        o["access_keys"] = std::move(keys);
        boost::json::array oidc;
        for (const auto& [_, p] : a.oidc_providers) {
            oidc.push_back(boost::json::object{{"url", p.url},
                                               {"client_ids", strings_to(p.client_ids)},
                                               {"thumbprints", strings_to(p.thumbprints)},
                                               {"create_date", p.create_date},
                                               {"tags", tags_to(p.tags)}});
        }
        o["oidc_providers"] = std::move(oidc);
        return o;
    }

    template<class Entity> static void read_common(const boost::json::object& e, Entity& out) {
        out.name = iam_detail::str(e, "name");
        out.path = iam_detail::str(e, "path");
        out.id = iam_detail::str(e, "id");
        out.create_date = iam_detail::i64(e, "create_date");
    }

    template<class Map, class Entity>
    static void insert_unique(Map& m, std::string key, Entity&& v, std::string_view what) {
        if (!m.emplace(std::move(key), std::forward<Entity>(v)).second) {
            throw std::invalid_argument("duplicate " + std::string(what));
        }
    }

    [[nodiscard]] static auto account_from_json(const boost::json::object& o) -> iam_account {
        using namespace iam_detail;
        iam_account a;
        a.id = str(o, "id");
        a.alias = str(o, "alias");
        a.create_date = i64(o, "create_date");
        for (const auto& v : o.at("users").as_array()) {
            const auto& e = v.as_object();
            iam_user u;
            read_common(e, u);
            u.tags = tags_from(e.if_contains("tags"));
            u.inline_policies = inline_from(e.if_contains("inline"));
            u.attached = string_set(e.if_contains("attached"));
            u.boundary = opt_str(e, "boundary");
            u.groups = string_set(e.if_contains("groups"));
            auto key = lower(u.name);
            insert_unique(a.users, std::move(key), std::move(u), "user");
        }
        for (const auto& v : o.at("groups").as_array()) {
            const auto& e = v.as_object();
            iam_group g;
            read_common(e, g);
            g.inline_policies = inline_from(e.if_contains("inline"));
            g.attached = string_set(e.if_contains("attached"));
            g.members = string_set(e.if_contains("members"));
            auto key = lower(g.name);
            insert_unique(a.groups, std::move(key), std::move(g), "group");
        }
        for (const auto& v : o.at("roles").as_array()) {
            const auto& e = v.as_object();
            iam_role r;
            read_common(e, r);
            r.trust_policy = str(e, "trust_policy");
            r.description = str(e, "description");
            r.max_session_duration = i64(e, "max_session_duration");
            r.tags = tags_from(e.if_contains("tags"));
            r.inline_policies = inline_from(e.if_contains("inline"));
            r.attached = string_set(e.if_contains("attached"));
            r.boundary = opt_str(e, "boundary");
            r.instance_profiles = string_set(e.if_contains("instance_profiles"));
            auto key = lower(r.name);
            insert_unique(a.roles, std::move(key), std::move(r), "role");
        }
        for (const auto& v : o.at("instance_profiles").as_array()) {
            const auto& e = v.as_object();
            iam_instance_profile p;
            read_common(e, p);
            p.tags = tags_from(e.if_contains("tags"));
            p.roles = string_set(e.if_contains("roles"));
            auto key = lower(p.name);
            insert_unique(a.instance_profiles, std::move(key), std::move(p), "instance profile");
        }
        for (const auto& v : o.at("policies").as_array()) {
            const auto& e = v.as_object();
            iam_managed_policy p;
            read_common(e, p);
            p.update_date = i64(e, "update_date");
            p.description = str(e, "description");
            p.tags = tags_from(e.if_contains("tags"));
            p.default_version = e.at("default_version").to_number<int>();
            p.next_version = e.at("next_version").to_number<int>();
            for (const auto& vv : e.at("versions").as_array()) {
                const auto& ve = vv.as_object();
                p.versions.emplace(ve.at("n").to_number<int>(),
                                   policy_version{str(ve, "document"), i64(ve, "create_date")});
            }
            auto key = lower(p.name);
            insert_unique(a.policies, std::move(key), std::move(p), "policy");
        }
        for (const auto& v : o.at("access_keys").as_array()) {
            const auto& e = v.as_object();
            iam_access_key k;
            k.id = str(e, "id");
            k.owner = str(e, "owner");
            k.sealed_secret = str(e, "sealed_secret");
            k.active = e.at("active").as_bool();
            k.create_date = i64(e, "create_date");
            if (const auto* lu = e.if_contains("last_used")) {
                const auto& l = lu->as_object();
                k.last_used = key_last_used{i64(l, "time"), str(l, "service"), str(l, "region")};
            }
            auto key = k.id;
            insert_unique(a.access_keys, std::move(key), std::move(k), "access key");
        }
        for (const auto& v : o.at("oidc_providers").as_array()) {
            const auto& e = v.as_object();
            iam_oidc_provider p;
            p.url = str(e, "url");
            p.client_ids = string_vec(e.if_contains("client_ids"));
            p.thumbprints = string_vec(e.if_contains("thumbprints"));
            p.create_date = i64(e, "create_date");
            p.tags = tags_from(e.if_contains("tags"));
            auto key = p.url;
            insert_unique(a.oidc_providers, std::move(key), std::move(p), "OIDC provider");
        }
        return a;
    }

    /// Checks what apply() keeps consistent and a snapshot could break:
    /// both sides of every membership, key owners, globally unique key ids,
    /// policy version bookkeeping, and the attachment and boundary counts,
    /// which are recomputed rather than trusted.
    void check_invariants(std::map<std::string, iam_account>& accounts) const {
        std::set<std::string> key_ids;
        for (auto& [id, a] : accounts) {
            if (a.id != id) {
                throw std::invalid_argument("account key mismatch");
            }
            for (const auto& [ukey, u] : a.users) {
                for (const auto& g : u.groups) {
                    const auto it = a.groups.find(g);
                    if (it == a.groups.end() || !it->second.members.contains(ukey)) {
                        throw std::invalid_argument("user " + u.name + " in a missing group");
                    }
                }
            }
            for (const auto& [gkey, g] : a.groups) {
                for (const auto& m : g.members) {
                    const auto it = a.users.find(m);
                    if (it == a.users.end() || !it->second.groups.contains(gkey)) {
                        throw std::invalid_argument("group " + g.name + " has a missing member");
                    }
                }
            }
            for (const auto& [rkey, r] : a.roles) {
                for (const auto& p : r.instance_profiles) {
                    const auto it = a.instance_profiles.find(p);
                    if (it == a.instance_profiles.end() || !it->second.roles.contains(rkey)) {
                        throw std::invalid_argument("role " + r.name + " in a missing profile");
                    }
                }
            }
            for (const auto& [pkey, p] : a.instance_profiles) {
                for (const auto& r : p.roles) {
                    const auto it = a.roles.find(r);
                    if (it == a.roles.end() || !it->second.instance_profiles.contains(pkey)) {
                        throw std::invalid_argument("profile " + p.name + " has a missing role");
                    }
                }
            }
            for (const auto& [kid, k] : a.access_keys) {
                if (!k.owner.empty() && !a.users.contains(k.owner)) {
                    throw std::invalid_argument("access key " + kid + " has no owner");
                }
                if (!key_ids.insert(kid).second) {
                    throw std::invalid_argument("access key " + kid + " in two accounts");
                }
            }
            for (auto& [_, p] : a.policies) {
                if (!p.versions.contains(p.default_version) || p.versions.empty() ||
                    p.versions.rbegin()->first >= p.next_version) {
                    throw std::invalid_argument("policy " + p.name + " has bad versions");
                }
                p.attachment_count = 0;
                p.boundary_usage_count = 0;
            }
            // The account is a parameter rather than a capture: clang's
            // analyzer loses track of a captured structured binding.
            auto count = [this](iam_account& acct, const std::set<std::string>& attached,
                                const std::optional<std::string>* boundary) {
                for (const auto& parn : attached) {
                    iam_managed_policy* mp = nullptr;
                    if (!resolve_policy(acct, parn, &mp)) {
                        throw std::invalid_argument("attachment of a missing policy " + parn);
                    }
                    if (mp != nullptr) {
                        ++mp->attachment_count;
                    }
                }
                if (boundary != nullptr && boundary->has_value()) {
                    iam_managed_policy* mp = nullptr;
                    if (!resolve_policy(acct, **boundary, &mp)) {
                        throw std::invalid_argument("boundary of a missing policy");
                    }
                    if (mp != nullptr) {
                        ++mp->boundary_usage_count;
                    }
                }
            };
            for (const auto& [_, u] : a.users) {
                count(a, u.attached, &u.boundary);
            }
            for (const auto& [_, g] : a.groups) {
                count(a, g.attached, nullptr);
            }
            for (const auto& [_, r] : a.roles) {
                count(a, r.attached, &r.boundary);
            }
        }
    }
};

static_assert(kythira::state_machine<iam_state_machine, std::uint64_t>);

// ── Leader-side helpers ─────────────────────────────────────────────────────

/// What the leader does before proposing: generate ids and secrets, seal,
/// and stamp the time. None of this may run inside apply().
namespace iam_leader {

/// AWS unique ids: a four-letter prefix and 16 characters of RFC 4648
/// base32 (A–Z, 2–7). Prefixes: AIDA user, AGPA group, AROA role, ANPA
/// policy, AIPA instance profile, AKIA access key, ASIA temporary key.
[[nodiscard]] inline auto generate_unique_id(std::string_view prefix) -> std::string {
    static constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
    const auto bytes = random_bytes(16);
    std::string out(prefix);
    for (auto b : bytes) {
        out += alphabet[b & 0x1F];
    }
    return out;
}

/// A 40-character secret access key, like AWS's.
[[nodiscard]] inline auto generate_secret_access_key() -> std::string {
    static constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const auto bytes = random_bytes(40);
    std::string out;
    out.reserve(40);
    for (auto b : bytes) {
        out += alphabet[b & 0x3F];
    }
    return out;
}

[[nodiscard]] inline auto now_seconds() -> std::int64_t {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

/// A command with its type and the leader's time stamped in.
[[nodiscard]] inline auto command(iam_command type, boost::json::object body,
                                  std::int64_t now = now_seconds()) -> std::vector<std::byte> {
    body["type"] = static_cast<int>(type);
    body["now"] = now;
    return iam_detail::bytes_of(body);
}

/// A `create_access_key` command and the plaintext secret to return once,
/// in the CreateAccessKey response. `user` is empty for the account root.
struct new_access_key {
    std::string key_id;
    std::string secret;
    std::vector<std::byte> command;
};

[[nodiscard]] inline auto make_access_key(const seal_key& kek, std::string_view account,
                                          std::string_view user, std::int64_t now = now_seconds())
    -> new_access_key {
    new_access_key out;
    out.key_id = generate_unique_id("AKIA");
    out.secret = generate_secret_access_key();
    out.command = command(iam_command::create_access_key,
                          {{"account", account},
                           {"user", user},
                           {"key_id", out.key_id},
                           {"sealed_secret", seal_secret(kek, out.secret, out.key_id)}},
                          now);
    return out;
}

/// The plaintext secret of a stored access key, or nothing when this node
/// holds no KEK (a replica without the unseal passphrase fails closed).
[[nodiscard]] inline auto open_access_key(const seal_key& kek, const iam_access_key& key)
    -> std::optional<std::string> {
    return open_secret(kek, key.sealed_secret, key.id);
}

/// AWS's name rules: 1–64 characters for users and roles, 1–128 for groups,
/// policies and instance profiles, from [A-Za-z0-9+=,.@_-].
[[nodiscard]] inline auto valid_name(std::string_view kind, std::string_view name) -> bool {
    const std::size_t max = (kind == "user" || kind == "role") ? 64 : 128;
    if (name.empty() || name.size() > max) {
        return false;
    }
    return std::ranges::all_of(name, [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) != 0 ||
               std::string_view("+=,.@_-").find(c) != std::string_view::npos;
    });
}

/// AWS's path rule: "/" alone, or "/" ... "/" of at most 512 printable ASCII
/// characters (U+0021–U+007E).
[[nodiscard]] inline auto valid_path(std::string_view path) -> bool {
    if (path.empty() || path.size() > 512 || path.front() != '/' || path.back() != '/') {
        return false;
    }
    return std::ranges::all_of(path, [](char c) { return c >= '!' && c <= '~'; });
}

/// Batches access-key last-used updates (Requirement 4.8): at most one Raft
/// command per key per interval, so authentication does not turn every read
/// into a write. Not thread-safe; the front end serialises calls.
class last_used_batcher {
public:
    explicit last_used_batcher(std::chrono::seconds interval = std::chrono::minutes(5))
        : _interval(interval.count()) {}

    /// Notes a use. Returns true when the key is now due for a write.
    auto note(std::string_view key_id, std::int64_t now, std::string_view service,
              std::string_view region) -> bool {
        auto& e = _keys[std::string(key_id)];
        e.pending = key_last_used{now, std::string(service), std::string(region)};
        return e.written == 0 || now - e.written >= _interval;
    }

    /// A `record_key_last_used` command for every key due at `now`, or
    /// nothing when none is.
    auto take_due(std::int64_t now) -> std::optional<std::vector<std::byte>> {
        boost::json::array entries;
        for (auto& [kid, e] : _keys) {
            if (e.pending && (e.written == 0 || now - e.written >= _interval)) {
                entries.push_back(boost::json::object{{"key_id", kid},
                                                      {"time", e.pending->time},
                                                      {"service", e.pending->service},
                                                      {"region", e.pending->region}});
                e.written = now;
                e.pending.reset();
            }
        }
        if (entries.empty()) {
            return std::nullopt;
        }
        return command(iam_command::record_key_last_used, {{"entries", std::move(entries)}}, now);
    }

private:
    struct entry {
        std::int64_t written = 0;
        std::optional<key_last_used> pending;
    };
    std::int64_t _interval;
    std::map<std::string, entry> _keys;
};

}  // namespace iam_leader

}  // namespace kythira::iam
