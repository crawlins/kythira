# Implementation Plan — DNS Peer Discovery

## Status: In Progress (tasks 3 and 4 remain; task 6 added and complete)

**Last Updated**: June 22, 2026

## Overview

Implement five DNS-based `peer_discovery` implementations: `rfc1035_peer_discovery`
(read-only A/AAAA queries via libldns), `rfc2136_ldns_discovery` (RFC 2136 dynamic
registration via A/AAAA, libldns), `rfc6763_peer_discovery` (read-only DNS-SD SRV
query via libldns), `rfc6763_ldns_peer_discovery` (RFC 2136 dynamic registration via
PTR/SRV, libldns), and `poco_peer_discovery` (full registration and discovery via
Poco DNSSD and the platform daemon).

The `peer_discovery` concept and `peer_info` type are defined in
`include/raft/peer_discovery.hpp` (see the [node-bootstrap spec](../node-bootstrap/tasks.md)).

## Task Dependency Graph

```json
{
  "waves": [
    {
      "wave": 1,
      "tasks": [1, 3, 5],
      "description": "Query-only base classes (1, 3) and standalone Poco implementation (5) — all independent"
    },
    {
      "wave": 2,
      "tasks": [2, 4],
      "description": "Full ldns implementations: rfc2136 depends on task 1; rfc6763_ldns depends on task 3"
    }
  ]
}
```

## Tasks

- [x] 1. Implement `rfc1035_peer_discovery` in `include/raft/rfc1035_peer_discovery.hpp`
  - Add `libldns` (≥ 1.7) detection to `CMakeLists.txt` if not already present;
    guard with `#ifdef KYTHIRA_HAS_LDNS`
  - Define `rfc1035_peer_discovery::config` struct with fields: `server`, `port`
    (53), `shared_name`
  - Ctor `rfc1035_peer_discovery(config)`: stores config only; no network I/O
  - `find_peers(timeout)`: creates `ldns_resolver` pointing at `config.server`;
    issues separate A (`LDNS_RR_TYPE_A`) and AAAA (`LDNS_RR_TYPE_AAAA`) queries
    for `shared_name` per RFC 1035 and its updates (including RFC 3596); merges
    answer sections; converts each IP rdata string to `peer_info{ip, ip}`; returns
    all results without self-filtering as an immediately-resolved future
  - Does NOT implement `register_node`; does NOT satisfy the `peer_discovery`
    concept; no `static_assert` for concept satisfaction
  - Verify: `cmake --build build` succeeds with and without libldns present
  - _Requirements: 1.1–1.4_

- [x] 2. Implement `rfc2136_ldns_discovery` in `include/raft/rfc2136_ldns_discovery.hpp`
  - Depends on task 1 (`rfc1035_peer_discovery`)
  - Define `rfc2136_ldns_discovery::config` struct embedding
    `rfc1035_peer_discovery::config query` plus fields: `zone`, `ttl` (30),
    `tsig_key_name`, `tsig_algorithm` (`"hmac-sha256."`), `tsig_key_base64`
  - Ctor `rfc2136_ldns_discovery(config)`: constructs embedded
    `rfc1035_peer_discovery _rfc1035{cfg.query}`; no network I/O
  - `register_node(self_id, self_address) -> Future<void>`: stores self identity,
    detects IPv4 vs IPv6 from `self_address`, sends RFC 2136 UPDATE adding an A
    (IPv4) or AAAA (IPv6) record for `self_address` at `shared_name` with configured
    TTL; future resolves on RCODE NOERROR, rejects otherwise
  - Dtor: calls `deregister_self()` (swallows all exceptions) to send RFC 2136
    UPDATE deleting the node's own A or AAAA record
  - `find_peers(timeout)`: delegates to `_rfc1035.find_peers(timeout)`, then
    filters out the entry whose address matches `_self_address`
  - `maybe_sign(ldns_pkt*)`: if `tsig_key_name` non-empty, calls
    `ldns_pkt_tsig_sign_next()` with configured algorithm and key
  - UPDATE packets sent via `ldns_tcp_send()` (TCP preferred per RFC 2136 §6.1)
  - Add `static_assert(peer_discovery<rfc2136_ldns_discovery, std::string, std::string>)`
  - Add `DEPENDENCIES.md` entry: `libldns ≥ 1.7`
  - Verify: `cmake --build build` succeeds with and without libldns present
  - _Requirements: 2.1–2.9, 6.1, 6.3, 6.4_

- [ ] 3. Implement `rfc6763_peer_discovery` in `include/raft/rfc6763_peer_discovery.hpp`
  - Guard with `#ifdef KYTHIRA_HAS_LDNS` (libldns already detected by task 1)
  - Define `rfc6763_peer_discovery::config` struct with fields: `server`, `port`
    (53), `service_name` (cluster-level, e.g. `"_raft._tcp.cluster.example.com."`)
  - Ctor `rfc6763_peer_discovery(config)`: stores config only; no network I/O
  - `find_peers(timeout)`:
    - Creates `ldns_resolver` pointing at `config.server` with timeout set
    - Issues a single SRV query (`LDNS_RR_TYPE_SRV`) for `service_name`; the
      cluster-level SRV RRSET holds one entry per live node
    - Extracts target hostname via `ldns_rdf2str()` and port via
      `ldns_rdf2native_int16()` from each SRV RR; constructs `"host:port"` string
    - Returns `peer_info{target_host, "host:port"}` for each SRV RR as an
      immediately-resolved future; results are not self-filtered
  - Does NOT implement `register_node`; does NOT satisfy the `peer_discovery`
    concept; no `static_assert` for concept satisfaction
  - Verify: `cmake --build build` succeeds with and without libldns present
  - _Requirements: 4.1–4.4_

- [ ] 4. Implement `rfc6763_ldns_peer_discovery` in
  `include/raft/rfc6763_ldns_peer_discovery.hpp`
  - Depends on task 3 (`rfc6763_peer_discovery`)
  - Guard with `#ifdef KYTHIRA_HAS_LDNS`
  - Define `rfc6763_ldns_peer_discovery::config` struct embedding
    `rfc6763_peer_discovery::config query` plus fields: `zone` (cluster zone),
    `domain_service_name` (e.g. `"_raft._tcp.example.com."`), `domain_zone`
    (e.g. `"example.com."`), `srv_priority` (10), `srv_weight` (0), `ttl` (120),
    `tsig_key_name`, `tsig_algorithm` (`"hmac-sha256."`), `tsig_key_base64`
  - Ctor `rfc6763_ldns_peer_discovery(config)`: constructs embedded
    `rfc6763_peer_discovery _rfc6763{cfg.query}`; no network I/O
  - `register_node(self_id, self_address) -> Future<void>`:
    - Parses `self_address` as `"host:port"`; throws `std::invalid_argument` if
      malformed or port out of range [1, 65535]
    - Stores `_self_id`, `_self_address`, `_self_host`, `_self_port`, and
      `_instance_name = self_id + "." + service_name`
    - Sends cluster-zone UPDATE (`build_cluster_update(true)`, Zone = `_cfg.zone`)
      adding three records:
      - PTR: `service_name. TTL IN PTR <instance_name>.`
      - Instance SRV: `<instance_name>. TTL IN SRV <priority> <weight> <port> <host>.`
      - Cluster-level SRV: `service_name. TTL IN SRV <priority> <weight> <port> <host>.`
    - Sends domain-zone UPDATE (`build_domain_update(true)`, Zone = `_cfg.domain_zone`)
      adding one record:
      - Domain-level SRV: `<domain_service_name>. TTL IN SRV <priority> <weight> <port> <host>.`
    - Future resolves once both updates return RCODE NOERROR; rejects if either fails
  - Dtor: calls `deregister_self()` (swallows all exceptions); sends DELETE
    packets for all four records via `build_cluster_update(false)` and
    `build_domain_update(false)` (RFC 2136 §2.5.4)
  - `find_peers(timeout)`: delegates to `_rfc6763.find_peers(timeout)` (single
    cluster-level SRV query), then filters out the entry whose `address` matches
    `_self_address`
  - `maybe_sign(ldns_pkt*)`: same TSIG signing pattern as `rfc2136_ldns_discovery`
  - All UPDATE packets sent via `ldns_tcp_send()` (TCP preferred per RFC 2136 §6.1)
  - Add `static_assert(peer_discovery<rfc6763_ldns_peer_discovery, std::string, std::string>)`
  - Verify: `cmake --build build` succeeds with and without libldns present
  - _Requirements: 5.1–5.10, 6.2, 6.3_

- [x] 5. Implement `poco_peer_discovery` in `include/raft/poco_peer_discovery.hpp`
  - Add Poco DNSSD (≥ 1.9) detection to `CMakeLists.txt`; guard with
    `#ifdef KYTHIRA_HAS_POCO_DNSSD` (independent of `KYTHIRA_HAS_LDNS`)
  - Define `poco_peer_discovery::config` struct with fields: `service_type`
    (default `"_raft._tcp"`) and `domain` (default `""` = local mDNS)
  - Ctor `poco_peer_discovery(config)`: stores config and constructs
    `Poco::DNSSD::DNSSDResponder`; no network I/O
  - `register_node(self_id, self_address) -> Future<void>`:
    - Parses `self_address` as `"host:port"`; throws `std::invalid_argument` if
      malformed or port out of range [1, 65535]
    - Stores `_self_id` and `_self_address`
    - Constructs `Poco::DNSSD::ServiceInfo` with `name = self_id`,
      `type = _cfg.service_type`, `domain = _cfg.domain`, `port = port`
    - Calls `_responder.registerService(info, _service_handle)` and subscribes to
      the `serviceRegistered` event; resolves the future on success or rejects on
      registration error
  - `find_peers(timeout)`:
    - Creates a `Poco::DNSSD::DNSSDBrowser` scoped to this call
    - Subscribes `serviceFound` → calls `browser.resolve(info)` for each found
      instance
    - Subscribes `serviceResolved` → appends
      `peer_info{info.name(), info.host() + ":" + std::to_string(info.port())}` to
      results vector
    - Calls `browser.browse(_cfg.service_type, _cfg.domain)`, waits `timeout`
      milliseconds, then calls `browser.stopBrowse()`
    - Filters out entry whose `address` matches `_self_address`
    - Returns results as an immediately-resolved future
  - Dtor: calls `_responder.unregisterService(_service_handle)` in a try/catch;
    swallows all exceptions
  - Add `static_assert(peer_discovery<poco_peer_discovery, std::string, std::string>)`
  - Add `DEPENDENCIES.md` entry:
    `Poco ≥ 1.9 (PocoFoundation + PocoDNSSD) — DNS-SD service registration and browsing via platform daemon`
  - Verify: `cmake --build build` succeeds with and without Poco DNSSD present
  - _Requirements: 7.1–7.7, 6.3, 6.6_

- [x] 6. Implement `rfc2136_dns_sd_discovery` in `include/raft/rfc2136_dns_sd_discovery.hpp`
  - DNS-SD peer discovery over unicast DNS using RFC 2136 dynamic update; a
    different design from `rfc6763_ldns_peer_discovery` (task 4) — registers
    PTR + SRV + TXT records with a `fresh_until=<epoch>` TXT field instead of
    relying solely on DNS TTL for staleness detection.
  - Define `rfc2136_dns_sd_discovery::config` with: `server`, `port` (53),
    `zone`, `service_domain`, `service_type` (`"_kythira._tcp"`), `ttl` (30),
    `freshness_interval` (60 s), and optional TSIG fields.
  - `register_node(self_id, self_address) -> Future<void>`: sends RFC 2136 UPDATE
    adding PTR (`<service_type>.<service_domain>. IN PTR <self_id>...`), SRV, and
    TXT (`fresh_until=<epoch>`) records; starts a background fresher thread that
    renews the TXT record every `freshness_interval / 2`.
  - `find_peers(timeout)`: queries PTR records to enumerate instances; for each
    instance queries TXT to obtain `fresh_until`; filters out instances whose
    `fresh_until` has passed; queries SRV records for host:port; returns
    `peer_info{node_id, "host:port"}` excluding self.
  - Dtor: stops the fresher thread; sends RFC 2136 DELETE UPDATE best-effort.
  - Fault injection: `raft/dns/rfc2136/dns_sd/update` (throws),
    `raft/dns/rfc2136/dns_sd/update/noop` (silent pass-through).
  - `static_assert(peer_discovery<rfc2136_dns_sd_discovery, std::string, std::string>)`
  - Tests: 6 unit cases (`rfc2136_dns_sd_suite` in `dns_peer_discovery_unit_test.cpp`)
    and 4 chaos cases (`rfc2136_dns_sd_chaos_suite` in `dns_peer_discovery_chaos_test.cpp`).
  - Docker scenario test (`docker-dns-sd-discovery-tests`): 3 cases covering
    healthy nodes, peer discovery, and dead-node absence after freshness expiry
    (BIND9 + 3 `dns_sd_discovery_node` containers, 20 s freshness interval).
  - _Outside original requirements scope; addresses freshness-based staleness
    detection not covered by TTL-only approach in tasks 3–4_

## Notes

- Tasks 1, 3, and 5 are independent and can be implemented in parallel; tasks 2 and 4
  depend only on their respective base class (tasks 1 and 3, respectively). Task 5
  has no dependencies on the libldns tasks and can proceed at any time.
- The `maybe_sign()` helper and TCP send pattern are identical across `rfc2136_ldns_discovery`
  and `rfc6763_ldns_peer_discovery`. Consider a shared internal helper in a
  `detail/` header (not a public API) to avoid duplication, but only if both are
  implemented at the same time.
- `rfc6763_ldns_peer_discovery` sends two UPDATE packets per registration: one to the
  cluster zone (PTR + instance SRV + cluster-level SRV) and one to the domain zone
  (domain-level SRV). The cluster-zone packet is sent first; a failure there rejects
  the future before the domain-zone packet is attempted.
- The `rfc2136_ldns_discovery` requirement references in task 2 changed from
  `3.1–3.3` to `6.1, 6.3, 6.4` when Requirement 3 was renumbered to Requirement 6
  after adding Requirements 4 and 5.
