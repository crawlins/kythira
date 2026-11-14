// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file module_registry.hpp
/// @brief Build-time module registration and in-process interfaces
///        between co-located modules (.kiro/specs/service-colocation/
///        task 1, Requirements 1.3 and 7).
///
/// Three pieces:
///
/// - `module_catalog<Host>` lists the modules compiled in. Each module's
///   library defines one `module_registrar<Host>` at namespace scope; the
///   node links the library whole-archive when its Kconfig switch is on,
///   so adding a module changes no node code (Requirement 1.3).
/// - `plan_modules` turns `KYTHIRA_SERVICES` into the modules to build, in
///   start order, or the reason it cannot: an unknown name (listing the
///   ones compiled in), a missing required module, a start-order cycle.
/// - `module_registry` holds the interfaces co-located modules publish
///   (`iam.evaluator`, `hsm.client`, `kinesis.put_records`) by name and
///   type. A lookup that misses means "not co-located", and the consumer
///   uses its network client instead (Requirement 7.3).

#include <raft/service_module.hpp>

#include <algorithm>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <typeindex>
#include <utility>
#include <variant>
#include <vector>

namespace kythira::service_node {

/// The interfaces co-located modules publish, by name and type.
class module_registry {
public:
    /// Publishes `iface` under `name`.
    /// @throws std::logic_error when `name` is already published: two
    ///         modules claiming one interface is a build mistake.
    template<typename Interface> void publish(std::string name, std::shared_ptr<Interface> iface) {
        if (!iface) {
            throw std::invalid_argument("module_registry: null interface for '" + name + "'");
        }
        std::unique_lock lock(_mutex);
        const auto [it, inserted] =
            _entries.try_emplace(std::move(name), entry{typeid(Interface), std::move(iface)});
        if (!inserted) {
            throw std::logic_error("module_registry: '" + it->first + "' is already published");
        }
    }

    /// The interface published under `name`, or null when no co-located
    /// module publishes it.
    /// @throws std::logic_error when it is published with another type:
    ///         the two modules disagree about the interface, which no
    ///         fallback can paper over.
    template<typename Interface>
    [[nodiscard]] auto lookup(std::string_view name) const -> std::shared_ptr<Interface> {
        std::shared_lock lock(_mutex);
        const auto it = _entries.find(name);
        if (it == _entries.end()) {
            return nullptr;
        }
        if (it->second.type != std::type_index(typeid(Interface))) {
            throw std::logic_error("module_registry: '" + std::string(name) +
                                   "' is published as another type");
        }
        return std::static_pointer_cast<Interface>(it->second.iface);
    }

    [[nodiscard]] auto contains(std::string_view name) const -> bool {
        std::shared_lock lock(_mutex);
        return _entries.contains(name);
    }

    /// The published names, sorted.
    [[nodiscard]] auto names() const -> std::vector<std::string> {
        std::shared_lock lock(_mutex);
        std::vector<std::string> out;
        out.reserve(_entries.size());
        for (const auto& [name, e] : _entries) {
            out.push_back(name);
        }
        return out;
    }

private:
    struct entry {
        std::type_index type;
        std::shared_ptr<void> iface;
    };

    mutable std::shared_mutex _mutex;
    std::map<std::string, entry, std::less<>> _entries;
};

/// The modules compiled into this binary, as factories by name.
template<typename Host> class module_catalog {
public:
    using factory = std::function<std::unique_ptr<service_module<Host>>()>;

    /// The process-wide catalog the registrars fill.
    [[nodiscard]] static auto global() -> module_catalog& {
        static module_catalog catalog;
        return catalog;
    }

    /// Adds a module.
    /// @throws std::logic_error when `name` is already registered.
    void add(std::string name, factory make) {
        const auto [it, inserted] = _factories.try_emplace(std::move(name), std::move(make));
        if (!inserted) {
            throw std::logic_error("module_catalog: module '" + it->first +
                                   "' is registered twice");
        }
    }

    [[nodiscard]] auto contains(std::string_view name) const -> bool {
        return _factories.contains(name);
    }

    /// A new instance of module `name`, or null when it is not compiled in.
    [[nodiscard]] auto make(std::string_view name) const -> std::unique_ptr<service_module<Host>> {
        const auto it = _factories.find(name);
        return it == _factories.end() ? nullptr : it->second();
    }

    /// The registered names, sorted.
    [[nodiscard]] auto names() const -> std::vector<std::string> {
        std::vector<std::string> out;
        out.reserve(_factories.size());
        for (const auto& [name, f] : _factories) {
            out.push_back(name);
        }
        return out;
    }

private:
    std::map<std::string, factory, std::less<>> _factories;
};

/// Registers a module in a catalog (by default the global one) when
/// constructed. A module's library defines one at namespace scope:
///
/// @code
/// const kythira::service_node::module_registrar<host_type> k_registrar(
///     "dynamodb", [] { return std::make_unique<dynamodb_module>(); });
/// @endcode
template<typename Host> class module_registrar {
public:
    module_registrar(std::string name, typename module_catalog<Host>::factory make,
                     module_catalog<Host>& catalog = module_catalog<Host>::global()) {
        catalog.add(std::move(name), std::move(make));
    }
};

/// The modules a node will run, in start order; stop in reverse.
template<typename Host> struct module_plan {
    std::vector<std::unique_ptr<service_module<Host>>> modules;
    /// Each module's group space, by position in `modules`.
    std::vector<group_space> spaces;
};

/// Splits `KYTHIRA_SERVICES` ("dynamodb, iam") into names, dropping
/// empty entries and surrounding spaces.
[[nodiscard]] inline auto split_services(std::string_view text) -> std::vector<std::string> {
    std::vector<std::string> out;
    while (!text.empty()) {
        const auto comma = text.find(',');
        auto item = text.substr(0, comma);
        while (!item.empty() && item.front() == ' ') {
            item.remove_prefix(1);
        }
        while (!item.empty() && item.back() == ' ') {
            item.remove_suffix(1);
        }
        if (!item.empty()) {
            out.emplace_back(item);
        }
        if (comma == std::string_view::npos) {
            break;
        }
        text.remove_prefix(comma + 1);
    }
    return out;
}

/// Whether `KYTHIRA_SERVICES` in `env` names `service`. A module in
/// kythira_node asks at configure whether a provider it would call
/// in-process (IAM's evaluator, say) will be on the node, before the
/// registry holds anything; the registry lookup at start then finds it.
[[nodiscard]] inline auto co_located(const env_lookup& env, std::string_view service) -> bool {
    const auto services = env("KYTHIRA_SERVICES");
    if (!services) {
        return false;
    }
    for (const auto& name : split_services(*services)) {
        if (name == service) {
            return true;
        }
    }
    return false;
}

/// Builds the modules `services` names from `catalog`, in start order.
/// `composite` gives each its composite group space; otherwise exactly one
/// module may be named and it keeps today's ids and keys.
/// @returns the plan, or the reason the node must not start.
template<typename Host>
[[nodiscard]] auto plan_modules(const module_catalog<Host>& catalog,
                                const std::vector<std::string>& services, bool composite)
    -> std::variant<module_plan<Host>, std::string> {
    const auto join = [](const std::vector<std::string>& v) {
        std::string s;
        for (const auto& n : v) {
            s += (s.empty() ? "" : ", ") + n;
        }
        return s.empty() ? std::string("none") : s;
    };
    if (services.empty()) {
        return std::string("no services named; compiled in: ") + join(catalog.names());
    }
    if (!composite && services.size() != 1) {
        return std::string("a stand-alone daemon runs one service, not ") + join(services);
    }
    std::map<std::string, std::unique_ptr<service_module<Host>>, std::less<>> chosen;
    for (const auto& name : services) {
        if (chosen.contains(name)) {
            return "service '" + name + "' is named twice";
        }
        auto m = catalog.make(name);
        if (!m) {
            return "service '" + name +
                   "' is not compiled in; compiled in: " + join(catalog.names());
        }
        if (m->name() != name) {
            return "module registered as '" + name + "' names itself '" + std::string(m->name()) +
                   "'";
        }
        chosen.emplace(name, std::move(m));
    }
    for (const auto& [name, m] : chosen) {
        for (const auto& need : m->requires_modules()) {
            if (!chosen.contains(need)) {
                return "service '" + name + "' needs '" + need + "' in KYTHIRA_SERVICES";
            }
        }
    }
    // Depth-first topological order over requires_modules and starts_after
    // among the chosen modules, ties broken by name so the order is the
    // same on every node.
    module_plan<Host> plan;
    std::map<std::string, int, std::less<>> state;  // 1 visiting, 2 done
    std::string cycle;
    std::function<bool(const std::string&)> visit = [&](const std::string& name) {
        auto& s = state[name];
        if (s == 2) {
            return true;
        }
        if (s == 1) {
            cycle = name;
            return false;
        }
        s = 1;
        auto& m = chosen.at(name);
        auto before = m->requires_modules();
        for (auto& n : m->starts_after()) {
            before.push_back(std::move(n));
        }
        std::ranges::sort(before);
        for (const auto& b : before) {
            if (chosen.contains(b) && !visit(b)) {
                return false;
            }
        }
        state[name] = 2;
        plan.modules.push_back(std::move(m));
        return true;
    };
    for (const auto& [name, m] : chosen) {
        if (m && !visit(name)) {
            return "services cannot be ordered: '" + cycle + "' must start before itself";
        }
    }
    for (const auto& m : plan.modules) {
        if (composite && (m->number() == 0 || m->number() > module_numbers::max)) {
            return "service '" + std::string(m->name()) + "' has module number " +
                   std::to_string(m->number()) + ", outside 1-" +
                   std::to_string(module_numbers::max);
        }
        const auto space =
            composite ? group_space::composite(m->number()) : group_space::standalone(m->number());
        for (std::size_t i = 0; i < plan.spaces.size(); ++i) {
            if (space.overlaps(plan.spaces[i])) {
                return "services '" + std::string(plan.modules[i]->name()) + "' and '" +
                       std::string(m->name()) + "' claim the same group ids (module number " +
                       std::to_string(m->number()) + ")";
            }
        }
        plan.spaces.push_back(space);
    }
    return plan;
}

}  // namespace kythira::service_node
