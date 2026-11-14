// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file redis_module.hpp
/// @brief The Redis gateway as a service module
///        (.kiro/specs/service-colocation/ task 7): what redis_gateway_node
///        did between reading its settings and its signal wait, behind
///        service_module<Host>, so that redis_gateway_node (one module on
///        its own Beast transport, ids and data directory exactly as
///        before) and kythira_node (beside other modules, on the node's
///        shared listener, in Redis's group space) run the same code.
///
/// The serving side is chosen by KYTHIRA_CACHE_PERSISTENCE from the
/// factories the module is given, one per storage its build has
/// (redis_serving_api.hpp); each is a multi_raft stack in its own
/// translation unit.
///
/// IAM: beside the IAM module in kythira_node, with
/// KYTHIRA_REDIS_IAM_ACCOUNT_ID, _REGION and _CACHE_NAME naming the cache,
/// AUTH for the ACL's `auth=iam` and `auth=both` users goes to that
/// module's evaluator (`iam.evaluator`), in-process. Otherwise a CONFIG_REDIS_GATEWAY_IAM build
/// keeps its embedded IAM replica (KYTHIRA_IAM_*).

#include "config.hpp"
#include "redis_serving_api.hpp"

#ifdef KYTHIRA_REDIS_GATEWAY_IAM
#include "iam_auth.hpp"
#endif

#include <kythira/redis/redis_acl.hpp>

#include <raft/module_interfaces.hpp>
#include <raft/module_registry.hpp>
#include <raft/node_core.hpp>
#include <raft/service_module.hpp>

#include <cstddef>
#include <cstdint>
#include <exception>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kythira::redis_node {

inline auto read_acl_file(const std::string& path) -> std::string {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("redis_gateway_node: cannot read ACL file " + path);
    }
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

/// The ACL the gateway checks: the file, and with forwarding the internal
/// user, created from the secret rather than written into the file, so the
/// file never holds an identity whose secret it cannot verify was the one
/// the peers were given.
/// @throws std::exception for a file that cannot be read or parsed.
inline void load_acl(const node_options& opt, kythira::redis_acl& acl) {
    if (opt._acl_file.empty()) {
        return;
    }
    auto text = read_acl_file(opt._acl_file);
    acl.reload(text);
    const auto& g = opt._gateway;
    if (!g._internal_secret.empty() && !acl.empty()) {
        acl.reload(text + "user " + g._internal_user + " " +
                   kythira::redis_acl::hash_secret(g._internal_secret) + " read_write *\n");
    }
}

template<typename Host> class redis_module final : public service_node::service_module<Host> {
public:
    /// One serving factory per storage; an empty one is a storage this
    /// build or wire does not have, refused at configure.
    struct factories {
        serving_factory<Host> memory;
        serving_factory<Host> file;
        serving_factory<Host> engine;
    };

    /// @param standalone redis_gateway_node's module: its own variables for
    ///        the node-wide settings and the data directory itself.
    ///        Otherwise kythira_node's: the node's settings fill in what the
    ///        module's variables leave unset (composite_env), and the data
    ///        directory is the node's redis/.
    /// @param max_raft_body The largest Raft request the node's listener
    ///        takes, or 0 when the module sizes its own listener (the
    ///        stand-alone daemon does).
    redis_module(bool standalone, factories f, std::size_t max_raft_body = 0)
        : _standalone(standalone), _factories(std::move(f)), _max_raft_body(max_raft_body) {}

    redis_module(const redis_module&) = delete;
    auto operator=(const redis_module&) -> redis_module& = delete;
    redis_module(redis_module&&) = delete;
    auto operator=(redis_module&&) -> redis_module& = delete;
    ~redis_module() override { stop(); }

    [[nodiscard]] auto name() const -> std::string_view override { return "redis"; }
    [[nodiscard]] auto number() const -> std::uint8_t override {
        return service_node::module_numbers::redis;
    }
    [[nodiscard]] auto env_prefix() const -> std::string_view override { return "KYTHIRA_REDIS_"; }
    [[nodiscard]] auto data_subdir() const -> std::string override {
        return _standalone ? std::string() : std::string("redis");
    }
    /// IAM's evaluator, when co-located, answers before this module
    /// serves.
    [[nodiscard]] auto starts_after() const -> std::vector<std::string> override { return {"iam"}; }

    /// Everything that can be wrong with the configuration fails here,
    /// before the module serves: the variables, the ACL file, IAM's
    /// replica, a storage this build lacks, an entry too large for the
    /// node's listener.
    [[nodiscard]] auto configure(const service_node::env_lookup& env)
        -> std::optional<std::string> override {
        try {
            _opt = from_env(
                _standalone ? env
                            : composite_env(service_node::module_tuning(env, "KYTHIRA_REDIS_")));
            if (_max_raft_body != 0 && raft_request_body_limit(_opt) > _max_raft_body) {
                return "KYTHIRA_REDIS_MAX_VALUE_BYTES and KYTHIRA_CACHE_CHUNK_BYTES need Raft "
                       "requests of " +
                       std::to_string(raft_request_body_limit(_opt)) +
                       " bytes; the node's listener takes " + std::to_string(_max_raft_body);
            }
            switch (_opt._cache._persistence) {
                case cache_persistence::memory:
                    _factory = _factories.memory;
                    break;
                case cache_persistence::file:
                    _factory = _factories.file;
                    break;
                case cache_persistence::engine:
                    _factory = _factories.engine;
                    break;
            }
            if (!_factory) {
                return std::string(
                    "redis_gateway_node: this build has no stack for "
                    "KYTHIRA_CACHE_PERSISTENCE on this wire");
            }
            // The co-located IAM module's evaluator, taken at start, when
            // kythira_node runs one beside this module.
            _iam_in_process =
                !_standalone && service_node::co_located(env, "iam") && iam_scope_from(env);
#ifdef KYTHIRA_REDIS_GATEWAY_IAM
            // Before the gateway starts; it outlives the gateway configured
            // with its verifier.
            if (!_iam_in_process) {
                _iam = start_iam_auth_from_env();
                if (_iam) {
                    _opt._gateway._credential_verifier = _iam->verifier();
                }
            }
#endif
            load_acl(_opt, _acl);
            _gateway_cfg = _opt._gateway;
            route_verifier_users();
        } catch (const std::exception& e) {
            return std::string(e.what());
        }
        return std::nullopt;
    }

    void register_groups(Host& host, const service_node::group_space& space) override {
        _serving = _factory(_opt, _acl, _gateway_cfg, host, space);
    }

    /// @throws std::invalid_argument when IAM is co-located but publishes
    ///         no evaluator.
    void start(service_node::module_registry& registry) override {
        if (_iam_in_process) {
            // IAM starts first (starts_after); the gateway is built by
            // _serving->start() below, from _gateway_cfg as it is then.
            auto iam = registry.lookup<service_node::iam_evaluator>(
                service_node::interface_names::iam_evaluator);
            if (!iam) {
                throw std::invalid_argument("redis: IAM is co-located but publishes no evaluator");
            }
            _gateway_cfg._credential_verifier = iam->credential_verifier(_iam_scope);
            route_verifier_users();
            _iam_evaluator = std::move(iam);
        }
        _serving->start();
        _started = true;
    }

    void drain() override {
        if (_serving) {
            _serving->drain();
        }
    }

    void stop() override {
        if (_serving) {
            _serving->stop();
        }
        _started = false;
        _serving.reset();
    }

    [[nodiscard]] auto health() const -> service_node::module_health override {
        if (!_started) {
            return {false, "not started"};
        }
        return {true, {}};
    }

    [[nodiscard]] auto metrics() const -> std::vector<service_node::module_metric> override {
        if (!_serving) {
            return {};
        }
        return {{"groups", static_cast<double>(_serving->group_count()), {}},
                {"raft_term_sum", _serving->raft_term_sum(), {}}};
    }

    /// The RESP listener's port once started (tests pass port 0).
    [[nodiscard]] auto redis_port() const -> std::uint16_t {
        return _serving ? _serving->redis_port() : 0;
    }

private:
    /// KYTHIRA_REDIS_IAM_ACCOUNT_ID, _REGION and _CACHE_NAME: all three, or
    /// none (no IAM AUTH).
    /// @throws std::invalid_argument for some but not all, or a bad account.
    auto iam_scope_from(const service_node::env_lookup& env) -> bool {
        const auto account = env("KYTHIRA_REDIS_IAM_ACCOUNT_ID");
        const auto region = env("KYTHIRA_REDIS_IAM_REGION");
        const auto cache = env("KYTHIRA_REDIS_IAM_CACHE_NAME");
        if (!account && !region && !cache) {
            return false;
        }
        if (!account || !region || !cache) {
            throw std::invalid_argument(
                "KYTHIRA_REDIS_IAM_ACCOUNT_ID, _REGION and _CACHE_NAME go together");
        }
        if (account->size() != 12 ||
            account->find_first_not_of("0123456789") != std::string::npos) {
            throw std::invalid_argument(
                "KYTHIRA_REDIS_IAM_ACCOUNT_ID must be a 12-digit account id");
        }
        _iam_scope = {*account, *region, *cache};
        return true;
    }

    /// With a credential verifier (IAM), the ACL's `auth=iam` and
    /// `auth=both` users are the verifier's.
    void route_verifier_users() {
        if (_gateway_cfg._credential_verifier && !_gateway_cfg._credential_verifier_users) {
            _gateway_cfg._credential_verifier_users = [acl = &_acl](std::string_view user) {
                const auto mode = acl->auth_mode_of(user);
                return mode == kythira::redis_auth_mode::iam ||
                       mode == kythira::redis_auth_mode::both;
            };
        }
    }

    bool _standalone;
    factories _factories;
    std::size_t _max_raft_body;
    serving_factory<Host> _factory;
    node_options _opt;
#ifdef KYTHIRA_REDIS_GATEWAY_IAM
    std::unique_ptr<iam_auth> _iam;
#endif
    /// kythira_node beside the IAM module.
    bool _iam_in_process{false};
    service_node::elasticache_scope _iam_scope;
    std::shared_ptr<service_node::iam_evaluator> _iam_evaluator;
    kythira::redis_acl _acl;
    kythira::redis_gateway_config _gateway_cfg;
    std::unique_ptr<serving> _serving;
    bool _started{false};
};

}  // namespace kythira::redis_node
