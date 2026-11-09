// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file
/// @brief The JWKS cache AssumeRoleWithWebIdentity verifies tokens with
///        (Requirement 11.3-11.4, design Component 10).
///
/// An OIDC provider registered in IAM names an issuer (`host/path`). Its
/// signing keys are found the way every relying party finds them: GET
/// `https://<issuer>/.well-known/openid-configuration`, read `jwks_uri`,
/// GET that. Keys are cached by issuer for at most an hour, and an unknown
/// `kid` refetches once (no more often than `refetch_floor`, so a stream
/// of tokens with made-up `kid`s cannot turn this endpoint into a
/// request amplifier against the issuer).
///
/// The fetch is the server-side request forgery surface of the IAM
/// endpoint: anyone may call AssumeRoleWithWebIdentity, and a provider's
/// URL is whatever an account's administrator registered. So:
///  - only `https` URLs are fetched, the discovery document's `jwks_uri`
///    included;
///  - with an allowlist, only its hosts are fetched, and a name that
///    resolves to a private, loopback, link-local or otherwise
///    non-public address is refused;
///  - the connection goes to the address that was checked (the client is
///    pinned to it), so a second DNS answer cannot rebind the name to
///    169.254.169.254 between the check and the connect;
///  - 5 s timeout and 64 KiB per response.
///
/// The fetch itself is a `fetcher` the caller supplies, so the cache and
/// its rules are tested without a network; `https_fetcher` is the
/// cpp-httplib one `iam_node` uses.

#include <raft/jwt.hpp>

#include <boost/json.hpp>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if __has_include(<httplib.h>) && defined(CPPHTTPLIB_OPENSSL_SUPPORT)
#include <httplib.h>
#define KYTHIRA_IAM_HAS_HTTPS_FETCHER 1
#endif

namespace kythira::iam::oidc {

/// What may be fetched, and how.
struct fetch_rules {
    /// Hosts that may be fetched. Empty: any host, and private addresses
    /// are not refused (a test issuer in a compose network is private).
    std::vector<std::string> allowed_hosts;
    std::chrono::milliseconds timeout{5000};
    std::size_t max_bytes = 64 * 1024;
    /// An HTTPS proxy, `host:port`; empty for none. With a proxy, the
    /// proxy resolves and connects; the host checks still apply.
    std::string proxy;
    /// A PEM bundle to trust instead of the system store (tests only).
    std::string ca_file;
};

/// One fetched document, or why it could not be.
struct fetch_result {
    int status = 0;
    std::string body;
    std::string error;  // non-empty: the fetch failed before a response
};

using fetcher = std::function<fetch_result(const std::string& url)>;

namespace detail {

[[nodiscard]] inline auto lower(std::string_view s) -> std::string {
    std::string out(s);
    std::ranges::transform(out, out.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

[[nodiscard]] inline auto v4_forbidden(std::uint32_t a) -> bool {
    const auto in = [&](std::uint32_t net, int bits) {
        const std::uint32_t mask = bits == 0 ? 0 : ~std::uint32_t{0} << (32 - bits);
        return (a & mask) == net;
    };
    return in(0x00000000, 8)      // "this" network
           || in(0x0A000000, 8)   // 10/8
           || in(0x64400000, 10)  // 100.64/10, carrier-grade NAT
           || in(0x7F000000, 8)   // loopback
           || in(0xA9FE0000, 16)  // 169.254/16, link-local and metadata services
           || in(0xAC100000, 12)  // 172.16/12
           || in(0xC0000000, 24)  // 192.0.0/24, IETF protocol assignments
           || in(0xC0A80000, 16)  // 192.168/16
           || in(0xC6120000, 15)  // 198.18/15, benchmarking
           || in(0xE0000000, 4)   // multicast
           || in(0xF0000000, 4);  // reserved, broadcast
}

}  // namespace detail

/// Whether `ip` (a numeric IPv4 or IPv6 address) is one a fetch may not
/// reach when an allowlist is set: anything but a public unicast address.
/// An IPv4-mapped or -compatible IPv6 address is judged as its IPv4 one,
/// so `::ffff:169.254.169.254` is refused like `169.254.169.254`.
/// Unparseable text is refused.
[[nodiscard]] inline auto address_forbidden(std::string_view ip) -> bool {
    const std::string text(ip);
    in_addr v4{};
    if (inet_pton(AF_INET, text.c_str(), &v4) == 1) {
        return detail::v4_forbidden(ntohl(v4.s_addr));
    }
    in6_addr v6{};
    if (inet_pton(AF_INET6, text.c_str(), &v6) != 1) {
        return true;
    }
    const auto* b = v6.s6_addr;
    const bool first_ten_zero = std::all_of(b, b + 10, [](unsigned char c) { return c == 0; });
    if (first_ten_zero && ((b[10] == 0xff && b[11] == 0xff) || (b[10] == 0 && b[11] == 0))) {
        const std::uint32_t a = (std::uint32_t{b[12]} << 24) | (std::uint32_t{b[13]} << 16) |
                                (std::uint32_t{b[14]} << 8) | std::uint32_t{b[15]};
        if (b[10] == 0 && b[11] == 0 && a <= 1) {
            return true;  // :: and ::1
        }
        return detail::v4_forbidden(a);
    }
    return (b[0] & 0xfe) == 0xfc                       // fc00::/7, unique local
           || (b[0] == 0xfe && (b[1] & 0xc0) == 0x80)  // fe80::/10, link-local
           || b[0] == 0xff                             // multicast
           || (b[0] == 0x20 && b[1] == 0x01 && b[2] == 0x0d && b[3] == 0xb8)   // documentation
           || (b[0] == 0x00 && b[1] == 0x64 && b[2] == 0xff && b[3] == 0x9b);  // NAT64
}

/// The parts of an `https://host[:port]/path` URL.
struct https_url {
    std::string host;  // lower-cased
    int port = 443;
    std::string path;  // with its leading slash, "/" at least
};

/// `url` split, or why it may not be fetched: not `https`, no host, or
/// (with an allowlist) a host not on it.
[[nodiscard]] inline auto check_url(std::string_view url, const fetch_rules& rules)
    -> std::pair<std::optional<https_url>, std::string> {
    constexpr std::string_view scheme = "https://";
    if (!detail::lower(url.substr(0, scheme.size())).starts_with(scheme)) {
        return {std::nullopt, "only https URLs are fetched"};
    }
    const auto rest = url.substr(scheme.size());
    const auto slash = rest.find('/');
    const auto authority = rest.substr(0, slash);
    if (authority.empty() || authority.find('@') != std::string_view::npos) {
        return {std::nullopt, "the URL has no host"};
    }
    https_url out;
    out.path = slash == std::string_view::npos ? "/" : std::string(rest.substr(slash));
    if (const auto q = out.path.find_first_of("?#"); q != std::string::npos) {
        out.path.resize(q);
    }
    auto host = authority;
    if (host.front() == '[') {  // [v6]:port
        const auto close = host.find(']');
        if (close == std::string_view::npos) {
            return {std::nullopt, "the URL's host is malformed"};
        }
        if (close + 1 < host.size()) {
            if (host[close + 1] != ':') {
                return {std::nullopt, "the URL's host is malformed"};
            }
            out.port = std::atoi(std::string(host.substr(close + 2)).c_str());
        }
        host = host.substr(1, close - 1);
    } else if (const auto colon = host.rfind(':'); colon != std::string_view::npos) {
        out.port = std::atoi(std::string(host.substr(colon + 1)).c_str());
        host = host.substr(0, colon);
    }
    if (host.empty() || out.port <= 0 || out.port > 65535) {
        return {std::nullopt, "the URL's host or port is malformed"};
    }
    out.host = detail::lower(host);
    if (!rules.allowed_hosts.empty() &&
        std::ranges::none_of(rules.allowed_hosts,
                             [&](const auto& h) { return detail::lower(h) == out.host; })) {
        return {std::nullopt, "host " + out.host + " is not on the OIDC fetch allowlist"};
    }
    return {std::move(out), {}};
}

/// Resolves `host` and returns the address to connect to, or why none may
/// be. With an allowlist every address the name resolves to must be
/// public: one private answer refuses the name, so a name cannot mix a
/// public and a metadata address and hope for the second.
[[nodiscard]] inline auto resolve_checked(const std::string& host, const fetch_rules& rules)
    -> std::pair<std::optional<std::string>, std::string> {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (const int rc = getaddrinfo(host.c_str(), nullptr, &hints, &res); rc != 0) {
        return {std::nullopt, "cannot resolve " + host + ": " + gai_strerror(rc)};
    }
    const std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> owner(res, &freeaddrinfo);
    std::vector<std::string> addrs;
    for (const auto* ai = res; ai != nullptr; ai = ai->ai_next) {
        std::array<char, INET6_ADDRSTRLEN> buf{};
        const void* src = nullptr;
        if (ai->ai_family == AF_INET) {
            src = &reinterpret_cast<const sockaddr_in*>(ai->ai_addr)->sin_addr;  // NOLINT
        } else if (ai->ai_family == AF_INET6) {
            src = &reinterpret_cast<const sockaddr_in6*>(ai->ai_addr)->sin6_addr;  // NOLINT
        } else {
            continue;
        }
        if (inet_ntop(ai->ai_family, src, buf.data(), buf.size()) != nullptr) {
            addrs.emplace_back(buf.data());
        }
    }
    if (addrs.empty()) {
        return {std::nullopt, "no address for " + host};
    }
    if (!rules.allowed_hosts.empty()) {
        for (const auto& a : addrs) {
            if (address_forbidden(a)) {
                return {std::nullopt,
                        host + " resolves to " + a + ", which is not a public address"};
            }
        }
    }
    return {addrs.front(), {}};
}

#ifdef KYTHIRA_IAM_HAS_HTTPS_FETCHER
/// The fetcher `iam_node` uses: cpp-httplib over TLS with the system
/// trust store (or `rules.ca_file`), pinned to the checked address.
[[nodiscard]] inline auto https_fetcher(fetch_rules rules) -> fetcher {
    return [rules = std::move(rules)](const std::string& url) -> fetch_result {
        const auto [parsed, why] = check_url(url, rules);
        if (!parsed) {
            return {0, {}, why};
        }
        httplib::SSLClient client(parsed->host, parsed->port);
        if (rules.proxy.empty()) {
            const auto [addr, bad] = resolve_checked(parsed->host, rules);
            if (!addr) {
                return {0, {}, bad};
            }
            // Connect to the checked address; SNI and the certificate
            // check still use the name.
            client.set_hostname_addr_map({{parsed->host, *addr}});
        } else {
            if (!rules.allowed_hosts.empty()) {
                // The proxy resolves for itself; still refuse a name that
                // resolves privately here.
                const auto [addr, bad] = resolve_checked(parsed->host, rules);
                if (!addr) {
                    return {0, {}, bad};
                }
            }
            const auto colon = rules.proxy.rfind(':');
            client.set_proxy(rules.proxy.substr(0, colon),
                             colon == std::string::npos
                                 ? 3128
                                 : std::atoi(rules.proxy.substr(colon + 1).c_str()));
        }
        client.enable_server_certificate_verification(true);
        if (!rules.ca_file.empty()) {
            client.set_ca_cert_path(rules.ca_file);
        }
        const auto secs = std::chrono::duration_cast<std::chrono::seconds>(rules.timeout);
        const auto usecs =
            std::chrono::duration_cast<std::chrono::microseconds>(rules.timeout - secs);
        client.set_connection_timeout(secs.count(), usecs.count());
        client.set_read_timeout(secs.count(), usecs.count());
        client.set_write_timeout(secs.count(), usecs.count());
        client.set_follow_location(false);
        fetch_result out;
        bool too_big = false;
        const auto res = client.Get(
            parsed->path,
            [&](const httplib::Response& r) {
                out.status = r.status;
                return true;
            },
            [&](const char* data, std::size_t len) {
                if (out.body.size() + len > rules.max_bytes) {
                    too_big = true;
                    return false;
                }
                out.body.append(data, len);
                return true;
            });
        if (too_big) {
            return {0, {}, "the response exceeds " + std::to_string(rules.max_bytes) + " bytes"};
        }
        if (!res) {
            return {0, {}, "fetch failed: " + httplib::to_string(res.error())};
        }
        return out;
    };
}
#endif

/// Why a key could not be found for a token.
enum class key_error : std::uint8_t {
    unavailable,  // the issuer's documents could not be fetched or read
    unknown_kid,  // fetched, and no key has this kid
};

/// Signing keys by issuer, fetched through `fetcher`.
class jwks_cache {
public:
    using clock_fn = std::function<std::chrono::system_clock::time_point()>;

    static constexpr auto k_max_age = std::chrono::hours(1);
    static constexpr auto k_refetch_floor = std::chrono::seconds(30);
    static constexpr std::size_t k_max_issuers = 256;
    static constexpr std::size_t k_max_keys = 64;

    explicit jwks_cache(fetcher fetch, clock_fn clock = std::chrono::system_clock::now)
        : _fetch(std::move(fetch)), _clock(std::move(clock)) {}

    /// The JWK `kid` names for `issuer` (`host/path`, no scheme), fetching
    /// when nothing fresh is cached and refetching once for an unknown kid.
    [[nodiscard]] auto key(const std::string& issuer, const std::string& kid)
        -> std::pair<std::optional<boost::json::object>, key_error> {
        const std::lock_guard lock(_mu);
        const auto now = _clock();
        auto it = _by_issuer.find(issuer);
        bool fetched_now = false;
        if (it == _by_issuer.end() || now - it->second.fetched_at >= k_max_age) {
            if (!refresh(issuer, now)) {
                return {std::nullopt, key_error::unavailable};
            }
            fetched_now = true;
            it = _by_issuer.find(issuer);
        }
        if (auto k = find(it->second, kid)) {
            return {std::move(k), key_error::unknown_kid};
        }
        // An unknown kid: the issuer may have rotated. Refetch once, and no
        // more often than the floor.
        if (fetched_now || now - it->second.last_miss_refetch < k_refetch_floor) {
            return {std::nullopt, key_error::unknown_kid};
        }
        it->second.last_miss_refetch = now;
        if (!refresh(issuer, now)) {
            return {std::nullopt, key_error::unavailable};
        }
        it = _by_issuer.find(issuer);
        if (auto k = find(it->second, kid)) {
            return {std::move(k), key_error::unknown_kid};
        }
        return {std::nullopt, key_error::unknown_kid};
    }

    /// Reads `issuer`'s keys straight from `jwks_url` with `fetch`, with no
    /// discovery step: `iam_node --kythira-issuer-local`, where the Kythira
    /// issuer's JWKS is read from a CA member over the cluster's own mutual
    /// TLS rather than from the public bucket the discovery document
    /// names. Operator configuration, so the allowlist does not apply.
    void set_direct(const std::string& issuer, std::string jwks_url, fetcher fetch) {
        const std::lock_guard lock(_mu);
        _direct[issuer] = {std::move(jwks_url), std::move(fetch)};
        _by_issuer.erase(issuer);
    }

    /// How many documents have been fetched (tests).
    [[nodiscard]] auto fetches() const -> std::size_t {
        const std::lock_guard lock(_mu);
        return _fetches;
    }

private:
    struct entry {
        std::map<std::string, boost::json::object> keys;  // by kid
        std::chrono::system_clock::time_point fetched_at;
        std::chrono::system_clock::time_point last_miss_refetch{};
    };

    [[nodiscard]] static auto find(const entry& e, const std::string& kid)
        -> std::optional<boost::json::object> {
        const auto k = e.keys.find(kid);
        if (k == e.keys.end()) {
            return std::nullopt;
        }
        return k->second;
    }

    [[nodiscard]] auto get_json(const std::string& url, const fetcher& fetch)
        -> std::optional<boost::json::object> {
        ++_fetches;
        const auto r = fetch(url);
        if (!r.error.empty() || r.status != 200) {
            return std::nullopt;
        }
        boost::system::error_code ec;
        auto v = boost::json::parse(r.body, ec);
        if (ec || !v.is_object()) {
            return std::nullopt;
        }
        return std::move(v.as_object());
    }

    /// The JWKS through discovery: the document must name this issuer
    /// (OpenID Connect Discovery 1.0 section 4.3) and an https `jwks_uri`.
    [[nodiscard]] auto discover(const std::string& issuer) -> std::optional<boost::json::object> {
        const auto iss = "https://" + issuer;
        auto discovery = get_json(iss + "/.well-known/openid-configuration", _fetch);
        if (!discovery) {
            return std::nullopt;
        }
        const auto* named = discovery->if_contains("issuer");
        const auto* uri = discovery->if_contains("jwks_uri");
        if (named == nullptr || !named->is_string() || uri == nullptr || !uri->is_string()) {
            return std::nullopt;
        }
        auto named_text = std::string(named->as_string());
        while (named_text.ends_with('/')) {
            named_text.pop_back();
        }
        if (named_text != iss) {
            return std::nullopt;
        }
        return get_json(std::string(uri->as_string()), _fetch);
    }

    /// Discovery, then the JWKS, or the JWKS directly for a `set_direct`
    /// issuer.
    auto refresh(const std::string& issuer, std::chrono::system_clock::time_point now) -> bool {
        std::optional<boost::json::object> jwks;
        if (const auto d = _direct.find(issuer); d != _direct.end()) {
            jwks = get_json(d->second.first, d->second.second);
        } else {
            jwks = discover(issuer);
        }
        if (!jwks) {
            return false;
        }
        const auto* keys = jwks->if_contains("keys");
        if (keys == nullptr || !keys->is_array()) {
            return false;
        }
        entry e;
        e.fetched_at = now;
        for (const auto& k : keys->as_array()) {
            if (!k.is_object() || e.keys.size() >= k_max_keys) {
                continue;
            }
            const auto& o = k.as_object();
            const auto* kid = o.if_contains("kid");
            const auto* use = o.if_contains("use");
            if (kid == nullptr || !kid->is_string() ||
                (use != nullptr && (!use->is_string() || use->as_string() != "sig"))) {
                continue;
            }
            e.keys.emplace(std::string(kid->as_string()), o);
        }
        if (const auto old = _by_issuer.find(issuer); old != _by_issuer.end()) {
            e.last_miss_refetch = old->second.last_miss_refetch;
        }
        if (_by_issuer.size() >= k_max_issuers && !_by_issuer.contains(issuer)) {
            const auto oldest = std::ranges::min_element(
                _by_issuer, {}, [](const auto& kv) { return kv.second.fetched_at; });
            _by_issuer.erase(oldest);
        }
        _by_issuer[issuer] = std::move(e);
        return true;
    }

    fetcher _fetch;
    clock_fn _clock;
    mutable std::mutex _mu;
    std::map<std::string, entry> _by_issuer;
    std::map<std::string, std::pair<std::string, fetcher>> _direct;
    std::size_t _fetches = 0;
};

}  // namespace kythira::iam::oidc
