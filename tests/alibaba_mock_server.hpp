#pragma once

/// @file alibaba_mock_server.hpp
/// @brief An in-memory stand-in for the Alibaba Cloud routes
///        `alibaba_ess_quorum_manager` and `alibaba_oss_persistence_engine`
///        actually call (`.kiro/specs/alibaba-cloud-services/`, Requirement
///        16.6) — ESS + ECS on the control plane, an OSS bucket on the data
///        plane, one `httplib::Server` for both.
///
/// Alibaba has no LocalStack-analogue emulator, so this tier has to be built,
/// exactly as `tests/oci_mock_server.hpp` had to be built for OCI and
/// `tests/acme_test_server.hpp` for ACME.
///
/// **What it models and what it does not.** It models *state machines and call
/// sequences*: DesiredCapacity drives instance launches, `RemoveInstances`
/// terminates and shrinks in one step, `TagResources` merges keys, an object
/// PUT is visible to the next GET, a listing paginates by continuation token.
///
/// **It verifies request signatures, and that is the entire reason it exists
/// in this shape.** The OCI sibling's header records why: a mock that
/// re-derived the signature by calling the client's own signing code would be
/// "verifying the client against a second copy of the client's own logic", and
/// acting on that reasoning is what let two real OCI defects ship past 31
/// green tests — the client signed one `Host` and sent another, and the
/// endpoint domain was wrong for every service. Both were found only by
/// calling live Oracle.
///
/// This provider then paid the same tuition a second time, in the same week
/// this file was written. `alibaba_oss_client`'s first live `PutObject`
/// returned `SignatureDoesNotMatch`: OSS V4 signs `Content-Type` when the
/// request carries one, cpp-httplib's body overload puts `Content-Type` on the
/// wire whether or not the caller asked for it, and the client's signed set
/// omitted it. Every local test was green, because no local test compared what
/// was *sent* against what was *signed* (spike-notes.md Finding 2). OSS itself
/// named the defect — its 403 body echoes the CanonicalRequest the server
/// computed, which differed from the client's by exactly one
/// `content-type:application/octet-stream` line. This mock echoes the same
/// thing for the same reason.
///
/// So: **every canonical request here is reconstructed from the bytes that
/// actually arrived** — the raw request-target off the request line, the
/// `Host`/`x-acs-*`/`x-oss-*`/`Content-Type` headers as received, the date the
/// request carries, the body as received — and the HMAC is recomputed over
/// that with the configured AccessKeySecret. Nothing in this file calls
/// `alibaba_signing::sign_request` or `alibaba_oss_client::prepared_headers`;
/// the primitives below are local OpenSSL wrappers precisely so that a shared
/// bug in the client's canonical-form assembly cannot cancel itself out. The
/// question this tier answers is the one golden vectors structurally cannot:
/// *does what we sent match what we signed?*
///
/// Both schemes are covered, because this provider has two of them and they
/// evolve independently on the vendor side (design.md's two-signers decision):
///
///  * **ACS3-HMAC-SHA256** for the RPC control plane. The signed header set is
///    taken from the request's own `SignedHeaders=`, *and* the request is
///    rejected if that list omits `host` or any `x-acs-*` header the request
///    actually carries — the ACS3 analogue of the Content-Type hole above.
///  * **OSS4-HMAC-SHA256** for the object store. Here the signed set is
///    **derived from the received headers** by the vendor's rule
///    (`content-md5`/`content-type` when present, plus every `x-oss-*`), not
///    read off the request, which is exactly what makes the live defect
///    reproducible locally: withhold `content-type` from the signature, leave
///    it on the wire, and this server answers 403.
///
/// **One deliberate exception to byte-strictness: the canonical query string.**
/// It is rebuilt from the *parsed* parameters and re-encoded with the vendor's
/// strict RFC 3986 rule, not copied verbatim off the wire. That is not
/// laziness — it is what the real services do (both schemes define the
/// canonical query over the parameter set, and the server necessarily parses
/// before it canonicalises), and being stricter than the real service would
/// manufacture failures. It has to be, because **cpp-httplib rewrites the
/// query string it is handed**: `ClientImpl::process_request` splits the path
/// at `?`, decodes the query into `Params`, and re-encodes it with its own
/// encoder, which leaves `, ! $ ' ( ) * ; /` literal where Alibaba's rule
/// percent-encodes them. Measured on cpp-httplib 0.27.0: a client that signs
/// `InstanceIds=%5B%22i-1%22%2C%22i-2%22%5D` puts
/// `InstanceIds=%5B%22i-1%22,%22i-2%22%5D` on the wire. The parameters survive
/// that round trip unchanged, which is why both services still accept it — but
/// a byte-strict mock would reject every multi-instance `DescribeInstances`.
///
/// What that leaves unverified, stated plainly: this tier cannot prove Alibaba
/// canonicalises from the parsed parameters rather than from the raw query
/// bytes. It is what every documented signing scheme of this family does and
/// what AWS SigV4 provably does, but it is a documentation-level claim here,
/// and the one parameter shape it could bite is a value containing a space —
/// signed `%20`, put on the wire by cpp-httplib as `+`. Nothing this provider
/// sends today contains a space (an operator's `extra_tags` value is the only
/// way in). That belongs in spike task 0.2's live half, not in a mock.
///
/// **`TagResources` is additive, on purpose.** ECS creates-or-overwrites
/// exactly the keys it is given and leaves the rest alone. A mock that
/// replaced the whole map would make design.md Property 4 untestable and would
/// invite the read-merge-write cycle `alibaba_ess_quorum_manager`'s header
/// argues against — the exact mirror image of `oci_mock_server`, which models
/// replacement because OCI replaces.
///
/// **The OSS side is path-style only** (`/<bucket>/<key>`), because that is the
/// only addressing `alibaba_oss_client` emits under `endpoint_override`
/// (Requirement 2.3). A virtual-host route would be dead code that quietly
/// diverged.
///
/// One server answers for ESS, ECS and OSS at once, because
/// `alibaba_client_config::endpoint_override` replaces the host for all of them
/// simultaneously. The route patterns are globally distinct: the control plane
/// is `POST /`, the object store is `PUT`/`GET`/`DELETE` on `/{bucket}/{key}`.

#include <httplib.h>

#include <boost/json.hpp>

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace kythira::testing {

namespace alibaba_mock_detail {

inline constexpr const char* k_lower_hex = "0123456789abcdef";
inline constexpr const char* k_upper_hex = "0123456789ABCDEF";

/// Lowercase-hex SHA-256. Local rather than borrowed from
/// `alibaba_signing::detail` so that no part of the verification path can share
/// a bug with the code under test.
[[nodiscard]] inline auto sha256_hex(std::string_view data) -> std::string {
    std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
    SHA256(reinterpret_cast<const unsigned char*>(data.data()), data.size(), digest.data());
    std::string out;
    out.reserve(2 * digest.size());
    for (const unsigned char b : digest) {
        out.push_back(k_lower_hex[b >> 4U]);
        out.push_back(k_lower_hex[b & 0x0FU]);
    }
    return out;
}

/// Raw (binary) HMAC-SHA256 — the OSS V4 key-derivation chain feeds one
/// HMAC's output into the next as key material, so the raw form is the useful
/// one and hex is applied only at the end.
[[nodiscard]] inline auto hmac_sha256(std::string_view key, std::string_view msg) -> std::string {
    std::array<unsigned char, EVP_MAX_MD_SIZE> mac{};
    unsigned int mac_len = 0;
    HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
         reinterpret_cast<const unsigned char*>(msg.data()), msg.size(), mac.data(), &mac_len);
    return {reinterpret_cast<const char*>(mac.data()), mac_len};
}

[[nodiscard]] inline auto to_hex(std::string_view raw) -> std::string {
    std::string out;
    out.reserve(2 * raw.size());
    for (const char c : raw) {
        const auto b = static_cast<unsigned char>(c);
        out.push_back(k_lower_hex[b >> 4U]);
        out.push_back(k_lower_hex[b & 0x0FU]);
    }
    return out;
}

[[nodiscard]] inline auto hmac_sha256_hex(std::string_view key, std::string_view msg)
    -> std::string {
    return to_hex(hmac_sha256(key, msg));
}

/// RFC 3986 percent-encoding, unreserved literal and uppercase `%XX` for
/// everything else — the rule both Alibaba schemes canonicalise query
/// parameters with.
[[nodiscard]] inline auto percent_encode(std::string_view in) -> std::string {
    std::string out;
    out.reserve(in.size());
    for (const char c : in) {
        const auto b = static_cast<unsigned char>(c);
        const bool unreserved =
            (std::isalnum(b) != 0) || b == '-' || b == '_' || b == '.' || b == '~';
        if (unreserved) {
            out.push_back(c);
        } else {
            out.push_back('%');
            out.push_back(k_upper_hex[b >> 4U]);
            out.push_back(k_upper_hex[b & 0x0FU]);
        }
    }
    return out;
}

[[nodiscard]] inline auto to_lower(std::string_view in) -> std::string {
    std::string out;
    out.reserve(in.size());
    for (const char c : in) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

[[nodiscard]] inline auto split(std::string_view in, char sep) -> std::vector<std::string> {
    std::vector<std::string> out;
    std::size_t pos = 0;
    while (pos <= in.size()) {
        const auto next = in.find(sep, pos);
        if (next == std::string_view::npos) {
            out.emplace_back(in.substr(pos));
            break;
        }
        out.emplace_back(in.substr(pos, next - pos));
        pos = next + 1;
    }
    return out;
}

/// The request-target's path part, **as it arrived on the request line** —
/// `httplib::Request::path` is percent-*decoded*, which would silently repair a
/// client that encoded its resource differently from the way it signed it.
[[nodiscard]] inline auto raw_path_of(const httplib::Request& req) -> std::string {
    const auto& target = req.target.empty() ? req.path : req.target;
    const auto query = target.find('?');
    return query == std::string::npos ? target : target.substr(0, query);
}

/// Minimal XML text escaping — the OSS error bodies below embed a whole
/// canonical request, which routinely contains `&` from the query string.
[[nodiscard]] inline auto xml_escape(std::string_view in) -> std::string {
    std::string out;
    out.reserve(in.size());
    for (const char c : in) {
        switch (c) {
            case '&':
                out += "&amp;";
                break;
            case '<':
                out += "&lt;";
                break;
            case '>':
                out += "&gt;";
                break;
            default:
                out.push_back(c);
                break;
        }
    }
    return out;
}

/// One `Name=value` element out of an `Authorization` header's comma-separated
/// parameter list (`Credential=...,SignedHeaders=...,Signature=...`). Alibaba
/// quotes none of these, unlike OCI's `name="value"` form.
[[nodiscard]] inline auto auth_param(std::string_view header, std::string_view name)
    -> std::string {
    for (const auto& part : split(header, ',')) {
        std::string_view piece{part};
        while (!piece.empty() && (piece.front() == ' ' || piece.front() == '\t')) {
            piece.remove_prefix(1);
        }
        if (piece.size() > name.size() && piece.substr(0, name.size()) == name &&
            piece[name.size()] == '=') {
            return std::string(piece.substr(name.size() + 1));
        }
    }
    return {};
}

}  // namespace alibaba_mock_detail

/// @brief A fake Alibaba Cloud control plane (ESS + ECS) and OSS bucket over
///        one `httplib::Server`, with live signature verification.
///
/// Every accessor takes the same lock the request handlers do, so a test can
/// inspect or mutate state while the server is running without racing it.
class alibaba_mock_server {
public:
    /// One scaling-group member, as the ESS and ECS halves jointly report it.
    struct instance_state {
        std::string id;
        /// ESS lifecycle: `InService`, `Pending`, `Removing`, ...
        std::string lifecycle_state{"InService"};
        /// ECS status: `Running`, `Starting`, `Stopped`, ...
        std::string status{"Running"};
        std::string zone_id;
        std::string private_ip;
        std::map<std::string, std::string> tags;
    };

    explicit alibaba_mock_server(std::string scaling_group_id = "asg-kythira-mock",
                                 std::string zone_id = "cn-hangzhou-b",
                                 std::string bucket = "kythira-mock")
        : _group_id(std::move(scaling_group_id)),
          _zone_id(std::move(zone_id)),
          _bucket(std::move(bucket)),
          _launch_zone(_zone_id) {
        install_routes();
    }

    alibaba_mock_server(const alibaba_mock_server&) = delete;
    auto operator=(const alibaba_mock_server&) -> alibaba_mock_server& = delete;
    alibaba_mock_server(alibaba_mock_server&&) = delete;
    auto operator=(alibaba_mock_server&&) -> alibaba_mock_server& = delete;

    ~alibaba_mock_server() { stop(); }

    /// Binds an ephemeral port and serves on a background thread. Ephemeral
    /// rather than fixed: a fixed port turns an unlucky `TIME_WAIT` into a
    /// flaky suite, and nothing here needs a predictable port.
    auto start() -> void {
        _port = _server.bind_to_any_port("127.0.0.1");
        _thread = std::thread([this] { _server.listen_after_bind(); });
        for (int i = 0; i < 400 && !_server.is_running(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    auto stop() -> void {
        _server.stop();
        if (_thread.joinable()) {
            _thread.join();
        }
    }

    /// The `endpoint_override` a client should be pointed at.
    [[nodiscard]] auto origin() const -> std::string {
        return "http://127.0.0.1:" + std::to_string(_port);
    }

    // ── Credentials ──────────────────────────────────────────────────────────

    /// The AccessKey pair every request must sign with. A request whose
    /// `Credential=` names an unknown AccessKeyId is refused with the vendor's
    /// own `InvalidAccessKeyId` shape rather than a generic 403, because a
    /// client that sends the wrong key and a client that computes the wrong
    /// signature are different bugs.
    auto set_credentials(std::string access_key_id, std::string access_key_secret) -> void {
        const std::lock_guard lock(_mutex);
        _access_key_id = std::move(access_key_id);
        _access_key_secret = std::move(access_key_secret);
    }

    /// When set, a request must also carry this `x-acs-security-token` /
    /// `x-oss-security-token`; when empty (the default) a request that carries
    /// one is still accepted, since the token is part of the signed set either
    /// way and the signature is what is actually being checked.
    auto set_security_token(std::string token) -> void {
        const std::lock_guard lock(_mutex);
        _security_token = std::move(token);
    }

    [[nodiscard]] auto access_key_id() const -> std::string {
        const std::lock_guard lock(_mutex);
        return _access_key_id;
    }

    [[nodiscard]] auto access_key_secret() const -> std::string {
        const std::lock_guard lock(_mutex);
        return _access_key_secret;
    }

    /// How many requests have been refused for a bad or missing signature.
    ///
    /// Asserting this is **zero** after a successful flow is what proves the
    /// happy path really went through verification rather than around it — a
    /// mock that silently accepts everything is worse than no mock at all.
    [[nodiscard]] auto signature_failures() const -> std::size_t {
        const std::lock_guard lock(_mutex);
        return _signature_failures;
    }

    /// The canonical request the server rebuilt for the most recent rejection,
    /// which is the diagnostic that turns a bare 403 into a five-minute fix.
    [[nodiscard]] auto last_rejected_canonical_request() const -> std::string {
        const std::lock_guard lock(_mutex);
        return _last_rejected_canonical;
    }

    // ── ESS / ECS knobs and state ────────────────────────────────────────────

    [[nodiscard]] auto scaling_group_id() const -> std::string { return _group_id; }
    [[nodiscard]] auto zone_id() const -> std::string { return _zone_id; }
    [[nodiscard]] auto bucket() const -> std::string { return _bucket; }

    auto set_group_exists(bool exists) -> void {
        const std::lock_guard lock(_mutex);
        _group_exists = exists;
    }

    /// Whether raising DesiredCapacity materialises instances. False is how a
    /// test reaches `provision_node`'s timeout-and-roll-back path: capacity goes
    /// up and nothing ever appears, which is what a real capacity shortfall
    /// looks like from the client's side.
    auto set_auto_launch(bool enabled) -> void {
        const std::lock_guard lock(_mutex);
        _auto_launch = enabled;
    }

    /// The zone ESS actually places new instances in. Set it away from a
    /// caller's `target_group` to exercise Requirement 7.3's proceed-and-log.
    auto set_launch_zone(std::string zone) -> void {
        const std::lock_guard lock(_mutex);
        _launch_zone = std::move(zone);
    }

    /// The ECS status a freshly launched instance reports. `Starting` models an
    /// instance ESS has admitted but which never comes up.
    auto set_launch_status(std::string status) -> void {
        const std::lock_guard lock(_mutex);
        _launch_status = std::move(status);
    }

    /// Tags a launched instance already carries before the manager adopts it —
    /// so an additive `TagResources` is distinguishable from a replacing one.
    auto set_launch_tags(std::map<std::string, std::string> tags) -> void {
        const std::lock_guard lock(_mutex);
        _launch_tags = std::move(tags);
    }

    auto set_min_size(std::int64_t size) -> void {
        const std::lock_guard lock(_mutex);
        _min_size = size;
    }

    auto set_max_size(std::int64_t size) -> void {
        const std::lock_guard lock(_mutex);
        _max_size = size;
    }

    /// Seed one instance already in the group. Returns its ECS instance ID.
    auto add_instance(std::map<std::string, std::string> tags,
                      std::string lifecycle_state = "InService", std::string status = "Running",
                      std::string zone = {}) -> std::string {
        const std::lock_guard lock(_mutex);
        auto id = launch_locked(std::move(tags), std::move(lifecycle_state), std::move(status),
                                zone.empty() ? _launch_zone : std::move(zone));
        _desired_capacity = static_cast<std::int64_t>(_instances.size());
        return id;
    }

    [[nodiscard]] auto instances() const -> std::vector<instance_state> {
        const std::lock_guard lock(_mutex);
        return _instances;
    }

    [[nodiscard]] auto instance(const std::string& id) const -> std::optional<instance_state> {
        const std::lock_guard lock(_mutex);
        for (const auto& inst : _instances) {
            if (inst.id == id) {
                return inst;
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] auto tags_of(const std::string& id) const -> std::map<std::string, std::string> {
        const auto found = instance(id);
        return found ? found->tags : std::map<std::string, std::string>{};
    }

    [[nodiscard]] auto instance_count() const -> std::size_t {
        const std::lock_guard lock(_mutex);
        return _instances.size();
    }

    [[nodiscard]] auto desired_capacity() const -> std::int64_t {
        const std::lock_guard lock(_mutex);
        return _desired_capacity;
    }

    auto set_instance_status(const std::string& id, std::string status) -> void {
        const std::lock_guard lock(_mutex);
        for (auto& inst : _instances) {
            if (inst.id == id) {
                inst.status = std::move(status);
                return;
            }
        }
    }

    auto set_instance_lifecycle(const std::string& id, std::string lifecycle) -> void {
        const std::lock_guard lock(_mutex);
        for (auto& inst : _instances) {
            if (inst.id == id) {
                inst.lifecycle_state = std::move(lifecycle);
                return;
            }
        }
    }

    // ── OSS state ────────────────────────────────────────────────────────────

    /// Seed or overwrite an object without going through the signed path — how
    /// a test plants the corrupt object Requirement 15.6 is about.
    auto oss_put(const std::string& key, std::string value) -> void {
        const std::lock_guard lock(_mutex);
        _objects[key] = std::move(value);
    }

    auto oss_erase(const std::string& key) -> void {
        const std::lock_guard lock(_mutex);
        _objects.erase(key);
    }

    [[nodiscard]] auto oss_has(const std::string& key) const -> bool {
        const std::lock_guard lock(_mutex);
        return _objects.contains(key);
    }

    [[nodiscard]] auto oss_body(const std::string& key) const -> std::string {
        const std::lock_guard lock(_mutex);
        const auto it = _objects.find(key);
        return it == _objects.end() ? std::string{} : it->second;
    }

    [[nodiscard]] auto oss_keys() const -> std::vector<std::string> {
        const std::lock_guard lock(_mutex);
        std::vector<std::string> out;
        out.reserve(_objects.size());
        for (const auto& [key, value] : _objects) {
            out.push_back(key);
        }
        return out;
    }

    /// Keys returned per `ListObjectsV2` page. The default is larger than any
    /// test's key set, so pagination is opt-in; setting it small is how the
    /// continuation-token walk gets exercised.
    auto set_oss_page_size(std::size_t keys) -> void {
        const std::lock_guard lock(_mutex);
        _oss_page_size = keys == 0 ? 1 : keys;
    }

    /// When non-zero, every PUT answers with this status instead of storing.
    auto set_oss_put_status(int status) -> void { _oss_put_status.store(status); }

    /// Fail this many further PUTs (503), then go back to storing — the
    /// transient failure the persistence engine's single retry rides out.
    auto set_oss_failing_puts(int count) -> void { _oss_failing_puts.store(count); }

    // ── Observation ──────────────────────────────────────────────────────────

    /// Every request the server accepted, in order, as
    /// `"RPC <Action>"` / `"OSS PUT <key>"` / `"OSS LIST <prefix>"`.
    [[nodiscard]] auto request_log() const -> std::vector<std::string> {
        const std::lock_guard lock(_mutex);
        return _requests;
    }

    [[nodiscard]] auto count_requests(std::string_view prefix) const -> std::size_t {
        const std::lock_guard lock(_mutex);
        std::size_t n = 0;
        for (const auto& entry : _requests) {
            if (entry.starts_with(prefix)) {
                ++n;
            }
        }
        return n;
    }

    auto clear_request_log() -> void {
        const std::lock_guard lock(_mutex);
        _requests.clear();
    }

private:
    // ─────────────────────────────────────────────────────────────────────────
    // Replies
    // ─────────────────────────────────────────────────────────────────────────

    static auto rpc_reply(httplib::Response& res, const boost::json::value& body, int status = 200)
        -> void {
        res.status = status;
        res.set_content(boost::json::serialize(body), "application/json");
    }

    /// The vendor's RPC error shape: capitalised `Code`/`Message`/`RequestId`.
    static auto rpc_error(httplib::Response& res, int status, const std::string& code,
                          const std::string& message) -> void {
        boost::json::object body;
        body["Code"] = code;
        body["Message"] = message;
        body["RequestId"] = "MOCK-REQUEST-ID";
        body["HostId"] = "mock.aliyuncs.com";
        res.status = status;
        res.set_content(boost::json::serialize(body), "application/json");
    }

    /// The vendor's OSS error shape: XML, with the optional diagnostic elements
    /// real OSS attaches to `SignatureDoesNotMatch`.
    static auto oss_error(httplib::Response& res, int status, const std::string& code,
                          const std::string& message,
                          const std::vector<std::pair<std::string, std::string>>& extra = {})
        -> void {
        std::string body = R"(<?xml version="1.0" encoding="UTF-8"?><Error><Code>)";
        body += alibaba_mock_detail::xml_escape(code);
        body += "</Code><Message>";
        body += alibaba_mock_detail::xml_escape(message);
        body += "</Message><RequestId>MOCK-REQUEST-ID</RequestId>";
        for (const auto& [name, value] : extra) {
            body += "<" + name + ">" + alibaba_mock_detail::xml_escape(value) + "</" + name + ">";
        }
        body += "</Error>";
        res.status = status;
        res.set_content(body, "application/xml");
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Canonicalisation — every input below comes off the wire
    // ─────────────────────────────────────────────────────────────────────────

    /// The canonical query string, rebuilt from the parameters the server
    /// parsed out of the received target and re-encoded with the vendor's
    /// strict RFC 3986 rule.
    ///
    /// See the file comment for why this one line is parameter-level rather
    /// than byte-level: cpp-httplib re-encodes the query it is handed, both
    /// services canonicalise from the parsed parameter set, and a byte-strict
    /// check here would reject requests real Alibaba accepts.
    [[nodiscard]] static auto canonical_query_of(const httplib::Request& req) -> std::string {
        std::map<std::string, std::string> sorted;
        for (const auto& [name, value] : req.params) {
            sorted.insert_or_assign(name, value);
        }
        std::string out;
        for (const auto& [name, value] : sorted) {
            if (!out.empty()) {
                out.push_back('&');
            }
            out += alibaba_mock_detail::percent_encode(name);
            out.push_back('=');
            out += alibaba_mock_detail::percent_encode(value);
        }
        return out;
    }

    /// `"{lowercase-name}:{received-value}\n"` for each name, sorted. The values
    /// are whatever arrived; that is the whole point.
    [[nodiscard]] static auto canonical_headers_of(const httplib::Request& req,
                                                   const std::vector<std::string>& names)
        -> std::string {
        std::vector<std::string> sorted = names;
        std::ranges::sort(sorted);
        sorted.erase(std::ranges::unique(sorted).begin(), sorted.end());
        std::string out;
        for (const auto& name : sorted) {
            out += name;
            out.push_back(':');
            out += req.get_header_value(name);
            out.push_back('\n');
        }
        return out;
    }

    // ── ACS3-HMAC-SHA256 (ESS, ECS — the RPC control plane) ──────────────────

    /// Rebuild the V3 canonical request from the received request.
    ///
    /// Method, raw path, query, the named headers' *received* values, the
    /// signed-name list as sent, and the SHA-256 of the body **as it arrived**.
    /// A client that hashes one body and sends another, or signs one host and
    /// sends another, disagrees with this string and is rejected.
    [[nodiscard]] static auto acs3_canonical_request(const httplib::Request& req,
                                                     const std::vector<std::string>& signed_names)
        -> std::string {
        const std::string payload_hash = alibaba_mock_detail::sha256_hex(req.body);
        std::vector<std::string> sorted = signed_names;
        std::ranges::sort(sorted);
        std::string joined;
        for (const auto& name : sorted) {
            if (!joined.empty()) {
                joined.push_back(';');
            }
            joined += name;
        }
        std::string out;
        out += req.method;
        out.push_back('\n');
        out += alibaba_mock_detail::raw_path_of(req);
        out.push_back('\n');
        out += canonical_query_of(req);
        out.push_back('\n');
        out += canonical_headers_of(req, sorted);
        out.push_back('\n');
        out += joined;
        out.push_back('\n');
        out += payload_hash;
        return out;
    }

    /// @return true when the request carries a valid ACS3 signature; otherwise
    ///         fills @p res with the vendor's own error and returns false.
    ///
    /// Caller must **not** hold `_mutex`.
    auto acs3_authorized(const httplib::Request& req, httplib::Response& res) -> bool {
        const auto header = req.get_header_value("Authorization");
        static constexpr std::string_view k_scheme = "ACS3-HMAC-SHA256 ";
        if (!header.starts_with(k_scheme)) {
            note_signature_failure({});
            rpc_error(res, 400, "MissingSecurityToken",
                      "the request carried no ACS3-HMAC-SHA256 Authorization header");
            return false;
        }
        const std::string params = header.substr(k_scheme.size());
        const auto credential = alibaba_mock_detail::auth_param(params, "Credential");
        const auto signed_list = alibaba_mock_detail::auth_param(params, "SignedHeaders");
        const auto provided = alibaba_mock_detail::auth_param(params, "Signature");
        if (credential.empty() || signed_list.empty() || provided.empty()) {
            note_signature_failure({});
            rpc_error(res, 400, "IncompleteSignature",
                      "the Authorization header is missing Credential, SignedHeaders or Signature");
            return false;
        }

        std::string secret;
        {
            const std::lock_guard lock(_mutex);
            if (credential != _access_key_id) {
                ++_signature_failures;
                rpc_error(res, 404, "InvalidAccessKeyId.NotFound",
                          "specified access key is not found: " + credential);
                return false;
            }
            secret = _access_key_secret;
        }

        auto signed_names = alibaba_mock_detail::split(signed_list, ';');

        // The signed set must cover `host` and every `x-acs-*` header the
        // request actually carries. Without this, a client could simply leave a
        // header out of its own SignedHeaders list and the recomputation would
        // agree with it — the ACS3 analogue of the OSS Content-Type hole this
        // whole file exists to close. Alibaba enforces the same rule.
        std::set<std::string> declared(signed_names.begin(), signed_names.end());
        std::vector<std::string> missing;
        if (!declared.contains("host")) {
            missing.emplace_back("host");
        }
        for (const auto& [name, value] : req.headers) {
            const auto lowered = alibaba_mock_detail::to_lower(name);
            if (lowered.starts_with("x-acs-") && !declared.contains(lowered)) {
                missing.push_back(lowered);
            }
        }
        if (!missing.empty()) {
            std::string joined;
            for (const auto& name : missing) {
                joined += joined.empty() ? "" : ", ";
                joined += name;
            }
            note_signature_failure({});
            rpc_error(res, 400, "IncompleteSignature",
                      "SignedHeaders must include host and every x-acs-* header the request "
                      "carries; missing: " +
                          joined);
            return false;
        }

        // The vendor cross-checks the declared payload digest against the body,
        // and so does this: the canonical header line would otherwise agree with
        // whatever the client claimed while the body said something else.
        const auto declared_digest = req.get_header_value("x-acs-content-sha256");
        const auto actual_digest = alibaba_mock_detail::sha256_hex(req.body);
        if (!declared_digest.empty() && declared_digest != actual_digest) {
            note_signature_failure({});
            rpc_error(res, 400, "SignatureDoesNotMatch",
                      "x-acs-content-sha256 is " + declared_digest +
                          " but the received body hashes"
                          " to " +
                          actual_digest);
            return false;
        }

        const auto canonical = acs3_canonical_request(req, signed_names);
        const auto string_to_sign =
            std::string("ACS3-HMAC-SHA256\n") + alibaba_mock_detail::sha256_hex(canonical);
        const auto expected = alibaba_mock_detail::hmac_sha256_hex(secret, string_to_sign);
        if (expected != provided) {
            note_signature_failure(canonical);
            // Echo what the *server* built. When this fires, the difference
            // between it and what the client signed is the defect.
            rpc_error(res, 400, "SignatureDoesNotMatch",
                      "the request signature does not match the one the server computed from the "
                      "received bytes. Server canonical request:\n" +
                          canonical + "\nServer string-to-sign:\n" + string_to_sign);
            return false;
        }
        return true;
    }

    // ── OSS4-HMAC-SHA256 (the object store) ─────────────────────────────────

    /// The header set OSS V4 signs, **derived from what arrived**:
    /// `content-md5`/`content-type` when the request carries them, every
    /// `x-oss-*` header, and anything named in `AdditionalHeaders`.
    ///
    /// Deriving rather than trusting the request's own declaration is the point
    /// of this function. It is what makes the live `SignatureDoesNotMatch`
    /// reproducible here: put `content-type` on the wire, leave it out of the
    /// signature, and the two canonical requests differ by exactly that line.
    [[nodiscard]] static auto oss_signed_header_names(const httplib::Request& req,
                                                      const std::string& additional)
        -> std::vector<std::string> {
        std::vector<std::string> names;
        for (const auto* fixed : {"content-md5", "content-type"}) {
            if (req.has_header(fixed)) {
                names.emplace_back(fixed);
            }
        }
        for (const auto& [name, value] : req.headers) {
            const auto lowered = alibaba_mock_detail::to_lower(name);
            if (lowered.starts_with("x-oss-")) {
                names.push_back(lowered);
            }
        }
        if (!additional.empty()) {
            for (const auto& raw : alibaba_mock_detail::split(additional, ';')) {
                const auto lowered = alibaba_mock_detail::to_lower(raw);
                if (!lowered.empty() && req.has_header(lowered)) {
                    names.push_back(lowered);
                }
            }
        }
        std::ranges::sort(names);
        names.erase(std::ranges::unique(names).begin(), names.end());
        return names;
    }

    /// Rebuild the V4 canonical request from the received request. The
    /// canonical URI is the raw path-style resource off the request line, which
    /// under `endpoint_override` is byte-identical to what the client signs.
    [[nodiscard]] static auto oss_canonical_request(const httplib::Request& req,
                                                    const std::vector<std::string>& signed_names,
                                                    const std::string& additional) -> std::string {
        std::string out;
        out += req.method;
        out.push_back('\n');
        out += alibaba_mock_detail::raw_path_of(req);
        out.push_back('\n');
        out += canonical_query_of(req);
        out.push_back('\n');
        out += canonical_headers_of(req, signed_names);
        out.push_back('\n');
        out += alibaba_mock_detail::to_lower(additional);
        out.push_back('\n');
        // The only hashed-payload value OSS V4 currently supports.
        out += "UNSIGNED-PAYLOAD";
        return out;
    }

    /// Caller must **not** hold `_mutex`.
    auto oss_authorized(const httplib::Request& req, httplib::Response& res) -> bool {
        const auto header = req.get_header_value("Authorization");
        static constexpr std::string_view k_scheme = "OSS4-HMAC-SHA256 ";
        if (!header.starts_with(k_scheme)) {
            note_signature_failure({});
            oss_error(res, 403, "AccessDenied",
                      "the request carried no OSS4-HMAC-SHA256 Authorization header");
            return false;
        }
        const std::string params = header.substr(k_scheme.size());
        const auto credential = alibaba_mock_detail::auth_param(params, "Credential");
        const auto additional = alibaba_mock_detail::auth_param(params, "AdditionalHeaders");
        const auto provided = alibaba_mock_detail::auth_param(params, "Signature");
        if (credential.empty() || provided.empty()) {
            note_signature_failure({});
            oss_error(res, 403, "AccessDenied",
                      "the Authorization header is missing Credential or Signature");
            return false;
        }

        // `Credential=<AccessKeyId>/<date>/<region>/oss/aliyun_v4_request` — the
        // scope is read back off the request, so tampering with it changes the
        // derived signing key and the signature stops matching.
        const auto slash = credential.find('/');
        if (slash == std::string::npos) {
            note_signature_failure({});
            oss_error(res, 403, "AccessDenied", "malformed Credential: " + credential);
            return false;
        }
        const std::string key_id = credential.substr(0, slash);
        const std::string scope = credential.substr(slash + 1);
        const auto scope_parts = alibaba_mock_detail::split(scope, '/');
        if (scope_parts.size() != 4 || scope_parts[2] != "oss" ||
            scope_parts[3] != "aliyun_v4_request") {
            note_signature_failure({});
            oss_error(res, 403, "AccessDenied", "malformed credential scope: " + scope);
            return false;
        }

        std::string secret;
        {
            const std::lock_guard lock(_mutex);
            if (key_id != _access_key_id) {
                ++_signature_failures;
                oss_error(
                    res, 403, "InvalidAccessKeyId",
                    "the OSS Access Key Id you provided does not exist in our records: " + key_id);
                return false;
            }
            secret = _access_key_secret;
        }

        const auto timestamp = req.get_header_value("x-oss-date");
        if (timestamp.empty()) {
            note_signature_failure({});
            oss_error(res, 403, "AccessDenied", "the request carried no x-oss-date header");
            return false;
        }
        // The scope's date element must be the date the request itself carries;
        // otherwise a replay could reuse yesterday's derived key.
        if (timestamp.size() < 8 || timestamp.substr(0, 8) != scope_parts[0]) {
            note_signature_failure({});
            oss_error(res, 403, "AccessDenied",
                      "credential scope date " + scope_parts[0] + " disagrees with x-oss-date " +
                          timestamp);
            return false;
        }

        const auto signed_names = oss_signed_header_names(req, additional);
        const auto canonical = oss_canonical_request(req, signed_names, additional);
        const auto string_to_sign = "OSS4-HMAC-SHA256\n" + timestamp + "\n" + scope + "\n" +
                                    alibaba_mock_detail::sha256_hex(canonical);

        using alibaba_mock_detail::hmac_sha256;
        std::string signing_key = hmac_sha256("aliyun_v4" + secret, scope_parts[0]);
        signing_key = hmac_sha256(signing_key, scope_parts[1]);
        signing_key = hmac_sha256(signing_key, "oss");
        signing_key = hmac_sha256(signing_key, "aliyun_v4_request");
        const auto expected = alibaba_mock_detail::to_hex(hmac_sha256(signing_key, string_to_sign));

        if (expected != provided) {
            note_signature_failure(canonical);
            // Real OSS echoes StringToSign and CanonicalRequest on this error,
            // and that echo is precisely how the live content-type defect was
            // diagnosed. Reproducing it is not decoration.
            oss_error(res, 403, "SignatureDoesNotMatch",
                      "The request signature we calculated does not match the signature you "
                      "provided. Check your key and signing method.",
                      {{"SignatureProvided", provided},
                       {"StringToSign", string_to_sign},
                       {"CanonicalRequest", canonical}});
            return false;
        }
        return true;
    }

    auto note_signature_failure(const std::string& canonical) -> void {
        const std::lock_guard lock(_mutex);
        ++_signature_failures;
        _last_rejected_canonical = canonical;
    }

    // ─────────────────────────────────────────────────────────────────────────
    // ESS / ECS state machine
    // ─────────────────────────────────────────────────────────────────────────

    /// Caller holds `_mutex`.
    auto launch_locked(std::map<std::string, std::string> tags, std::string lifecycle,
                       std::string status, std::string zone) -> std::string {
        const int ordinal = ++_instance_counter;
        instance_state inst;
        inst.id = "i-mock" + std::to_string(ordinal);
        inst.lifecycle_state = std::move(lifecycle);
        inst.status = std::move(status);
        inst.zone_id = std::move(zone);
        inst.private_ip = "192.168.0." + std::to_string(ordinal);
        inst.tags = std::move(tags);
        _instances.push_back(std::move(inst));
        return _instances.back().id;
    }

    /// Caller holds `_mutex`. ESS closes the gap between membership and
    /// DesiredCapacity by launching, which is what terminates
    /// `provision_node`'s poll.
    auto reconcile_capacity_locked() -> void {
        if (!_auto_launch) {
            return;
        }
        while (static_cast<std::int64_t>(_instances.size()) < _desired_capacity) {
            (void)launch_locked(_launch_tags, "InService", _launch_status, _launch_zone);
        }
    }

    [[nodiscard]] static auto to_int(const std::string& raw, std::int64_t fallback)
        -> std::int64_t {
        try {
            return raw.empty() ? fallback : std::stoll(raw);
        } catch (const std::exception&) {
            return fallback;
        }
    }

    /// ECS takes its ID list as a JSON array inside one query parameter — the
    /// single place this RPC-style API departs from flat `Key.N` lists.
    [[nodiscard]] static auto requested_instance_ids(const httplib::Request& req)
        -> std::vector<std::string> {
        std::vector<std::string> ids;
        const auto raw = req.get_param_value("InstanceIds");
        if (raw.empty()) {
            return ids;
        }
        boost::json::value parsed;
        try {
            parsed = boost::json::parse(raw);
        } catch (const std::exception&) {
            return ids;
        }
        const auto* arr = parsed.if_array();
        if (arr == nullptr) {
            return ids;
        }
        for (const auto& entry : *arr) {
            if (entry.is_string()) {
                ids.emplace_back(entry.get_string());
            }
        }
        return ids;
    }

    [[nodiscard]] static auto ecs_view(const instance_state& inst) -> boost::json::object {
        boost::json::object entry;
        entry["InstanceId"] = inst.id;
        entry["Status"] = inst.status;
        entry["ZoneId"] = inst.zone_id;

        boost::json::array ips;
        ips.push_back(boost::json::string(inst.private_ip));
        boost::json::object ip_wrapper;
        ip_wrapper["IpAddress"] = ips;
        boost::json::object vpc;
        vpc["PrivateIpAddress"] = ip_wrapper;
        entry["VpcAttributes"] = vpc;

        boost::json::array tags;
        for (const auto& [key, value] : inst.tags) {
            boost::json::object tag;
            tag["TagKey"] = key;
            tag["TagValue"] = value;
            tags.push_back(tag);
        }
        boost::json::object tag_wrapper;
        tag_wrapper["Tag"] = tags;
        entry["Tags"] = tag_wrapper;
        return entry;
    }

    /// Caller holds `_mutex`.
    auto handle_rpc_locked(const std::string& action, const httplib::Request& req,
                           httplib::Response& res) -> void {
        _requests.push_back("RPC " + action);
        boost::json::object out;
        out["RequestId"] = "MOCK-REQUEST-ID";

        if (action == "DescribeScalingGroups") {
            boost::json::array groups;
            const auto wanted = req.get_param_value("ScalingGroupId.1");
            if (_group_exists && (wanted.empty() || wanted == _group_id)) {
                boost::json::object group;
                group["ScalingGroupId"] = _group_id;
                group["ScalingGroupName"] = "kythira-mock";
                group["LifecycleState"] = "Active";
                group["DesiredCapacity"] = _desired_capacity;
                group["TotalCapacity"] = static_cast<std::int64_t>(_instances.size());
                group["MinSize"] = _min_size;
                group["MaxSize"] = _max_size;
                groups.push_back(group);
            }
            boost::json::object wrapper;
            wrapper["ScalingGroup"] = groups;
            out["TotalCount"] = static_cast<std::int64_t>(groups.size());
            out["ScalingGroups"] = wrapper;
        } else if (action == "DescribeScalingInstances") {
            const auto page = to_int(req.get_param_value("PageNumber"), 1);
            const auto size = to_int(req.get_param_value("PageSize"), 50);
            boost::json::array listed;
            const auto first =
                static_cast<std::size_t>(std::max<std::int64_t>(0, (page - 1) * size));
            for (std::size_t i = first;
                 i < _instances.size() && listed.size() < static_cast<std::size_t>(size); ++i) {
                boost::json::object entry;
                entry["InstanceId"] = _instances[i].id;
                entry["LifecycleState"] = _instances[i].lifecycle_state;
                entry["ScalingGroupId"] = _group_id;
                entry["HealthStatus"] = "Healthy";
                listed.push_back(entry);
            }
            boost::json::object wrapper;
            wrapper["ScalingInstance"] = listed;
            out["TotalCount"] = static_cast<std::int64_t>(_instances.size());
            out["ScalingInstances"] = wrapper;
        } else if (action == "ModifyScalingGroup") {
            if (req.get_param_value("ScalingGroupId") != _group_id) {
                rpc_error(res, 404, "InvalidScalingGroupId.NotFound",
                          "the specified scaling group does not exist");
                return;
            }
            const auto wanted = to_int(req.get_param_value("DesiredCapacity"), -1);
            if (wanted < 0) {
                rpc_error(res, 400, "InvalidParameter", "DesiredCapacity is required");
                return;
            }
            if (wanted > _max_size) {
                rpc_error(res, 400, "IncorrectCapacity.MaxSize",
                          "DesiredCapacity " + std::to_string(wanted) + " exceeds MaxSize " +
                              std::to_string(_max_size));
                return;
            }
            _desired_capacity = wanted;
            reconcile_capacity_locked();
        } else if (action == "RemoveInstances") {
            if (req.get_param_value("ScalingGroupId") != _group_id) {
                rpc_error(res, 404, "InvalidScalingGroupId.NotFound",
                          "the specified scaling group does not exist");
                return;
            }
            std::vector<std::string> targets;
            for (int n = 1; req.has_param("InstanceId." + std::to_string(n)); ++n) {
                targets.push_back(req.get_param_value("InstanceId." + std::to_string(n)));
            }
            for (const auto& target : targets) {
                // MinSize is the group's own policy, and ESS refuses rather
                // than letting a caller shrink below it. Requirement 8.4 says
                // the manager must let that refusal surface instead of
                // second-guessing the group locally, so the mock has to be
                // able to produce it.
                if (req.get_param_value("DecreaseDesiredCapacity") != "false" &&
                    static_cast<std::int64_t>(_instances.size()) <= _min_size) {
                    rpc_error(res, 400, "IncorrectCapacity.MinSize",
                              "removing " + target + " would take scaling group " + _group_id +
                                  " below its MinSize of " + std::to_string(_min_size));
                    return;
                }
                const auto before = _instances.size();
                std::erase_if(_instances,
                              [&](const instance_state& inst) { return inst.id == target; });
                if (_instances.size() == before) {
                    rpc_error(res, 404, "InvalidInstanceId.NotFound",
                              "instance " + target + " is not in scaling group " + _group_id);
                    return;
                }
                // `DecreaseDesiredCapacity` defaults to true; the whole reason
                // the manager prefers RemoveInstances is that the termination
                // and the capacity decrement are one call.
                if (req.get_param_value("DecreaseDesiredCapacity") != "false") {
                    _desired_capacity = std::max<std::int64_t>(_min_size, _desired_capacity - 1);
                }
            }
            boost::json::array jobs;
            jobs.push_back(boost::json::string("job-mock"));
            boost::json::object wrapper;
            wrapper["ScalingActivityId"] = jobs;
            out["ScalingActivityIds"] = wrapper;
        } else if (action == "DescribeInstances") {
            boost::json::array described;
            for (const auto& id : requested_instance_ids(req)) {
                for (const auto& inst : _instances) {
                    if (inst.id == id) {
                        described.push_back(ecs_view(inst));
                    }
                }
            }
            boost::json::object wrapper;
            wrapper["Instance"] = described;
            out["TotalCount"] = static_cast<std::int64_t>(described.size());
            out["Instances"] = wrapper;
        } else if (action == "TagResources") {
            if (req.get_param_value("ResourceType") != "instance") {
                rpc_error(res, 400, "InvalidParameter.ResourceType",
                          "only ResourceType=instance is modelled");
                return;
            }
            const auto target = req.get_param_value("ResourceId.1");
            instance_state* found = nullptr;
            for (auto& inst : _instances) {
                if (inst.id == target) {
                    found = &inst;
                    break;
                }
            }
            if (found == nullptr) {
                rpc_error(res, 404, "InvalidResourceId.NotFound", "no such instance: " + target);
                return;
            }
            // **Additive per key.** ECS creates-or-overwrites the keys it is
            // given and leaves every other tag alone; modelling replacement
            // would invert design Property 4's test value.
            for (int n = 1; req.has_param("Tag." + std::to_string(n) + ".Key"); ++n) {
                found->tags.insert_or_assign(
                    req.get_param_value("Tag." + std::to_string(n) + ".Key"),
                    req.get_param_value("Tag." + std::to_string(n) + ".Value"));
            }
        } else if (action == "DescribeRegions") {
            // The read-only pre-flight the real tier uses; free to model and it
            // keeps this server usable by a smoke test.
            boost::json::array regions;
            boost::json::object region;
            region["RegionId"] = "cn-hangzhou";
            region["LocalName"] = "China (Hangzhou)";
            regions.push_back(region);
            boost::json::object wrapper;
            wrapper["Region"] = regions;
            out["Regions"] = wrapper;
        } else {
            rpc_error(res, 400, "InvalidAction.NotFound",
                      "the mock control plane does not model action: " + action);
            return;
        }

        rpc_reply(res, out);
    }

    // ─────────────────────────────────────────────────────────────────────────
    // OSS object store (path-style only)
    // ─────────────────────────────────────────────────────────────────────────

    /// `ListObjectsV2` with `encoding-type=url` and continuation-token
    /// pagination. Caller holds `_mutex`.
    auto handle_list_locked(const httplib::Request& req, httplib::Response& res) -> void {
        const auto prefix = req.get_param_value("prefix");
        const auto token = req.get_param_value("continuation-token");
        const auto encoding = req.get_param_value("encoding-type");
        _requests.push_back("OSS LIST " + prefix + (token.empty() ? "" : " @" + token));

        if (req.get_param_value("list-type") != "2") {
            oss_error(res, 400, "InvalidArgument", "only list-type=2 is modelled");
            return;
        }

        // `_objects` is a std::map, so iteration is already the lexicographic
        // order OSS lists in — which is the order the persistence engine's
        // 20-digit zero padding was chosen to make numeric.
        std::vector<std::string> matching;
        for (const auto& [key, value] : _objects) {
            if (key.starts_with(prefix)) {
                matching.push_back(key);
            }
        }

        std::size_t start = 0;
        if (!token.empty()) {
            const auto after = decode_continuation_token(token);
            if (!after) {
                oss_error(res, 400, "InvalidArgument",
                          "the continuation token is not one this bucket issued: " + token);
                return;
            }
            while (start < matching.size() && matching[start] <= *after) {
                ++start;
            }
        }

        const std::size_t stop = std::min(start + _oss_page_size, matching.size());
        const bool truncated = stop < matching.size();

        std::string body = R"(<?xml version="1.0" encoding="UTF-8"?><ListBucketResult>)";
        body += "<Name>" + _bucket + "</Name>";
        body += "<Prefix>" +
                (encoding == "url" ? alibaba_mock_detail::percent_encode(prefix)
                                   : alibaba_mock_detail::xml_escape(prefix)) +
                "</Prefix>";
        body += "<MaxKeys>" + std::to_string(_oss_page_size) + "</MaxKeys>";
        body += "<KeyCount>" + std::to_string(stop - start) + "</KeyCount>";
        for (std::size_t i = start; i < stop; ++i) {
            // With `encoding-type=url` the key is percent-encoded, which is what
            // lets a listing carry arbitrary key bytes without XML entities —
            // and is the decoder path `alibaba_oss_client` exercises.
            body += "<Contents><Key>";
            body += encoding == "url" ? alibaba_mock_detail::percent_encode(matching[i])
                                      : alibaba_mock_detail::xml_escape(matching[i]);
            body += "</Key><Size>" + std::to_string(_objects[matching[i]].size()) +
                    "</Size></Contents>";
        }
        body += std::string("<IsTruncated>") + (truncated ? "true" : "false") + "</IsTruncated>";
        if (truncated) {
            body += "<NextContinuationToken>" + encode_continuation_token(matching[stop - 1]) +
                    "</NextContinuationToken>";
        }
        body += "</ListBucketResult>";
        res.status = 200;
        res.set_content(body, "application/xml");
    }

    /// Continuation tokens are hex of the last key returned.
    ///
    /// Opaque (a client must not parse it) but deliberately restricted to
    /// characters that need no percent-encoding. A realistic base64 token would
    /// contain `+` and `/`, and the client re-encodes the token into its next
    /// canonical query — so a token needing encoding would be testing the
    /// vendor's `encoding-type` contract for continuation tokens, which is an
    /// open spike question (Task 0.2), not something a mock gets to decide.
    [[nodiscard]] static auto encode_continuation_token(std::string_view key) -> std::string {
        return alibaba_mock_detail::to_hex(key);
    }

    [[nodiscard]] static auto decode_continuation_token(std::string_view token)
        -> std::optional<std::string> {
        if (token.size() % 2 != 0) {
            return std::nullopt;
        }
        auto nibble = [](char c) -> int {
            if (c >= '0' && c <= '9') {
                return c - '0';
            }
            if (c >= 'a' && c <= 'f') {
                return c - 'a' + 10;
            }
            return -1;
        };
        std::string out;
        out.reserve(token.size() / 2);
        for (std::size_t i = 0; i + 1 < token.size(); i += 2) {
            const int hi = nibble(token[i]);
            const int lo = nibble(token[i + 1]);
            if (hi < 0 || lo < 0) {
                return std::nullopt;
            }
            out.push_back(static_cast<char>((hi << 4) | lo));
        }
        return out;
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Routes
    // ─────────────────────────────────────────────────────────────────────────

    auto install_routes() -> void {
        // ── Control plane: RPC style, one route, `x-acs-action` selects. ──
        _server.Post("/", [this](const httplib::Request& req, httplib::Response& res) {
            if (!acs3_authorized(req, res)) {
                return;
            }
            const auto action = req.get_header_value("x-acs-action");
            const std::lock_guard lock(_mutex);
            handle_rpc_locked(action, req, res);
        });

        // ── OSS data plane: path-style `/{bucket}/{key}` only. ──
        static constexpr const char* k_object_route = R"(/([^/]+)/(.*))";

        _server.Put(k_object_route, [this](const httplib::Request& req, httplib::Response& res) {
            if (!oss_authorized(req, res)) {
                return;
            }
            const auto bucket = req.matches[1].str();
            const auto key = req.matches[2].str();
            if (!bucket_ok(bucket, res)) {
                return;
            }
            if (key.empty()) {
                oss_error(res, 400, "InvalidArgument", "PUT requires an object key");
                return;
            }
            const int forced = _oss_put_status.load();
            int status = forced;
            if (status == 0 && _oss_failing_puts.load() > 0) {
                _oss_failing_puts.fetch_sub(1);
                status = 503;
            }
            const std::lock_guard lock(_mutex);
            _requests.push_back("OSS PUT " + key);
            if (status != 0) {
                oss_error(res, status, "InternalError",
                          "an injected failure; please try again later");
                return;
            }
            _objects[key] = req.body;
            res.status = 200;
            res.set_header("ETag",
                           "\"" + alibaba_mock_detail::sha256_hex(req.body).substr(0, 32) + "\"");
        });

        _server.Get(k_object_route, [this](const httplib::Request& req, httplib::Response& res) {
            if (!oss_authorized(req, res)) {
                return;
            }
            const auto bucket = req.matches[1].str();
            const auto key = req.matches[2].str();
            if (!bucket_ok(bucket, res)) {
                return;
            }
            const std::lock_guard lock(_mutex);
            if (key.empty()) {
                handle_list_locked(req, res);
                return;
            }
            _requests.push_back("OSS GET " + key);
            const auto it = _objects.find(key);
            if (it == _objects.end()) {
                oss_error(res, 404, "NoSuchKey", "the specified key does not exist: " + key);
                return;
            }
            res.status = 200;
            res.set_content(it->second, "application/octet-stream");
        });

        _server.Delete(k_object_route, [this](const httplib::Request& req, httplib::Response& res) {
            if (!oss_authorized(req, res)) {
                return;
            }
            const auto bucket = req.matches[1].str();
            const auto key = req.matches[2].str();
            if (!bucket_ok(bucket, res)) {
                return;
            }
            const std::lock_guard lock(_mutex);
            _requests.push_back("OSS DELETE " + key);
            _objects.erase(key);
            // OSS answers 204 for a present and an absent key alike, which is
            // what makes the engine's truncation idempotent.
            res.status = 204;
        });
    }

    auto bucket_ok(const std::string& bucket, httplib::Response& res) const -> bool {
        if (bucket == _bucket) {
            return true;
        }
        oss_error(res, 404, "NoSuchBucket", "the specified bucket does not exist: " + bucket);
        return false;
    }

    // ─────────────────────────────────────────────────────────────────────────
    // State
    // ─────────────────────────────────────────────────────────────────────────

    httplib::Server _server;
    std::thread _thread;
    int _port{0};

    mutable std::mutex _mutex;

    std::string _access_key_id{"mock-access-key-id"};
    std::string _access_key_secret{"mock-access-key-secret"};
    std::string _security_token;
    std::size_t _signature_failures{0};
    std::string _last_rejected_canonical;

    std::string _group_id;
    std::string _zone_id;
    std::string _bucket;
    bool _group_exists{true};
    std::int64_t _desired_capacity{0};
    std::int64_t _min_size{0};
    std::int64_t _max_size{10};
    bool _auto_launch{true};
    std::string _launch_zone;
    std::string _launch_status{"Running"};
    std::map<std::string, std::string> _launch_tags{{"owner", "platform-team"}};
    std::vector<instance_state> _instances;
    int _instance_counter{0};

    std::map<std::string, std::string> _objects;
    std::size_t _oss_page_size{1000};
    std::atomic<int> _oss_put_status{0};
    std::atomic<int> _oss_failing_puts{0};

    std::vector<std::string> _requests;
};

}  // namespace kythira::testing
