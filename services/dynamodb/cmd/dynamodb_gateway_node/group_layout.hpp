// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

// The groups a RocksDB node had, so that it restarts with them (task 24).
//
// A DynamoDB group's state machine keeps its applied index in the engine, so
// after a restart nothing replays the entries it applied before, a split's
// among them. A node that re-created only its bootstrap groups would have the
// parent claim its children's ranges again, and no children at all. The host
// tells the daemon every replica's layout synchronously in the apply path of
// each split (multi_raft_config::on_local_layout_changed), before the parent
// applies anything later, and the daemon saves it here; at start it creates
// the groups from this file, or from the bootstrap layout when there is none.
//
// The format is JSON: {"version": 1, "groups": [{"id": "<decimal>",
// "start": "<hex>" | null, "end": "<hex>" | null, "version": n,
// "conf_version": n, "voters": ["<node id>"...], "learners": [...]}]}.
// Range bounds are hex, as keys are binary.

#include "durable_file.hpp"

#include <boost/json.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace dynamodb_gateway {

namespace layout_detail {

inline auto to_hex(std::string_view raw) -> std::string {
    static constexpr std::string_view k_digits = "0123456789abcdef";
    std::string out;
    out.reserve(raw.size() * 2);
    for (const auto c : raw) {
        const auto b = static_cast<unsigned char>(c);
        out.push_back(k_digits[b >> 4U]);
        out.push_back(k_digits[b & 0xFU]);
    }
    return out;
}

inline auto from_hex(std::string_view hex) -> std::string {
    const auto digit = [](char c) -> int {
        if (c >= '0' && c <= '9') {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f') {
            return c - 'a' + 10;
        }
        throw std::runtime_error("group layout: bad hex digit");
    };
    if (hex.size() % 2 != 0) {
        throw std::runtime_error("group layout: odd hex length");
    }
    std::string out;
    out.reserve(hex.size() / 2);
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        out.push_back(static_cast<char>((digit(hex[i]) << 4) | digit(hex[i + 1])));
    }
    return out;
}

}  // namespace layout_detail

/// Saves and loads a node's group descriptors. `Descriptor` is the host's
/// `shard_descriptor<std::uint64_t, std::string, NodeId>`.
template<typename Descriptor, typename NodeId> class group_layout_file {
public:
    using to_text_fn = std::function<std::string(const NodeId&)>;
    using from_text_fn = std::function<NodeId(const std::string&)>;

    group_layout_file(std::filesystem::path path, to_text_fn to_text, from_text_fn from_text)
        : _path(std::move(path)), _to_text(std::move(to_text)), _from_text(std::move(from_text)) {}

    [[nodiscard]] auto path() const -> const std::filesystem::path& { return _path; }

    /// The saved layout, or none when nothing was saved. Throws on a file
    /// that cannot be read as one: starting from the bootstrap layout
    /// instead would be the very loss this file exists to prevent.
    [[nodiscard]] auto load() const -> std::optional<std::vector<Descriptor>> {
        std::ifstream in(_path);
        if (!in) {
            return std::nullopt;
        }
        const std::string text{std::istreambuf_iterator<char>(in),
                               std::istreambuf_iterator<char>()};
        boost::system::error_code ec;
        const auto doc = boost::json::parse(text, ec);
        if (ec || !doc.is_object() || !doc.as_object().contains("groups")) {
            throw std::runtime_error("group layout: " + _path.string() + " is not a layout");
        }
        std::vector<Descriptor> out;
        for (const auto& v : doc.at("groups").as_array()) {
            const auto& g = v.as_object();
            Descriptor d;
            d._group_id = std::stoull(std::string(g.at("id").as_string()));
            if (!g.at("start").is_null()) {
                d._range._start = layout_detail::from_hex(g.at("start").as_string());
            }
            if (!g.at("end").is_null()) {
                d._range._end = layout_detail::from_hex(g.at("end").as_string());
            }
            d._epoch._version = g.at("version").to_number<std::uint64_t>();
            d._epoch._conf_version = g.at("conf_version").to_number<std::uint64_t>();
            for (const auto& n : g.at("voters").as_array()) {
                d._voters.push_back(_from_text(std::string(n.as_string())));
            }
            for (const auto& n : g.at("learners").as_array()) {
                d._learners.push_back(_from_text(std::string(n.as_string())));
            }
            out.push_back(std::move(d));
        }
        return out;
    }

    /// Replaces the saved layout. False when it could not be saved durably.
    [[nodiscard]] auto save(const std::vector<Descriptor>& layout) const -> bool {
        boost::json::array groups;
        for (const auto& d : layout) {
            boost::json::object g;
            g["id"] = std::to_string(d._group_id);
            g["start"] = d._range._start
                             ? boost::json::value(layout_detail::to_hex(*d._range._start))
                             : boost::json::value(nullptr);
            g["end"] = d._range._end ? boost::json::value(layout_detail::to_hex(*d._range._end))
                                     : boost::json::value(nullptr);
            g["version"] = d._epoch._version;
            g["conf_version"] = d._epoch._conf_version;
            boost::json::array voters;
            for (const auto& n : d._voters) {
                voters.emplace_back(_to_text(n));
            }
            g["voters"] = std::move(voters);
            boost::json::array learners;
            for (const auto& n : d._learners) {
                learners.emplace_back(_to_text(n));
            }
            g["learners"] = std::move(learners);
            groups.emplace_back(std::move(g));
        }
        boost::json::object doc;
        doc["version"] = 1;
        doc["groups"] = std::move(groups);
        return write_durably(_path, boost::json::serialize(doc) + "\n");
    }

private:
    std::filesystem::path _path;
    to_text_fn _to_text;
    from_text_fn _from_text;
};

}  // namespace dynamodb_gateway
