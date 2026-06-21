# Design Document

## Overview

This document describes the design for the five DNS-based `peer_discovery`
implementations for Kythira. Four use libldns (≥ 1.7) for DNS wire-protocol
operations; one uses Poco DNSSD (≥ 1.9) to delegate registration and discovery to
the platform DNS-SD daemon. The `peer_discovery` concept itself and the `peer_info`
type are defined in `include/raft/peer_discovery.hpp` (see the
[node-bootstrap spec](../node-bootstrap/design.md)).

| Class                         | Role    | Record type         | Library / mechanism            |
|-------------------------------|---------|---------------------|--------------------------------|
| `rfc1035_peer_discovery`      | partial | A / AAAA            | libldns, unicast               |
| `rfc2136_ldns_discovery`      | full    | A / AAAA            | libldns, unicast, RFC 2136     |
| `rfc6763_peer_discovery`      | partial | SRV (cluster-level) | libldns, unicast               |
| `rfc6763_ldns_peer_discovery` | full    | PTR + SRV (4 recs)  | libldns, unicast, RFC 2136     |
| `poco_peer_discovery`         | full    | DNS-SD (PTR + SRV)  | Poco DNSSD, mDNS or unicast    |

libldns classes are compiled behind `#ifdef KYTHIRA_HAS_LDNS`; the Poco class is
compiled behind `#ifdef KYTHIRA_HAS_POCO_DNSSD`. Both guards are set by the build
system when the respective library is detected.

## Architecture

```
peer_discovery concept
  │
  ├── rfc1035_peer_discovery          (partial — find_peers only)
  │     └── consumed by rfc2136_ldns_discovery
  │
  ├── rfc2136_ldns_discovery          (full — register_node + find_peers)
  │     embeds rfc1035_peer_discovery
  │     register_node → RFC 2136 UPDATE  (ADD A/AAAA record for self)
  │     find_peers    → rfc1035.find_peers(), filter self by address
  │     ~dtor         → RFC 2136 UPDATE  (DELETE A/AAAA record for self)
  │
  ├── rfc6763_peer_discovery          (partial — find_peers only)
  │     └── consumed by rfc6763_ldns_peer_discovery
  │
  ├── rfc6763_ldns_peer_discovery     (full — register_node + find_peers)
  │     embeds rfc6763_peer_discovery
  │     register_node → RFC 2136 UPDATE  (ADD PTR + instance SRV + cluster SRV + domain SRV)
  │     find_peers    → rfc6763.find_peers(), filter self by address
  │     ~dtor         → RFC 2136 UPDATE  (DELETE all four records)
  │
  └── poco_peer_discovery             (full — register_node + find_peers)
        uses Poco::DNSSD::DNSSDResponder + DNSSDBrowser
        register_node → DNSSDResponder.registerService()   (via platform daemon)
        find_peers    → DNSSDBrowser browse+resolve, wait timeout, filter self by address
        ~dtor         → DNSSDResponder.unregisterService() (best-effort)
```

The four libldns implementations share the same TSIG signing helper pattern and TCP
UPDATE transport. `poco_peer_discovery` delegates entirely to the platform DNS-SD
daemon and makes no direct DNS wire calls. None of the five classes makes network I/O
in its constructor; all I/O is deferred to `register_node()` and `find_peers()`.

## Components and Interfaces

### 1. `include/raft/rfc1035_peer_discovery.hpp`

Provides only the `find_peers` half of the `peer_discovery` concept. Does not
implement `register_node` and therefore does NOT satisfy the full concept. It is
a building block consumed by `rfc2136_ldns_discovery`.

#### `rfc1035_peer_discovery::config`

```cpp
struct config {
    std::string   server;       // DNS server IP or hostname
    std::uint16_t port{53};
    std::string   shared_name;  // e.g. "raft.cluster.example.com."
};
```

#### Class sketch

```cpp
class rfc1035_peer_discovery {
public:
    explicit rfc1035_peer_discovery(config cfg);

    auto find_peers(std::chrono::milliseconds timeout)
        -> kythira::Future<std::vector<peer_info<std::string, std::string>>>;

private:
    // Builds an ldns_resolver* aimed at _cfg.server:_cfg.port.
    // Caller owns the returned pointer (ldns_resolver_deep_free).
    ldns_resolver* make_resolver() const;

    config _cfg;
};
```

#### `find_peers()` sequence

1. Create an `ldns_resolver` via `make_resolver()` pointing at `_cfg.server`.
2. Send two queries for `_cfg.shared_name` — one for `LDNS_RR_TYPE_A` and one
   for `LDNS_RR_TYPE_AAAA` — using `ldns_resolver_send()`. Set the resolver
   read timeout from the `timeout` argument on each query.
3. Merge the answer sections from both responses using
   `ldns_pkt_rr_list_by_type(..., LDNS_SECTION_ANSWER)`.
4. For each A or AAAA RR, stringify the rdata with `ldns_rdf2str()` to obtain
   the IP address string. Construct `peer_info{ip, ip}` (the IP address serves
   as both node identifier and contact address at this layer).
5. Return the collected `peer_info` entries as an immediately-resolved future.

### 2. `include/raft/rfc2136_ldns_discovery.hpp`

Handles node registration (RFC 2136 UPDATE) and delegates `find_peers` to an
embedded `rfc1035_peer_discovery` instance, filtering out the owning node's own
address from the results.

#### `rfc2136_ldns_discovery::config`

```cpp
struct config {
    rfc1035_peer_discovery::config query;  // DNS server, port, and shared_name
    std::string   zone;                    // e.g. "cluster.example.com."
    std::uint32_t ttl{30};                 // A/AAAA record TTL in seconds
    // RFC 2845 TSIG — leave key_name empty to disable authentication
    std::string   tsig_key_name;
    std::string   tsig_algorithm{"hmac-sha256."};
    std::string   tsig_key_base64;
};
```

#### Class sketch

```cpp
class rfc2136_ldns_discovery {
public:
    using node_id_type = std::string;
    using address_type = std::string;

    explicit rfc2136_ldns_discovery(config cfg);
    ~rfc2136_ldns_discovery();  // best-effort deregistration

    auto register_node(std::string self_id, std::string self_address)
        -> kythira::Future<void>;

    auto find_peers(std::chrono::milliseconds timeout)
        -> kythira::Future<std::vector<peer_info<std::string, std::string>>>;

private:
    void deregister_self() noexcept;

    // rr_type is LDNS_RR_TYPE_A or LDNS_RR_TYPE_AAAA; add=true → ADD, false → DELETE.
    ldns_pkt* build_update(ldns_rr_type rr_type, const std::string& ip, bool add) const;

    void maybe_sign(ldns_pkt* pkt) const;

    rfc1035_peer_discovery _rfc1035;
    std::string            _self_id;
    std::string            _self_address;
    config                 _cfg;
};
```

#### `find_peers()` delegation

```cpp
auto rfc2136_ldns_discovery::find_peers(std::chrono::milliseconds timeout)
    -> kythira::Future<std::vector<peer_info<std::string, std::string>>> {
    return _rfc1035.find_peers(timeout).then([this](auto peers) {
        std::erase_if(peers, [&](const auto& p) { return p.address == _self_address; });
        return peers;
    });
}
```

#### `register_node()` / `deregister_self()` sequence

Both call `build_update()` to construct an `ldns_pkt` of type `LDNS_PACKET_UPDATE`.
The record type is `LDNS_RR_TYPE_A` for an IPv4 `self_address` and
`LDNS_RR_TYPE_AAAA` for an IPv6 address (detected via `ldns_rdf_new_frm_str`
succeeding for the appropriate type):
- **register_node**: stores `self_id`/`self_address`; Zone = `_cfg.zone`; Update
  section = one ADD record: `shared_name TTL IN A <ip>` or `... IN AAAA <ip>`.
- **deregister_self**: Zone = `_cfg.zone`; Update section = one DELETE-specific-RR
  for the same A/AAAA content (RFC 2136 §2.5.4).

`maybe_sign()` calls `ldns_pkt_tsig_sign_next()` when `_cfg.tsig_key_name` is
non-empty. UPDATE packets are sent over TCP (`ldns_tcp_send()` per RFC 2136 §6.1).

`static_assert(peer_discovery<rfc2136_ldns_discovery, std::string, std::string>)` at
the bottom of the header.

### 3. `include/raft/rfc6763_peer_discovery.hpp`

Provides only the `find_peers` half of the `peer_discovery` concept using a single
SRV query at the cluster-level service name. Does not implement `register_node` and
therefore does NOT satisfy the full concept. It is a building block consumed by
`rfc6763_ldns_peer_discovery`.

#### `rfc6763_peer_discovery::config`

```cpp
struct config {
    std::string   server;        // DNS server IP or hostname
    std::uint16_t port{53};
    std::string   service_name;  // cluster-level, e.g. "_raft._tcp.cluster.example.com."
};
```

#### Class sketch

```cpp
class rfc6763_peer_discovery {
public:
    explicit rfc6763_peer_discovery(config cfg);

    auto find_peers(std::chrono::milliseconds timeout)
        -> kythira::Future<std::vector<peer_info<std::string, std::string>>>;

private:
    ldns_resolver* make_resolver() const;

    config _cfg;
};
```

#### `find_peers()` sequence

1. Create an `ldns_resolver` via `make_resolver()` pointing at `_cfg.server`.
2. Issue a single SRV query (`LDNS_RR_TYPE_SRV`) for `_cfg.service_name`. The
   cluster-level SRV RRSET holds one record per live node.
3. From each SRV RR in the answer section, extract the target hostname via
   `ldns_rdf2str()` and the port via `ldns_rdf2native_int16()`. Construct
   `"host:port"` as the address string.
4. Construct `peer_info{target_host, "host:port"}` for each SRV RR. Using the
   target hostname as `node_id` gives a stable, human-readable identifier that
   matches what was registered.
5. Return the collected entries as an immediately-resolved future. Results are
   not self-filtered — callers apply their own filter.

### 4. `include/raft/rfc6763_ldns_peer_discovery.hpp`

Handles node registration (RFC 2136 UPDATE adding PTR, instance SRV, cluster-level
SRV, and domain-level SRV records) and delegates `find_peers` to an embedded
`rfc6763_peer_discovery` instance, filtering out the owning node's own address.

#### `rfc6763_ldns_peer_discovery::config`

```cpp
struct config {
    rfc6763_peer_discovery::config query;  // DNS server, port, and cluster-level service_name
    std::string   zone;                    // cluster zone, e.g. "cluster.example.com."
    std::string   domain_service_name;     // domain-level service, e.g. "_raft._tcp.example.com."
    std::string   domain_zone;             // domain zone, e.g. "example.com."
    std::uint16_t srv_priority{10};
    std::uint16_t srv_weight{0};
    std::uint32_t ttl{120};               // TTL for all registered records, in seconds
    // RFC 2845 TSIG — leave key_name empty to disable authentication
    std::string   tsig_key_name;
    std::string   tsig_algorithm{"hmac-sha256."};
    std::string   tsig_key_base64;
};
```

#### Class sketch

```cpp
class rfc6763_ldns_peer_discovery {
public:
    using node_id_type = std::string;
    using address_type = std::string;

    explicit rfc6763_ldns_peer_discovery(config cfg);
    ~rfc6763_ldns_peer_discovery();  // best-effort deregistration

    // self_id: the instance label (e.g. "node1"); used in PTR and instance SRV names.
    // self_address: "host:port" (e.g. "node1.cluster.example.com.:4001").
    auto register_node(std::string self_id, std::string self_address)
        -> kythira::Future<void>;

    auto find_peers(std::chrono::milliseconds timeout)
        -> kythira::Future<std::vector<peer_info<std::string, std::string>>>;

private:
    void deregister_self() noexcept;

    // Builds the cluster-zone UPDATE packet (PTR + instance SRV + cluster SRV).
    // add=true → ADD; add=false → DELETE-specific-RR.
    ldns_pkt* build_cluster_update(bool add) const;

    // Builds the domain-zone UPDATE packet (domain-level SRV only).
    ldns_pkt* build_domain_update(bool add) const;

    void maybe_sign(ldns_pkt* pkt) const;

    rfc6763_peer_discovery _rfc6763;
    std::string            _self_id;       // instance label (e.g. "node1")
    std::string            _instance_name; // self_id + "." + service_name
    std::string            _self_host;     // hostname parsed from self_address
    std::uint16_t          _self_port{0};  // port parsed from self_address
    std::string            _self_address;  // stored for find_peers() filtering
    config                 _cfg;
};
```

#### `find_peers()` delegation

```cpp
auto rfc6763_ldns_peer_discovery::find_peers(std::chrono::milliseconds timeout)
    -> kythira::Future<std::vector<peer_info<std::string, std::string>>> {
    return _rfc6763.find_peers(timeout).then([this](auto peers) {
        std::erase_if(peers, [&](const auto& p) { return p.address == _self_address; });
        return peers;
    });
}
```

Filtering by `address` (i.e. `"host:port"`) matches what `rfc6763_peer_discovery`
returns for each cluster-level SRV record, and corresponds to the value stored in
`_self_address` at registration time.

#### `register_node()` / `deregister_self()` sequence

`register_node(self_id, self_address)`:
1. Parse `self_address` ("host:port") to extract `_self_host` and `_self_port`.
   Throw `std::invalid_argument` if the format is invalid or port is out of
   range [1, 65535].
2. Store `_self_id`, `_self_address`, and
   `_instance_name = self_id + "." + service_name`.
3. Send cluster-zone UPDATE via `build_cluster_update(true)`:
   Zone = `_cfg.zone`; three records in the Update section:
   - PTR ADD: `service_name. TTL IN PTR <instance_name>.`
   - Instance SRV ADD: `<instance_name>. TTL IN SRV <priority> <weight> <port> <host>.`
   - Cluster-level SRV ADD: `service_name. TTL IN SRV <priority> <weight> <port> <host>.`
4. Send domain-zone UPDATE via `build_domain_update(true)`:
   Zone = `_cfg.domain_zone`; one record in the Update section:
   - Domain-level SRV ADD: `<domain_service_name>. TTL IN SRV <priority> <weight> <port> <host>.`
5. Optionally sign each packet with `maybe_sign()` and send both via `ldns_tcp_send()`.
6. Resolve the future on RCODE NOERROR for both updates; reject if either fails.

`deregister_self()` (called from the destructor, noexcept):
1. Call `build_cluster_update(false)` — DELETE-specific-RR for PTR, instance SRV,
   and cluster-level SRV (RFC 2136 §2.5.4).
2. Call `build_domain_update(false)` — DELETE-specific-RR for domain-level SRV.
3. Sign and send both; swallow all exceptions.

`static_assert(peer_discovery<rfc6763_ldns_peer_discovery, std::string, std::string>)`
at the bottom of the header.

### 5. `include/raft/poco_peer_discovery.hpp`

A complete, standalone `peer_discovery` implementation that delegates registration
and discovery to the platform DNS-SD daemon via the Poco DNSSD library. No DNS wire
packets are constructed manually; all protocol work is handled by the daemon
(Bonjour on macOS, Avahi on Linux). Suitable for mDNS (zero-config, local subnet)
or unicast DNS-SD (by specifying a non-local domain in config).

#### `poco_peer_discovery::config`

```cpp
struct config {
    std::string service_type{"_raft._tcp"};  // DNS-SD service type
    std::string domain;                      // "" = local mDNS; "cluster.example.com." = unicast
};
```

#### Class sketch

```cpp
class poco_peer_discovery {
public:
    using node_id_type = std::string;
    using address_type = std::string;

    explicit poco_peer_discovery(config cfg);
    ~poco_peer_discovery();  // best-effort unregistration

    // self_id: DNS-SD instance name (e.g. "node1").
    // self_address: "host:port" — port is extracted and passed to registerService().
    auto register_node(std::string self_id, std::string self_address)
        -> kythira::Future<void>;

    auto find_peers(std::chrono::milliseconds timeout)
        -> kythira::Future<std::vector<peer_info<std::string, std::string>>>;

private:
    Poco::DNSSD::DNSSDResponder              _responder;
    Poco::DNSSD::DNSSDResponder::Handle      _service_handle{};
    std::string                              _self_id;
    std::string                              _self_address;
    config                                   _cfg;
};
```

#### `register_node()` sequence

1. Parse `self_address` ("host:port") to extract the port integer. Throw
   `std::invalid_argument` if the format is invalid or port is out of [1, 65535].
2. Store `_self_id` and `_self_address`.
3. Construct `Poco::DNSSD::ServiceInfo` with `name = self_id`,
   `type = _cfg.service_type`, `domain = _cfg.domain`, `port = port`.
4. Call `_responder.registerService(info, _service_handle)`. Subscribe to the
   `serviceRegistered` event to detect success or failure.
5. Resolve the returned future when `serviceRegistered` fires (success) or on
   registration error; reject on error.

The destructor calls `_responder.unregisterService(_service_handle)` in a
try/catch, swallowing all exceptions.

#### `find_peers()` sequence

1. Create a `Poco::DNSSD::DNSSDBrowser` scoped to this call.
2. Subscribe `serviceFound` — on each event, call `browser.resolve(info)` to
   initiate resolution.
3. Subscribe `serviceResolved` — on each event, construct
   `peer_info{info.name(), info.host() + ":" + std::to_string(info.port())}` and
   append to a local results vector.
4. Call `browser.browse(_cfg.service_type, _cfg.domain)` to start browsing.
5. Wait `timeout` milliseconds (using a `Poco::Timer` or `std::this_thread::sleep_for`).
6. Call `browser.stopBrowse()` to end the browsing session.
7. Filter out any entry whose `address` equals `_self_address`.
8. Return the results as an immediately-resolved future.

#### `peer_info` values

```
{ node_id: "node1", address: "node1.local.:4001" }   // mDNS resolution
{ node_id: "node2", address: "node2.cluster.example.com.:4001" }  // unicast
```

`static_assert(peer_discovery<poco_peer_discovery, std::string, std::string>)` at
the bottom of the header.

## Data Models

### A/AAAA-based discovery (rfc1035 / rfc2136)

The shared DNS name carries one record per live node:

```
raft.cluster.example.com.  30  IN  A     10.0.0.1
raft.cluster.example.com.  30  IN  A     10.0.0.2
raft.cluster.example.com.  30  IN  AAAA  fd00::3
```

`peer_info` entries returned by `rfc1035_peer_discovery::find_peers()`:

```
{ node_id: "10.0.0.1", address: "10.0.0.1" }
{ node_id: "10.0.0.2", address: "10.0.0.2" }
{ node_id: "fd00::3",  address: "fd00::3"  }
```

### SRV-based discovery (rfc6763 / rfc6763_ldns)

`rfc6763_ldns_peer_discovery` registers four DNS records per node:

```
; Cluster zone (cluster.example.com.)
_raft._tcp.cluster.example.com.           120  IN  PTR  node1._raft._tcp.cluster.example.com.   ; PTR (DNS-SD enumeration)
node1._raft._tcp.cluster.example.com.     120  IN  SRV  10 0 4001 node1.cluster.example.com.    ; instance SRV (RFC 6763 resolution)
_raft._tcp.cluster.example.com.           120  IN  SRV  10 0 4001 node1.cluster.example.com.    ; cluster-level SRV (peer discovery)

; Domain zone (example.com.)
_raft._tcp.example.com.                   120  IN  SRV  10 0 4001 node1.cluster.example.com.    ; domain-level SRV (parent-domain browsing)
```

`peer_info` entries returned by `rfc6763_peer_discovery::find_peers()` — which
queries only the cluster-level SRV RRSET at `_raft._tcp.cluster.example.com.`:

```
{ node_id: "node1.cluster.example.com.", address: "node1.cluster.example.com.:4001" }
{ node_id: "node2.cluster.example.com.", address: "node2.cluster.example.com.:4001" }
```

`rfc6763_ldns_peer_discovery::find_peers()` filters out the entry whose `address`
matches `self_address`, leaving only the remote peers.

### RFC 2136 UPDATE wire format (both pairs)

`rfc2136_ldns_discovery` UPDATE (ADD, IPv4):
```
Zone:   cluster.example.com.
Update: raft.cluster.example.com. 30 IN A 10.0.0.3   ; ADD
```

`rfc6763_ldns_peer_discovery` UPDATE — two packets sent in sequence:
```
; Packet 1 — cluster zone
Zone:   cluster.example.com.
Update: _raft._tcp.cluster.example.com.         120 IN PTR  node3._raft._tcp.cluster.example.com.   ; ADD
        node3._raft._tcp.cluster.example.com.   120 IN SRV  10 0 4001 node3.cluster.example.com.    ; ADD
        _raft._tcp.cluster.example.com.          120 IN SRV  10 0 4001 node3.cluster.example.com.    ; ADD

; Packet 2 — domain zone
Zone:   example.com.
Update: _raft._tcp.example.com.                 120 IN SRV  10 0 4001 node3.cluster.example.com.    ; ADD
```

## Correctness Properties

### Property 1: Self-filtering is symmetric with registration
**Validates: Requirements 2.3, 5.3, 7.3**

`rfc2136_ldns_discovery::find_peers()` filters by `_self_address`, which is the same
value inserted as the A/AAAA rdata at registration time. As long as `ldns_rdf2str()`
round-trips the IP without reformatting, a registered node will always exclude itself
from results.

`rfc6763_ldns_peer_discovery::find_peers()` also filters by `_self_address`
(`"host:port"`), which matches the `address` field in the `peer_info` returned by
`rfc6763_peer_discovery` for the cluster-level SRV record registered by this node.
The SRV target hostname and port are the same values parsed from `self_address` at
registration time, so the filter is exact-string and self-consistent.

`poco_peer_discovery::find_peers()` similarly filters by `_self_address`
(`"host:port"`). Because Poco DNSSD resolves the same host and port that were
supplied to `registerService()`, the resolved `address` of the self entry matches
`_self_address` exactly.

### Property 2: Deregistration does not affect other nodes
**Validates: Requirements 2.4, 5.4**

RFC 2136 UPDATE uses DELETE-specific-RR (§2.5.4), which removes only the record
whose rdata exactly matches the registered value. Other nodes' records in the same
RRSET are unaffected.

### Property 3: Crashed-node records expire via TTL
**Validates: Requirements 2.6, 5.6**

A/AAAA records (TTL 30 s) and PTR/SRV records (TTL 120 s) expire automatically if
a node crashes before its destructor runs. Callers tolerate stale entries because
the bootstrap retry loop (`run_bootstrap()`) handles unreachable peers.

### Property 4: Registration is idempotent
**Validates: Requirements 2.2, 5.2**

RFC 2136 UPDATE-ADD with the same rdata is idempotent — the DNS server treats a
duplicate ADD as a no-op if the record already exists with the same TTL. Calling
`register_node()` a second time with identical arguments is therefore safe.

### Property 5: Cluster-level and domain-level records are independent scopes
**Validates: Requirements 5.2, 5.4**

The two UPDATE packets sent by `rfc6763_ldns_peer_discovery` target different DNS
zones (`_cfg.zone` for cluster records and `_cfg.domain_zone` for the domain-level
SRV). A failure of one UPDATE does not affect the other zone's records, and
deregistration similarly sends two independent DELETE packets. Discovery
(`find_peers()`) relies only on the cluster-level SRV, so a domain-zone UPDATE
failure delays parent-domain visibility but never disrupts peer discovery.

## Error Handling

- **DNS server unreachable**: `ldns_resolver_send()` or `ldns_tcp_send()` returns
  a non-NOERROR status or throws → the returned future is rejected with the
  exception. Callers (bootstrap / reconnect loops) treat this as a transient failure
  and retry.
- **RCODE other than NOERROR on UPDATE**: `register_node()` future is rejected.
  The node bootstrap flow propagates the exception and aborts startup
  (per Requirement 10.3 in the node-bootstrap spec).
- **Empty PTR response**: `rfc6763_peer_discovery::find_peers()` returns an empty
  vector. The bootstrap loop treats this as "no peers found" and retries.
- **Destructor errors**: `deregister_self()` is declared `noexcept`. All exceptions
  are caught and swallowed. A log message is emitted if a logger is accessible.
- **Malformed `self_address`**: `register_node()` in `rfc6763_ldns_peer_discovery`
  parses `"host:port"` and throws `std::invalid_argument` if the colon separator is
  absent or the port is not a valid integer in [1, 65535].

## Testing Strategy

DNS discovery classes depend on a live DNS server and cannot be unit-tested in
isolation with the simulator. Testing strategy:

- **Build-guard tests**: CI builds the codebase with and without libldns present
  (for `KYTHIRA_HAS_LDNS`) and with and without Poco DNSSD present (for
  `KYTHIRA_HAS_POCO_DNSSD`), verifying all preprocessor guards compile cleanly.
  These run in the normal ctest suite.
- **`static_assert` verification**: All three full implementations carry
  `static_assert`s that fire at compile time if the class fails to satisfy
  `peer_discovery`. This catches interface regressions without runtime execution.
- **Integration tests — ldns** (future): A separate CI job spins up a Bind9 or
  PowerDNS container with RFC 2136 enabled, runs a small Kythira cluster using
  `rfc2136_ldns_discovery` and `rfc6763_ldns_peer_discovery`, and verifies that
  nodes discover each other and that DNS records are cleaned up after shutdown.
  These are tagged `dns-integration` and are excluded from the default `ctest` run.
- **Integration tests — Poco DNSSD** (future): A separate CI job runs with Avahi
  (or Bonjour) available, starts a small Kythira cluster using `poco_peer_discovery`,
  and verifies registration, discovery, and cleanup. Tagged `dnssd-integration`.

## Dependencies

```
libldns   ≥ 1.7   DNS packet construction, RFC 1035/3596 queries, RFC 6763 PTR/SRV,
                  RFC 2136 UPDATE, RFC 2845 TSIG
Poco      ≥ 1.9   PocoFoundation + PocoDNSSD — DNS-SD service registration and
                  browsing via platform daemon (Bonjour/Avahi)
```
