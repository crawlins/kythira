# Requirements Document

## Introduction

This document specifies requirements for DNS-based `peer_discovery` implementations
for Kythira Raft nodes. The `peer_discovery` concept and the overall node bootstrap
flow are defined in the [node-bootstrap spec](../node-bootstrap/requirements.md).

Five implementations are provided:

- **`rfc1035_peer_discovery`** — queries DNS A and AAAA records to discover cluster
  peers. Provides only `find_peers` (no `register_node`) and is intended as a
  building block for full implementations. Requires libldns ≥ 1.7.
- **`rfc2136_ldns_discovery`** — a complete `peer_discovery` implementation that
  registers the owning node via RFC 2136 DNS Dynamic Update and delegates peer
  queries to `rfc1035_peer_discovery`. Requires libldns ≥ 1.7.
- **`rfc6763_peer_discovery`** — discovers cluster peers via a single SRV query at
  the cluster-level DNS-SD service name. Provides only `find_peers` (no
  `register_node`) and is intended as a building block for full implementations.
  Requires libldns ≥ 1.7.
- **`rfc6763_ldns_peer_discovery`** — a complete `peer_discovery` implementation that
  registers four DNS records per node (PTR, instance SRV, cluster-level SRV,
  domain-level SRV) via RFC 2136 DNS Dynamic Update and delegates peer queries to
  `rfc6763_peer_discovery`. Requires libldns ≥ 1.7.
- **`poco_peer_discovery`** — a complete `peer_discovery` implementation that
  registers and discovers nodes using the platform DNS-SD daemon (Bonjour on macOS,
  Avahi on Linux) via the Poco DNSSD library. Supports both mDNS (zero-config, local
  network) and unicast DNS-SD. Requires Poco ≥ 1.9 with DNSSD support.

## Glossary

- **`rfc1035_peer_discovery`**: A partial `peer_discovery` that queries DNS A/AAAA
  records for a shared cluster name. Does not implement `register_node` and cannot
  satisfy the full `peer_discovery` concept on its own.
- **`rfc2136_ldns_discovery`**: A complete `peer_discovery` that registers the owning
  node's IP in DNS via RFC 2136 Dynamic Update and discovers peers via
  `rfc1035_peer_discovery`.
- **`rfc6763_peer_discovery`**: A partial `peer_discovery` that queries SRV records
  directly at the cluster-level service name to discover cluster nodes. Does not
  implement `register_node` and cannot satisfy the full `peer_discovery` concept on
  its own.
- **`rfc6763_ldns_peer_discovery`**: A complete `peer_discovery` that registers four
  DNS records per node via RFC 2136 Dynamic Update and discovers peers via
  `rfc6763_peer_discovery`.
- **TSIG**: Transaction Signature (RFC 2845) — an HMAC-based authentication mechanism
  for DNS messages.
- **Shared DNS name**: A single DNS name (e.g. `raft.cluster.example.com.`) that
  carries one A or AAAA record per live cluster node, forming a multi-entry RRSET.
- **Cluster-level service name**: A DNS name of the form `_service._proto.cluster.domain.`
  (e.g. `_raft._tcp.cluster.example.com.`) that carries PTR records enumerating
  cluster instances (RFC 6763) and SRV records enabling single-query peer discovery.
- **Domain-level service name**: A DNS name of the form `_service._proto.domain.`
  (e.g. `_raft._tcp.example.com.`) that carries SRV records enabling service
  browsing from the parent domain without knowledge of the specific cluster subdomain.
- **Instance name**: A DNS name of the form `label._service._proto.cluster.domain.`
  (e.g. `node1._raft._tcp.cluster.example.com.`) that carries the instance-level SRV
  record for one cluster node. The label portion (e.g. `node1`) comes from `self_id`
  at registration time.
- **Cluster-level SRV**: An SRV record keyed at the cluster-level service name
  (not the instance name), carrying one entry per live node and enabling
  `find_peers()` to discover all peers with a single DNS query.
- **Domain-level SRV**: An SRV record keyed at the domain-level service name,
  enabling service browsing from the parent domain. Registered for visibility only;
  not used by `find_peers()`.
- **Poco DNSSD**: The `Poco::DNSSD` library component that wraps the platform
  DNS-SD daemon API (`dns_sd.h` on Bonjour; `avahi-compat-libdns_sd` on Linux).
  `Poco::DNSSD::DNSSDResponder` publishes service instances; `Poco::DNSSD::DNSSDBrowser`
  browses and resolves them.

## Requirements

### Requirement 1: `rfc1035_peer_discovery`

**User Story:** As a developer, I want a lightweight DNS lookup class that queries
A and AAAA records to discover cluster peers, so it can be composed into discovery
implementations that handle registration via a separate mechanism.

#### Acceptance Criteria

1. `rfc1035_peer_discovery` SHALL be provided in
   `include/raft/rfc1035_peer_discovery.hpp`. It partially implements the
   `peer_discovery` concept — it provides `find_peers` but NOT `register_node`,
   and therefore does NOT itself satisfy the full concept.
2. WHEN `find_peers()` is called, the instance SHALL use RFC 1035 and the RFCs
   that update it to query for both A and AAAA records for the shared DNS name
   that represents the cluster. Each record in the response represents one cluster
   node; the IP addresses returned SHALL be provided as the full list of `peer_info`
   entries to the caller without any self-filtering.
3. `rfc1035_peer_discovery` SHALL be conditionally compiled only when libldns is
   detected by the build system (`#ifdef KYTHIRA_HAS_LDNS`).
4. The build SHALL succeed with and without libldns present.

### Requirement 2: `rfc2136_ldns_discovery`

**User Story:** As a developer deploying a Kythira cluster in a DNS environment that
supports RFC 2136, I want a `peer_discovery` that automatically registers my node's
address in DNS on startup and deregisters it on shutdown, so that the DNS RRSET for
the cluster name always reflects the set of live nodes.

#### Acceptance Criteria

1. `rfc2136_ldns_discovery` SHALL be provided in
   `include/raft/rfc2136_ldns_discovery.hpp` and SHALL satisfy the
   `peer_discovery<rfc2136_ldns_discovery, std::string, std::string>` concept,
   verified by a `static_assert` in that header.
2. WHEN `register_node(self_id, self_address)` is called it SHALL initiate a
   sequence that registers the provided node with the DNS server it is configured
   to use via the RFC 2136 protocol, by sending a DNS UPDATE adding an A record
   (IPv4 `self_address`) or AAAA record (IPv6 `self_address`) for the configured
   shared DNS name. The returned future SHALL resolve once the server acknowledges
   the update with RCODE NOERROR.
3. WHEN `find_peers()` is called, the instance SHALL delegate to its embedded
   `rfc1035_peer_discovery` instance and then filter out the entry whose address
   matches `self_address` before returning the result to the caller.
4. The destructor SHALL send an RFC 2136 UPDATE deleting the node's own A or AAAA
   record (best-effort; exceptions are swallowed).
5. Optional TSIG authentication (RFC 2845) SHALL be supported via a key name,
   algorithm, and base64-encoded key secret carried in a configuration struct. WHEN
   `tsig_key_name` is empty, no TSIG signing SHALL be performed.
6. The short-TTL design (default 30 s) ensures that records from nodes that crash
   without calling the destructor expire automatically.
7. UPDATE packets SHALL be sent via TCP, per RFC 2136 §6.1.
8. `rfc2136_ldns_discovery` SHALL be conditionally compiled only when libldns is
   detected by the build system (`#ifdef KYTHIRA_HAS_LDNS`).
9. The build SHALL succeed with and without libldns present.

### Requirement 4: `rfc6763_peer_discovery`

**User Story:** As a developer, I want a DNS-SD lookup class that discovers cluster
peers via a direct SRV query at the cluster-level service name, so it can be composed
into discovery implementations that handle registration via a separate mechanism.

#### Acceptance Criteria

1. `rfc6763_peer_discovery` SHALL be provided in
   `include/raft/rfc6763_peer_discovery.hpp`. It partially implements the
   `peer_discovery` concept — it provides `find_peers` but NOT `register_node`,
   and therefore does NOT itself satisfy the full concept.
2. WHEN `find_peers()` is called, the instance SHALL issue a single SRV query
   (`LDNS_RR_TYPE_SRV`) for the configured cluster-level service name. For each SRV
   record in the response it SHALL return a `peer_info` where `node_id` is the SRV
   target hostname and `address` is `"host:port"` (target hostname and port from the
   SRV rdata). Results SHALL NOT be self-filtered.
3. `rfc6763_peer_discovery` SHALL be conditionally compiled only when libldns is
   detected by the build system (`#ifdef KYTHIRA_HAS_LDNS`).
4. The build SHALL succeed with and without libldns present.

### Requirement 5: `rfc6763_ldns_peer_discovery`

**User Story:** As a developer deploying a Kythira cluster in a DNS environment that
supports RFC 2136, I want a DNS-SD `peer_discovery` that automatically publishes this
node's records at the cluster level (for peer discovery) and at the parent domain level
(for service browsing), and removes all of them on shutdown.

#### Acceptance Criteria

1. `rfc6763_ldns_peer_discovery` SHALL be provided in
   `include/raft/rfc6763_ldns_peer_discovery.hpp` and SHALL satisfy the
   `peer_discovery<rfc6763_ldns_peer_discovery, std::string, std::string>` concept,
   verified by a `static_assert` in that header.
2. WHEN `register_node(self_id, self_address)` is called it SHALL send RFC 2136 DNS
   UPDATE requests adding four records, where `<host>` and `<port>` are parsed from
   `self_address` (`"host:port"` format):
   a. **PTR** (cluster zone): `<service_name>. IN PTR <self_id>.<service_name>.` —
      adds this instance to the RFC 6763 DNS-SD enumeration.
   b. **Instance SRV** (cluster zone): `<self_id>.<service_name>. IN SRV <priority>
      <weight> <port> <host>.` — per-instance record for RFC 6763 instance resolution.
   c. **Cluster-level SRV** (cluster zone): `<service_name>. IN SRV <priority>
      <weight> <port> <host>.` — enables `find_peers()` to discover all peers with a
      single SRV query at the cluster-level service name.
   d. **Domain-level SRV** (domain zone): `<domain_service_name>. IN SRV <priority>
      <weight> <port> <host>.` — enables discovery from the parent domain without
      knowledge of the specific cluster subdomain.
   The returned future SHALL resolve once all updates are acknowledged with RCODE
   NOERROR. Records (a), (b), and (c) are sent in a single UPDATE packet to the
   cluster zone; record (d) is sent in a separate UPDATE packet to the domain zone.
3. WHEN `find_peers()` is called, the instance SHALL delegate to its embedded
   `rfc6763_peer_discovery` instance (which queries cluster-level SRV) and then
   filter out the entry whose `address` matches `self_address` before returning
   the result to the caller.
4. The destructor SHALL send RFC 2136 UPDATE DELETE requests removing all four records
   added at registration (best-effort; exceptions are swallowed).
5. Optional TSIG authentication (RFC 2845) SHALL be supported via a key name,
   algorithm, and base64-encoded key secret carried in a configuration struct. WHEN
   `tsig_key_name` is empty, no TSIG signing SHALL be performed.
6. The TTL for all registered records SHALL default to 120 s. Records from nodes that
   crash without calling the destructor expire automatically via TTL.
7. UPDATE packets SHALL be sent via TCP, per RFC 2136 §6.1.
8. `rfc6763_ldns_peer_discovery` SHALL be conditionally compiled only when libldns
   is detected by the build system (`#ifdef KYTHIRA_HAS_LDNS`).
9. The build SHALL succeed with and without libldns present.
10. The configuration SHALL carry a `domain_service_name` field (e.g.
    `"_raft._tcp.example.com."`) and a `domain_zone` field (e.g. `"example.com."`)
    identifying the parent-domain service type and its authoritative zone for record (d).

### Requirement 7: `poco_peer_discovery`

**User Story:** As a developer running a Kythira cluster on a local network or in a
container environment, I want a `peer_discovery` that uses the platform's DNS-SD
daemon for zero-configuration peer registration and discovery, so I do not need to
configure a DNS server or manage DNS UPDATE packets.

#### Acceptance Criteria

1. `poco_peer_discovery` SHALL be provided in `include/raft/poco_peer_discovery.hpp`
   and SHALL satisfy the `peer_discovery<poco_peer_discovery, std::string, std::string>`
   concept, verified by a `static_assert` in that header.
2. WHEN `register_node(self_id, self_address)` is called it SHALL register the node
   as a DNS-SD service instance via `Poco::DNSSD::DNSSDResponder`. The service type
   SHALL come from config, `self_id` SHALL be used as the service instance name, and
   the port SHALL be parsed from `self_address` (`"host:port"` format). The returned
   future SHALL resolve once the platform daemon confirms the registration.
3. WHEN `find_peers(timeout)` is called it SHALL browse for all instances of the
   configured service type using `Poco::DNSSD::DNSSDBrowser`, resolve each discovered
   instance to obtain its hostname and port, wait for at most `timeout`, and return
   `peer_info{instance_name, "host:port"}` for each resolved instance. Entries whose
   `address` matches `self_address` SHALL be excluded.
4. The destructor SHALL unregister the service from the platform DNS-SD daemon
   (best-effort; exceptions are swallowed).
5. `poco_peer_discovery` SHALL be conditionally compiled only when the Poco DNSSD
   library is detected by the build system (`#ifdef KYTHIRA_HAS_POCO_DNSSD`).
6. The build SHALL succeed with and without Poco DNSSD present.
7. The configuration SHALL carry a `service_type` field (e.g. `"_raft._tcp"`) and an
   optional `domain` field (empty string = local mDNS; non-empty = unicast DNS-SD
   domain, e.g. `"cluster.example.com."`).

### Requirement 6: Concept Satisfaction and Documentation

**User Story:** As a developer, I want compile-time verification that DNS discovery
implementations satisfy the required concepts, and clear documentation of the libldns
dependency.

#### Acceptance Criteria

1. `rfc2136_ldns_discovery` SHALL satisfy
   `peer_discovery<rfc2136_ldns_discovery, std::string, std::string>`, verified by a
   `static_assert` at the bottom of `include/raft/rfc2136_ldns_discovery.hpp`.
2. `rfc6763_ldns_peer_discovery` SHALL satisfy
   `peer_discovery<rfc6763_ldns_peer_discovery, std::string, std::string>`, verified
   by a `static_assert` at the bottom of `include/raft/rfc6763_ldns_peer_discovery.hpp`.
3. `poco_peer_discovery` SHALL satisfy
   `peer_discovery<poco_peer_discovery, std::string, std::string>`, verified by a
   `static_assert` at the bottom of `include/raft/poco_peer_discovery.hpp`.
4. `rfc1035_peer_discovery` and `rfc6763_peer_discovery` are explicitly excluded from
   concept-satisfaction verification; no `static_assert` for the full `peer_discovery`
   concept SHALL be added for either.
5. A `DEPENDENCIES.md` entry SHALL document the libldns dependency:
   `libldns ≥ 1.7 — DNS packet construction, RFC 1035/3596 queries, RFC 2136 UPDATE, RFC 2845 TSIG`.
6. A `DEPENDENCIES.md` entry SHALL document the Poco DNSSD dependency:
   `Poco ≥ 1.9 (PocoFoundation + PocoDNSSD) — DNS-SD service registration and browsing via platform daemon`.
