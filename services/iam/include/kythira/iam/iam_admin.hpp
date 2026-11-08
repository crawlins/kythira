// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file iam_admin.hpp
/// @brief The Kythira-only admin operations `kythira-iam` drives through
///        `iam_node`'s `/v1/admin/*` routes (`.kiro/specs/iam-compatible-auth/`
///        Requirement 8.3, design Component 7): create and list accounts,
///        rotate the STS sealing key, and register a read replica.
///
/// Each operation runs on the leader against an `admin_cluster`, which
/// `iam_node` implements over its Raft node and the tests implement over a
/// bare state machine. Everything non-deterministic (account ids, the KEK
/// salt, secrets and their sealing, the time) happens here, before a command
/// is proposed, never in `apply()`.
///
/// The KEK is derived from the unseal passphrase and the cluster's salt,
/// which the first `create_account` records. `kek_cache` derives it once per
/// salt and checks it against a stored sealed secret, so a member started
/// with the wrong passphrase fails loudly instead of sealing secrets no other
/// member can open.

#include <kythira/iam/iam_seal.hpp>
#include <kythira/iam/iam_state_machine.hpp>
#include <kythira/iam/iam_validation.hpp>

#include <boost/json.hpp>

#include <openssl/crypto.h>

#include <algorithm>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kythira::iam::admin {

/// An HTTP status and a JSON body.
struct reply {
    unsigned status = 200;
    boost::json::object body;
};

[[nodiscard]] inline auto error_reply(unsigned status, std::string_view code,
                                      std::string_view message) -> reply {
    return {status, {{"error", code}, {"message", message}}};
}

/// What the admin operations need from the cluster. Every call is made on
/// the leader; any of them may throw when leadership or quorum is lost, and
/// the route answers 503.
template<typename C>
concept admin_cluster = requires(C& c, const std::vector<std::byte>& cmd, const replica_record& r) {
    /// A linearisable copy of the IAM state.
    { c.read() } -> std::same_as<iam_state_machine>;
    /// Proposes `cmd` and returns its `apply()` result once committed.
    { c.submit(cmd) } -> std::same_as<std::vector<std::byte>>;
    /// The KEK for the cluster's salt, deriving it on first use.
    { c.kek(std::string_view{}) } -> std::same_as<const seal_key&>;
    /// Makes `r` reachable and adds it to the Raft configuration as a
    /// learner, once its registration has committed.
    { c.add_learner(r) } -> std::same_as<void>;
};

/// Derives and caches the KEK. Holds the unseal passphrase for the life of
/// the process, as `ca_cluster_node` does, and wipes it on destruction.
class kek_cache {
public:
    explicit kek_cache(std::string passphrase) : _passphrase(std::move(passphrase)) {
        if (_passphrase.empty()) {
            throw std::invalid_argument("iam: the unseal passphrase is empty");
        }
    }
    kek_cache(const kek_cache&) = delete;
    auto operator=(const kek_cache&) -> kek_cache& = delete;
    kek_cache(kek_cache&&) = delete;
    auto operator=(kek_cache&&) -> kek_cache& = delete;
    ~kek_cache() { OPENSSL_cleanse(_passphrase.data(), _passphrase.size()); }

    /// The KEK for `salt`. PBKDF2 runs once per salt; a cluster has one.
    [[nodiscard]] auto get(std::string_view salt) -> const seal_key& {
        const std::lock_guard lock(_mu);
        if (_key.empty() || _salt != salt) {
            _key = derive_seal_key(_passphrase, salt);
            _salt = std::string(salt);
        }
        return _key;
    }

private:
    std::string _passphrase;
    std::mutex _mu;
    std::string _salt;
    seal_key _key;
};

/// One sealed secret and the associated data it was sealed with: what a
/// member opens to check its KEK.
struct sealed_probe {
    std::string sealed;
    std::string associated_data;
};

/// The first sealed access key or STS key in `state`, or nothing when
/// nothing is sealed yet. Every secret is sealed under the same KEK, so one
/// is enough to check it.
[[nodiscard]] inline auto first_sealed(const iam_state_machine& state)
    -> std::optional<sealed_probe> {
    for (const auto& [_, a] : state.accounts()) {
        if (!a.access_keys.empty()) {
            const auto& k = a.access_keys.begin()->second;
            return sealed_probe{k.sealed_secret, k.id};
        }
    }
    if (!state.sts_keys().empty()) {
        const auto& k = state.sts_keys().front();
        return sealed_probe{k.sealed_key, k.kid};
    }
    return std::nullopt;
}

/// Whether `kek` opens `probe`; true when there is nothing to open.
[[nodiscard]] inline auto kek_opens(const std::optional<sealed_probe>& probe, const seal_key& kek)
    -> bool {
    return !probe || open_secret(kek, probe->sealed, probe->associated_data).has_value();
}

[[nodiscard]] inline auto kek_opens(const iam_state_machine& state, const seal_key& kek) -> bool {
    return kek_opens(first_sealed(state), kek);
}

namespace detail {

/// An `apply()` result: the object, or an error reply for an IAM error.
[[nodiscard]] inline auto result_of(const std::vector<std::byte>& bytes)
    -> std::pair<boost::json::object, std::optional<reply>> {
    auto o = iam_detail::parse_bytes(bytes).as_object();
    if (const auto* e = o.if_contains("error")) {
        const std::string code(e->as_string());
        const auto* m = o.if_contains("message");
        const std::string message = m != nullptr ? std::string(m->as_string()) : code;
        const unsigned status = code == iam_errors::entity_already_exists ? 409
                                : code == iam_errors::no_such_entity      ? 404
                                : code == iam_errors::limit_exceeded      ? 409
                                                                          : 400;
        return {std::move(o), error_reply(status, code, message)};
    }
    return {std::move(o), std::nullopt};
}

/// Twelve random digits, the shape of an AWS account id. The first is not
/// zero, so the id never reads as a shorter number.
[[nodiscard]] inline auto random_account_id() -> std::string {
    const auto bytes = random_bytes(12);
    std::string id;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        const int digit = i == 0 ? 1 + (bytes[i] % 9) : bytes[i] % 10;
        id += static_cast<char>('0' + digit);
    }
    return id;
}

[[nodiscard]] inline auto valid_account_id(std::string_view id) -> bool {
    return id.size() == 12 && std::ranges::all_of(id, [](char c) { return c >= '0' && c <= '9'; });
}

/// AWS's alias rule (INV-ACCT-2, iam_validation.hpp).
[[nodiscard]] inline auto valid_alias(std::string_view a) -> bool {
    return validate::account_alias(a);
}

}  // namespace detail

/// Creates an account and its root user's first access key. The secret is
/// in the reply and nowhere else in plaintext (Requirement 4.6): the CLI
/// prints it once and writes it with private_file.hpp permissions.
template<admin_cluster C>
[[nodiscard]] auto create_account(C& cluster, const boost::json::object& request) -> reply {
    std::string account_id;
    if (const auto* v = request.if_contains("account_id"); v != nullptr && v->is_string()) {
        account_id = std::string(v->as_string());
        if (!detail::valid_account_id(account_id)) {
            return error_reply(400, iam_errors::invalid_input, "account_id must be 12 digits.");
        }
    }
    std::string alias;
    if (const auto* v = request.if_contains("alias"); v != nullptr && v->is_string()) {
        alias = std::string(v->as_string());
        if (!alias.empty() && !detail::valid_alias(alias)) {
            return error_reply(400, iam_errors::invalid_input,
                               "alias must be 3-63 lower-case letters, digits and single hyphens, "
                               "and not a 12-digit number.");
        }
    }

    const auto state = cluster.read();
    if (account_id.empty()) {
        do {
            account_id = detail::random_account_id();
        } while (state.account(account_id) != nullptr);
    }
    boost::json::object body{{"account", account_id}};
    if (!alias.empty()) {
        body["alias"] = alias;
    }
    std::string salt = state.kek_salt();
    if (salt.empty()) {
        salt = generate_kek_salt();
        body["kek_salt"] = salt;
    }
    auto [created, failed] =
        detail::result_of(cluster.submit(iam_leader::command(iam_command::create_account, body)));
    if (failed) {
        return *failed;
    }
    // A concurrent first create_account may have set a different salt; the
    // committed one wins.
    if (body.contains("kek_salt")) {
        salt = cluster.read().kek_salt();
    }
    const auto& kek = cluster.kek(salt);
    auto key = iam_leader::make_access_key(kek, account_id, "");
    auto [made, key_failed] = detail::result_of(cluster.submit(key.command));
    if (key_failed) {
        OPENSSL_cleanse(key.secret.data(), key.secret.size());
        return *key_failed;
    }
    reply out{201,
              {{"account_id", account_id},
               {"alias", alias},
               {"access_key_id", key.key_id},
               {"secret_access_key", key.secret}}};
    OPENSSL_cleanse(key.secret.data(), key.secret.size());
    return out;
}

/// Every account, without secrets.
template<admin_cluster C> [[nodiscard]] auto list_accounts(C& cluster) -> reply {
    const auto state = cluster.read();
    boost::json::array accounts;
    for (const auto& [id, a] : state.accounts()) {
        std::size_t root_keys = 0;
        for (const auto& [_, k] : a.access_keys) {
            root_keys += k.owner.empty() ? 1 : 0;
        }
        accounts.push_back(
            boost::json::object{{"account_id", id},
                                {"alias", a.alias},
                                {"create_date", a.create_date},
                                {"users", static_cast<std::int64_t>(a.users.size())},
                                {"roles", static_cast<std::int64_t>(a.roles.size())},
                                {"root_access_keys", static_cast<std::int64_t>(root_keys)}});
    }
    return {200, {{"accounts", std::move(accounts)}}};
}

/// Records a new STS sealing key and starts retiring the others: a retiring
/// key still opens tokens until `retire_after`, by default the longest
/// session a token can carry (GetFederationToken's 36 hours).
template<admin_cluster C>
[[nodiscard]] auto rotate_sts_key(C& cluster, const boost::json::object& request) -> reply {
    std::int64_t retire_in = std::int64_t{36} * 3600;
    if (const auto* v = request.if_contains("retire_after_seconds"); v != nullptr) {
        if (!v->is_int64() || v->as_int64() < 0 || v->as_int64() > std::int64_t{30} * 86400) {
            return error_reply(400, iam_errors::invalid_input,
                               "retire_after_seconds must be between 0 and 2592000.");
        }
        retire_in = v->as_int64();
    }
    const auto state = cluster.read();
    if (state.kek_salt().empty()) {
        return error_reply(409, "NotUnsealed",
                           "No account exists yet: the first create-account sets the key "
                           "sealing salt.");
    }
    const auto& kek = cluster.kek(state.kek_salt());
    const auto now = iam_leader::now_seconds();
    const auto kid = "sts-" + jose::base64url_encode(random_bytes(9));
    auto key = random_bytes(32);
    const auto key_text = jose::base64url_encode(key);
    OPENSSL_cleanse(key.data(), key.size());
    auto sealed = seal_secret(kek, key_text, kid);
    auto [recorded, failed] = detail::result_of(cluster.submit(iam_leader::command(
        iam_command::record_sts_key, {{"kid", kid}, {"sealed_key", sealed}}, now)));
    if (failed) {
        return *failed;
    }
    boost::json::array retiring;
    for (const auto& k : state.sts_keys()) {
        if (k.retire_after) {
            continue;
        }
        auto [_, retire_failed] = detail::result_of(cluster.submit(
            iam_leader::command(iam_command::retire_sts_key,
                                {{"kid", k.kid}, {"retire_after", now + retire_in}}, now)));
        if (retire_failed) {
            return *retire_failed;
        }
        retiring.push_back(boost::json::object{{"kid", k.kid}, {"retire_after", now + retire_in}});
    }
    return {200, {{"kid", kid}, {"retiring", std::move(retiring)}}};
}

/// Registers a read replica and adds it to the Raft configuration as a
/// learner. `node_id` is canonical node id text; the caller checks it parses
/// as this build's id type.
template<admin_cluster C>
[[nodiscard]] auto add_replica(C& cluster, const boost::json::object& request) -> reply {
    const auto* id = request.if_contains("node_id");
    const auto* host = request.if_contains("rpc_host");
    const auto* port = request.if_contains("rpc_port");
    if (id == nullptr || !id->is_string() || id->as_string().empty() || host == nullptr ||
        !host->is_string() || host->as_string().empty() || port == nullptr || !port->is_int64() ||
        port->as_int64() <= 0 || port->as_int64() > 65535) {
        return error_reply(400, iam_errors::invalid_input,
                           "node_id, rpc_host and rpc_port (1-65535) are required.");
    }
    replica_record r{std::string(id->as_string()), std::string(host->as_string()),
                     static_cast<std::uint16_t>(port->as_int64()), iam_leader::now_seconds()};
    auto [_, failed] = detail::result_of(cluster.submit(iam_leader::command(
        iam_command::register_replica,
        {{"node_id", r.node_id}, {"rpc_host", r.rpc_host}, {"rpc_port", r.rpc_port}},
        r.create_date)));
    if (failed) {
        return *failed;
    }
    cluster.add_learner(r);
    return {200,
            {{"node_id", r.node_id},
             {"rpc_host", r.rpc_host},
             {"rpc_port", static_cast<std::int64_t>(r.rpc_port)}}};
}

}  // namespace kythira::iam::admin
