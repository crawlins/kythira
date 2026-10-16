// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file self_node_id.hpp
/// @brief A node's own composite id, read from its cloud's instance metadata
///        service (`.kiro/specs/cloud-composite-node-ids/` Requirement 6).
///
/// A quorum manager that cannot know a node's id before launch (EC2, ASG,
/// ESS, VMSS, MIG, OCI pool) leaves the node to learn it: the binary is
/// started with `--node-id=metadata:<provider>` and resolves it here.
///
/// Every lookup is opt-in (Requirement 6.3). Nothing in this header runs
/// unless a caller names a provider, because off-cloud a link-local address
/// can stall until the TCP timeout. Each lookup has one deadline,
/// `self_node_id_options::timeout`, that bounds every request it makes,
/// connection included, and nothing is retried: a host that is not on the
/// named cloud gets an error within that budget rather than a hang.
///
/// What each source reads:
/// - `aws_ec2`: IMDSv2 at 169.254.169.254, a PUT for a session token, then
///   `instance-id` and `placement/region`.
/// - `alibaba_ecs`: 100.100.100.200, the same token flow (hardened mode),
///   then `instance-id` and `region-id`.
/// - `azure_vm`: IMDS at 169.254.169.254, `instance/compute`'s `name` and
///   `resourceGroupName`, with `Metadata: true`.
/// - `gcp`: 169.254.169.254, `instance/name`, `instance/zone` and
///   `project/project-id`, with `Metadata-Flavor: Google`.
/// - `oci`: IMDS v2 at 169.254.169.254, `instance/id`, with
///   `Authorization: Bearer Oracle`; the region is read from the OCID.
///
/// The GCP server is addressed by IP, not `metadata.google.internal`, so an
/// off-cloud lookup never waits on DNS.

#include <raft/composite_node_id.hpp>

#include <httplib.h>
#include <boost/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace kythira {

/// @brief The metadata service a node reads its own id from.
enum class node_id_source : std::uint8_t {
    aws_ec2 = 0,      ///< AWS EC2 IMDSv2 → `aws_ec2_node_id`.
    alibaba_ecs = 1,  ///< Alibaba Cloud ECS metadata → `alibaba_ecs_node_id`.
    azure_vm = 2,     ///< Azure IMDS → `azure_vm_node_id`.
    gcp = 3,          ///< GCP Compute metadata server → `gcp_instance_node_id`.
    oci = 4,          ///< OCI IMDS v2 → `oci_instance_node_id`.
};

/// @brief The provider name of a source's composite id type, which is also
///        the canonical spelling in `metadata:<provider>`.
[[nodiscard]] constexpr auto node_id_source_name(node_id_source source) -> std::string_view {
    switch (source) {
        case node_id_source::aws_ec2:
            return aws_ec2_node_id::provider;
        case node_id_source::alibaba_ecs:
            return alibaba_ecs_node_id::provider;
        case node_id_source::azure_vm:
            return azure_vm_node_id::provider;
        case node_id_source::gcp:
            return gcp_instance_node_id::provider;
        case node_id_source::oci:
            return oci_instance_node_id::provider;
    }
    return "unknown";
}

/// @brief Reads a provider name: the composite type's own (`aws-ec2`,
///        `alibaba-ecs`, `azure-vm`, `gcp`, `oci`) or the cloud's short name
///        (`aws`, `alibaba`, `azure`). Exact and lower case only.
[[nodiscard]] constexpr auto parse_node_id_source(std::string_view name)
    -> std::optional<node_id_source> {
    if (name == "aws" || name == aws_ec2_node_id::provider) {
        return node_id_source::aws_ec2;
    }
    if (name == "alibaba" || name == alibaba_ecs_node_id::provider) {
        return node_id_source::alibaba_ecs;
    }
    if (name == "azure" || name == azure_vm_node_id::provider) {
        return node_id_source::azure_vm;
    }
    if (name == gcp_instance_node_id::provider) {
        return node_id_source::gcp;
    }
    if (name == oci_instance_node_id::provider) {
        return node_id_source::oci;
    }
    return std::nullopt;
}

/// @brief Where and how long to look.
struct self_node_id_options {
    /// The metadata server's address. Empty uses the provider's link-local
    /// one; tests point it at a loopback fake.
    std::string host;
    /// The metadata server's port. Every provider serves plain HTTP on 80.
    std::uint16_t port{80};
    /// One budget for the whole lookup, every request and connection
    /// included. The AWS SDK's own IMDS default is one second per attempt;
    /// two covers a token and two reads on a slow boot without letting an
    /// off-cloud start stall noticeably.
    std::chrono::milliseconds timeout{std::chrono::seconds{2}};
};

/// @brief The default metadata address of a source.
[[nodiscard]] constexpr auto default_metadata_host(node_id_source source) -> std::string_view {
    return source == node_id_source::alibaba_ecs ? "100.100.100.200" : "169.254.169.254";
}

/// @brief A node id, or the reason it could not be read.
///
/// The `std::expected<T, std::string>` the design names, spelled out because
/// clang 18 (the CI static-analysis compiler) does not provide
/// `std::expected` over libstdc++ 13. The member names match it, so moving to
/// `std::expected` later is a type change and nothing else.
template<typename T> class node_id_result {
public:
    /// Implicit, as `std::expected`'s value constructor is.
    node_id_result(T value) : _state(std::in_place_index<0>, std::move(value)) {}

    /// @brief The failure value.
    static auto failure(std::string error) -> node_id_result {
        return node_id_result(std::in_place_index<1>, std::move(error));
    }

    [[nodiscard]] auto has_value() const noexcept -> bool { return _state.index() == 0; }
    explicit operator bool() const noexcept { return has_value(); }

    /// Throws `std::logic_error` when there is no value.
    [[nodiscard]] auto value() const& -> const T& {
        if (!has_value()) {
            throw std::logic_error("node_id_result: no value: " + error());
        }
        return std::get<0>(_state);
    }
    [[nodiscard]] auto value() && -> T {
        if (!has_value()) {
            throw std::logic_error("node_id_result: no value: " + error());
        }
        return std::get<0>(std::move(_state));
    }
    [[nodiscard]] auto operator*() const& -> const T& { return value(); }
    [[nodiscard]] auto operator->() const -> const T* { return &value(); }

    /// The reason, or an empty string when there is a value.
    [[nodiscard]] auto error() const -> std::string {
        return has_value() ? std::string{} : std::get<1>(_state);
    }

private:
    template<std::size_t I, typename U>
    node_id_result(std::in_place_index_t<I> tag, U&& v) : _state(tag, std::forward<U>(v)) {}

    std::variant<T, std::string> _state;
};

namespace self_node_id_detail {

/// No metadata answer this lookup reads is anywhere near this large; a
/// response that is cannot be the service it claims to be.
inline constexpr std::size_t k_max_body = 64U * 1024U;

/// Token lifetime requested from IMDSv2 and Alibaba's hardened mode. The
/// token is used for the next two requests and dropped.
inline constexpr std::string_view k_token_ttl_seconds = "60";

/// How close to the deadline a failed request counts as having timed out.
inline constexpr std::chrono::milliseconds k_deadline_slack{20};

/// One lookup: one connection to the metadata server, one deadline.
class metadata_session {
public:
    metadata_session(node_id_source source, const self_node_id_options& options)
        : _source(source),
          _host(options.host.empty() ? std::string(default_metadata_host(source)) : options.host),
          _port(options.port),
          _timeout(options.timeout),
          _deadline(std::chrono::steady_clock::now() + options.timeout),
          _client(_host, _port) {
        _client.set_keep_alive(true);
    }

    /// GET (or, with `put`, an empty-bodied PUT) of `path`. Returns the body
    /// with surrounding ASCII whitespace removed, or an error naming the
    /// request. `response_header`, when set, must come back with the value
    /// `response_value`.
    auto request(std::string_view path, const httplib::Headers& headers, bool put = false,
                 std::string_view response_header = {}, std::string_view response_value = {})
        -> node_id_result<std::string> {
        const auto what = std::string(put ? "PUT " : "GET ") + std::string(path);
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            _deadline - std::chrono::steady_clock::now());
        if (remaining <= std::chrono::milliseconds::zero()) {
            return timed_out(what);
        }
        _client.set_connection_timeout(remaining);
        _client.set_read_timeout(remaining);
        _client.set_write_timeout(remaining);
        _client.set_max_timeout(remaining);

        std::string body;
        bool too_large = false;
        httplib::Result res;
        if (put) {
            res = _client.Put(std::string(path), headers, std::string{}, "text/plain");
            if (res) {
                body = res->body;
                too_large = body.size() > k_max_body;
            }
        } else {
            res =
                _client.Get(std::string(path), headers, [&](const char* data, std::size_t length) {
                    if (body.size() + length > k_max_body) {
                        too_large = true;
                        return false;
                    }
                    body.append(data, length);
                    return true;
                });
        }
        if (too_large) {
            return fail(what + ": response larger than " + std::to_string(k_max_body) + " bytes");
        }
        if (!res) {
            // A read that ran into the deadline surfaces as a plain read
            // error, a few milliseconds early by poll()'s rounding.
            if (std::chrono::steady_clock::now() + k_deadline_slack >= _deadline ||
                res.error() == httplib::Error::Timeout ||
                res.error() == httplib::Error::ConnectionTimeout) {
                return timed_out(what);
            }
            return fail(what + ": " + httplib::to_string(res.error()));
        }
        if (res->status != 200) {
            return fail(what + ": HTTP " + std::to_string(res->status));
        }
        if (!response_header.empty() &&
            res->get_header_value(std::string(response_header)) != response_value) {
            return fail(what + ": response lacks " + std::string(response_header) + ": " +
                        std::string(response_value) + ", so it is not the metadata server");
        }
        return trim(std::move(body));
    }

    /// An error prefixed with the source, for every failure that is not
    /// about one request.
    [[nodiscard]] auto fail(const std::string& message) const -> node_id_result<std::string> {
        return node_id_result<std::string>::failure(
            "metadata:" + std::string(node_id_source_name(_source)) + " (" + _host + ":" +
            std::to_string(_port) + "): " + message);
    }

private:
    [[nodiscard]] auto timed_out(const std::string& what) const -> node_id_result<std::string> {
        return fail(what + ": no answer within " + std::to_string(_timeout.count()) +
                    " ms (is this host on that cloud?)");
    }

    static auto trim(std::string s) -> std::string {
        auto space = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
        while (!s.empty() && space(s.back())) {
            s.pop_back();
        }
        const auto first = std::ranges::find_if_not(s, space);
        s.erase(s.begin(), first);
        return s;
    }

    node_id_source _source;
    std::string _host;
    std::uint16_t _port;
    std::chrono::milliseconds _timeout;
    std::chrono::steady_clock::time_point _deadline;
    httplib::Client _client;
};

/// Builds `Id` from a scope and native id read off the metadata server,
/// turning a validation failure into an error that names both values.
template<typename Id>
auto build(const metadata_session& session, std::string scope, std::string native)
    -> node_id_result<std::string> {
    try {
        return Id(std::move(scope), std::move(native)).to_string();
    } catch (const std::invalid_argument& e) {
        return session.fail(std::string("the service's answer is not a valid id: ") + e.what());
    }
}

/// The token-then-read flow AWS IMDSv2 and Alibaba's hardened mode share.
/// Without a token there is no fallback to the token-less form: that would be
/// a second attempt the design does not allow, and IMDSv1 is commonly
/// disabled anyway.
template<typename Id>
auto token_lookup(metadata_session& session, std::string_view token_ttl_header,
                  std::string_view token_header, std::string_view native_path,
                  std::string_view scope_path) -> node_id_result<std::string> {
    auto token = session.request(
        "/latest/api/token", {{std::string(token_ttl_header), std::string(k_token_ttl_seconds)}},
        /*put=*/true);
    if (!token) {
        return token;
    }
    if (token->empty()) {
        return session.fail("PUT /latest/api/token: empty token");
    }
    const httplib::Headers with_token{{std::string(token_header), *token}};
    auto native = session.request(native_path, with_token);
    if (!native) {
        return native;
    }
    auto scope = session.request(scope_path, with_token);
    if (!scope) {
        return scope;
    }
    return build<Id>(session, std::move(scope).value(), std::move(native).value());
}

inline auto azure_lookup(metadata_session& session) -> node_id_result<std::string> {
    static constexpr std::string_view path = "/metadata/instance/compute?api-version=2021-02-01";
    auto body = session.request(path, {{"Metadata", "true"}});
    if (!body) {
        return body;
    }
    boost::system::error_code ec;
    const auto doc = boost::json::parse(*body, ec);
    const auto* obj = ec ? nullptr : doc.if_object();
    const auto* name = obj != nullptr ? obj->if_contains("name") : nullptr;
    const auto* group = obj != nullptr ? obj->if_contains("resourceGroupName") : nullptr;
    if (name == nullptr || group == nullptr || !name->is_string() || !group->is_string()) {
        return session.fail(std::string("GET ") + std::string(path) +
                            ": not a JSON object with string \"name\" and \"resourceGroupName\"");
    }
    return build<azure_vm_node_id>(session, std::string(group->as_string()),
                                   std::string(name->as_string()));
}

inline auto gcp_lookup(metadata_session& session) -> node_id_result<std::string> {
    const httplib::Headers flavor{{"Metadata-Flavor", "Google"}};
    auto get = [&](std::string_view path) {
        return session.request(path, flavor, false, "Metadata-Flavor", "Google");
    };
    auto name = get("/computeMetadata/v1/instance/name");
    if (!name) {
        return name;
    }
    // "projects/<project number>/zones/<zone>": the number is not the
    // project id the scope needs, which is why project-id is read too.
    auto zone_path = get("/computeMetadata/v1/instance/zone");
    if (!zone_path) {
        return zone_path;
    }
    const auto& zp = *zone_path;
    const auto marker = zp.rfind("/zones/");
    if (!zp.starts_with("projects/") || marker == std::string::npos || marker + 7 >= zp.size()) {
        return session.fail(
            "GET /computeMetadata/v1/instance/zone: expected "
            "projects/<n>/zones/<zone>, got '" +
            zp + "'");
    }
    auto project = get("/computeMetadata/v1/project/project-id");
    if (!project) {
        return project;
    }
    return build<gcp_instance_node_id>(session, *project + "/" + zp.substr(marker + 7),
                                       std::move(name).value());
}

inline auto oci_lookup(metadata_session& session) -> node_id_result<std::string> {
    auto ocid = session.request("/opc/v2/instance/id", {{"Authorization", "Bearer Oracle"}});
    if (!ocid) {
        return ocid;
    }
    const auto region = oci_instance_rules::region_of(*ocid);
    if (!region) {
        return session.fail("GET /opc/v2/instance/id: not an instance OCID: '" + *ocid + "'");
    }
    return build<oci_instance_node_id>(session, std::string(*region), std::move(ocid).value());
}

}  // namespace self_node_id_detail

/// @brief This host's composite node id as canonical text, read from
///        `source`'s metadata service.
///
/// Opt-in and bounded: it is only ever called for a provider the operator
/// named, and returns an error, never hangs, within `options.timeout`. Never
/// throws for a network or service failure.
[[nodiscard]] inline auto self_node_id_text(node_id_source source,
                                            const self_node_id_options& options = {})
    -> node_id_result<std::string> {
    using namespace self_node_id_detail;
    metadata_session session(source, options);
    switch (source) {
        case node_id_source::aws_ec2:
            return token_lookup<aws_ec2_node_id>(
                session, "X-aws-ec2-metadata-token-ttl-seconds", "X-aws-ec2-metadata-token",
                "/latest/meta-data/instance-id", "/latest/meta-data/placement/region");
        case node_id_source::alibaba_ecs:
            return token_lookup<alibaba_ecs_node_id>(
                session, "X-aliyun-ecs-metadata-token-ttl-seconds", "X-aliyun-ecs-metadata-token",
                "/latest/meta-data/instance-id", "/latest/meta-data/region-id");
        case node_id_source::azure_vm:
            return azure_lookup(session);
        case node_id_source::gcp:
            return gcp_lookup(session);
        case node_id_source::oci:
            return oci_lookup(session);
    }
    return session.fail("unknown source");
}

/// @brief This host's node id as `NodeId`, read from `source`'s metadata
///        service (Requirement 6.1).
///
/// `NodeId` is `std::string` (canonical text) or a composite type. A
/// composite type must be the one `source` produces: asking Azure for an
/// `aws_ec2_node_id` is an error, not a conversion.
template<textual_node_id NodeId>
[[nodiscard]] auto self_node_id(node_id_source source, const self_node_id_options& options = {})
    -> node_id_result<NodeId> {
    auto text = self_node_id_text(source, options);
    if (!text) {
        return node_id_result<NodeId>::failure(text.error());
    }
    if (auto id = node_id_traits<NodeId>::from_text(*text)) {
        return std::move(*id);
    }
    return node_id_result<NodeId>::failure("metadata:" + std::string(node_id_source_name(source)) +
                                           ": '" + *text +
                                           "' is not an id of the requested node id type");
}

}  // namespace kythira
