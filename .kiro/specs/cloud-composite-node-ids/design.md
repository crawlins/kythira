# Design Document

## Overview

A node's identity becomes the cloud resource it runs on. Each provider gets
a small value type, a **composite node id**, made of the provider, the
scope in which the cloud's id is unique, and that native id. The cloud
quorum managers use their provider's type as `NodeId`, so mapping a node
to its instance is a field read, not a parse. One helper,
`node_id_traits`, prints and parses every kind of id, and the transports,
serializers, CA and binaries are generalised from `std::uint64_t` to
`NodeId` so a composite id can run end to end. Integer ids stay supported
and get one strict parser and one non-wrapping allocator.

```
              composite_node_id concept  (R1)
       ┌────────────┬────────────┬───────────┬──────────┬────────┐
  aws_ec2_node_id  alibaba_ecs  azure_vm   gcp_instance  oci   docker   (R2)
       │                                                         │
       └──── node_id_traits<NodeId>: to_text / from_text ────────┘ (R3)
                         │
   managers (R4, R5, R11) · discovery (R7) · serializers (R8)
   transports (R9) · CA (R12) · cmd/ (R10) · self_node_id (R6)
```

### Key design decision: a typed composite, not a bare string

`std::string` is already a legal `NodeId`, so the cheapest change would be
to store `"i-0abc..."` as a string. It is rejected as the primary form:

- A string carries no provider or scope. An EC2 id is unique per region; a
  VMSS member name is unique per resource group; a GCP name per project and
  zone. Without the scope, the manager has to assume it from configuration
  and a misconfigured manager can address the wrong resource.
- Validation would happen wherever the string is used, which is how the
  current per-manager parsers drifted apart.
- The typed form costs little: canonical text is a string, so every
  textual path (serializers, tags, SANs, the command line) handles both the
  same way.

`std::string` stays supported and means "canonical text of the manager's
composite type". The host binaries use it (Requirement 10), because one
binary must accept ids from any provider and a compile-time type per
provider would multiply the build.

### Key design decision: the instance id is the node id

The alternative the overflow note suggested was to keep allocating numbers
and resolve them through the `kythira:node-id` tag. That is what numeric
mode does (Requirement 11), but as the default it keeps every problem the
allocators have: max-plus-one races between concurrent provisions, id
reuse after decommission, a scan per lookup, and a tag that is
best-effort on AWS. Using the native id removes allocation entirely. The
only cost is that the id is unknown before launch on clouds that assign it,
which Requirement 6 answers by letting the node read its own id from
instance metadata.

### Key design decision: scope in the id, not in the manager

Putting scope in the id makes a node id meaningful outside the manager that
created it: in logs, in a backup, in another region's manager. It also
leaves room for a cluster that spans regions without changing the type.
Account and subscription are left out of scope: a manager only ever sees
one, and they are sensitive enough that putting them in every log line and
certificate is a cost. If cross-account clusters are ever wanted, the
scope segment can grow (`<account>/<region>`) without changing the
concept.

## Components

### `include/raft/composite_node_id.hpp` (R1, R2, R3)

```cpp
template<typename T>
concept composite_node_id =
    std::regular<T> && std::totally_ordered<T> &&
    requires(const T& t, std::string_view s) {
        { T::provider } -> std::convertible_to<std::string_view>;
        { t.scope() } -> std::same_as<std::string_view>;
        { t.native() } -> std::same_as<std::string_view>;
        { t.to_string() } -> std::same_as<std::string>;
        { T::parse(s) } -> std::same_as<std::optional<T>>;
        { std::hash<T>{}(t) } -> std::convertible_to<std::size_t>;
    };

template<typename T>
concept textual_node_id = std::same_as<T, std::string> || composite_node_id<T>;

template<typename T>
concept node_id = std::unsigned_integral<T> || textual_node_id<T>;
```

`node_id` moves from `types.hpp` to this header, and `types.hpp` includes
it, so every existing `requires node_id<NodeId>` picks up composites
without edits.

Each provider type is a thin instantiation of one template, so the
canonical-text, ordering and hashing logic exists once:

```cpp
template<fixed_string Provider, typename Rules>
class basic_composite_node_id {
public:
    static constexpr std::string_view provider = Provider;
    basic_composite_node_id(std::string scope, std::string native);  // validates, throws
    static auto parse(std::string_view) -> std::optional<basic_composite_node_id>;
    auto scope() const -> std::string_view;
    auto native() const -> std::string_view;
    auto to_string() const -> std::string;              // "<provider>:<scope>:<native>"
    auto operator<=>(const basic_composite_node_id&) const = default;  // on _text
private:
    std::string _text;                                  // canonical text, computed once
    std::uint16_t _scope_end, _native_begin;
};
using aws_ec2_node_id = basic_composite_node_id<"aws-ec2", aws_ec2_rules>;
```

Storing the canonical text and comparing on it gives Requirement 1.5's
single global order for free, and `std::hash<std::string>` over it gives
the hash. `Rules` supplies `validate_scope`, `validate_native` and
`fold_case`:

| Rules | Scope check | Native check | Case |
|---|---|---|---|
| `aws_ec2_rules` | `[a-z]{2}(-[a-z]+)+-\d` (`us-east-1`, `us-gov-west-1`) | `i-` + 8 or 17 `[0-9a-f]` | exact |
| `alibaba_ecs_rules` | `[a-z]{2}-[a-z0-9-]+` | `i-[a-z0-9]{1,64}` | exact |
| `azure_vm_rules` | 1-90 of `[a-z0-9._()-]` | 1-64 of `[a-z0-9._-]` | fold |
| `gcp_instance_rules` | `<project>/<zone>`, project may contain `:` (encoded) | RFC 1035 label ≤ 63 | exact |
| `oci_instance_rules` | equals the region field of the OCID | `ocid1.instance.<realm>.<region>.<unique>` | exact |
| `docker_container_rules` | 1-63 of `[a-z0-9_-]` | `[a-zA-Z0-9][a-zA-Z0-9_.-]*` | exact |

Percent-encoding (R1.6) applies to `:` and `%` in each segment before
joining, and `parse` decodes after splitting on the two unescaped `:`.

`node_id_traits<NodeId>`:

```cpp
template<node_id N> struct node_id_traits {
    static constexpr bool is_textual = textual_node_id<N>;
    static auto to_text(const N&) -> std::string;
    static auto from_text(std::string_view) -> std::optional<N>;
};
template<std::unsigned_integral N>
auto next_numeric_node_id(N max_seen) -> N;   // throws std::overflow_error at max
```

The numeric `from_text` is the strict parser the Alibaba manager already
has (`alibaba_ess_quorum_manager.hpp:1092-1104`), moved here: digits only,
`std::from_chars`, range checked against `N`.

### AWS managers (R4, R11)

```
NodeId = aws_ec2_node_id | std::string      NodeId = unsigned integral
───────────────────────────────────────     ──────────────────────────────
provision:                                  provision:
  RunInstances(tags: managed-by, cluster,     id = next_numeric(max tag over
               market, group)                        cluster, all states)
  id = {region, instance id}                  RunInstances(tags: … + node-id
  CreateTags(node-id = id.text, Name)                      + Name, {NODE_ID})
assess:                                     assess:
  DescribeInstanceStatus(ids → native)        DescribeInstances(filter cluster,
  key by id.text                                 node-id ∈ cluster ids)
decommission:                                 key by tag; fallback R11.4
  Terminate(id.native())                    decommission:
                                              lookup by tag → Terminate
```

The two columns are selected with `if constexpr
(node_id_traits<NodeId>::is_textual)`. The ASG manager already delegates
to EC2 helpers through `ec2_mgr_t`; those helpers become
`to_instance_id(const NodeId&) -> std::optional<std::string>` and
`from_instance(const Instance&) -> std::optional<NodeId>`, with the
numeric variants doing the tag lookup. `std::nullopt` means "not ours",
which `assess_quorum` reports as unreachable rather than throwing
(R4.4, R7.3).

R11.4's fallback is deliberately narrow: only when the tag is missing
*and* the 17 digits fit, which is exactly the set of instances the old
code could have created. An instance with neither is foreign, because the
old code would have thrown on it and could not have launched it.

### Other managers (R5)

| Manager | Id comes from | Lookup | Removed in composite mode |
|---|---|---|---|
| Alibaba ESS | the adopted instance's `InstanceId` + configured region | native `i-` id in ESS/ECS calls | `next_node_id_from`, ceiling check |
| OCI pool | the adopted instance's OCID | OCID in REST path | `next_node_id`, pool-wide tag scan |
| Azure VMSS Flex | member VM name + resource group | VM name | `next_node_id`, `find_instance` scan |
| GCP MIG | managed instance name + project/zone | instance name | `generate_node_id`, `label_id_in_use`, label filter |
| Azure VM | generated name `kythira-<cluster>-<8 base32>` | VM name | `next_node_id` |
| GCP Compute | generated name, same pattern, ≤ 63 | instance name | `generate_node_id` (name collisions retried as today) |
| Docker | generated container name | container name | `next_node_id`, `_max_seen_node_id` |

The group managers' adoption step is unchanged apart from the id: they
already identify the new member (by snapshot difference under the
group-scale-up-rollback spec, or by "no node-id tag" today) and tag it.
In composite mode they derive the id from what they adopted and write
canonical text to the tag.

The numeric paths of these managers stay and are fixed by R7 through the
shared helpers.

### `self_node_id` (R6)

`include/raft/self_node_id.hpp`, one function per provider behind a
`node_id_source` enum, each a single bounded HTTP call to the link-local
metadata endpoint with the provider's required header (AWS IMDSv2 token,
`Metadata: true` for Azure, `Metadata-Flavor: Google`, OCI
`Authorization: Bearer Oracle`). The function returns
`expected<NodeId, std::string>`; binaries print the error and exit.

It is opt-in (R6.3) because a link-local probe off-cloud can hang until the
TCP timeout.

### Raft core, messages and serializers (R8)

- `raft.hpp`: delete `node_id_to_u64`; pass `node_id_type` everywhere it
  was used (`:6796`, `:6828`, `:6956`).
- `types.hpp`: `fetch_log_entries_response<NodeId, …>::_responder_id` is a
  `NodeId`. `group_transport.hpp:325` sets it from the group's local id
  instead of `0`.
- `proto/raft_messages.proto`: `NodeIdValue responder = 6;` beside
  `uint64 responder_id = 1;`. Encoders write both when the id is numeric
  and only `responder` when it is textual.
- Serializers write `node_id_traits::to_text` for textual ids into their
  existing string slot (CBOR/ION already branch; protobuf uses
  `NodeIdValue.text`). JSON's numeric branch switches to
  `value.to_number<NodeId>()`.

### Transports (R9)

The concept change is mechanical: each concept gains `typename NodeId =
std::uint64_t` and uses it for `target` and for the request templates'
`NodeId` argument. Existing `static_assert(network_client<T>)` lines keep
compiling.

Each transport gains a `NodeId` template parameter, defaulting to
`std::uint64_t`, used for the peer map key, the `peer_capability_cache`
key and the `send_*` target. Transports already templated on
`Messages::node_id_type` (`group_transport`, `tcp_gossip_transport`,
`peer2peer_replication`) need only their numeric-only casts replaced by
`node_id_traits`.

`raft.proto` (gRPC) gets `NodeIdValue`-shaped fields at new tags beside
each `uint64` id; a numeric sender fills both, a textual sender only the
new one, and a receiver prefers the new one. That keeps a rolling upgrade
of a numeric cluster wire-compatible in both directions.

### CA (R12)

```
numeric id 7      →  DNS SAN  ca-cluster-node-7                (unchanged)
textual id        →  URI SAN  urn:kythira:node:aws-ec2:us-east-1:i-0abc…
```

The reserved-prefix check (`is_reserved_peer_dns_name`) gains a URI
counterpart, `is_reserved_peer_uri`, and the enrollment endpoint refuses
to issue either form without peer enrollment, exactly as it does for the
DNS prefix today. `tls_tcp_rpc` matches the presented certificate's URI
SAN when `NodeId` is textual and its DNS SAN otherwise.

`certificate_serial` is a value type over `std::array<std::uint8_t, 20>`
plus a length, with `from_hex`, `to_hex` and `from_u64`. Cloud providers
fill it from the issued certificate (parsed from DER when the API does not
return it). The revoke endpoints accept a hex string, and still accept a
JSON number for in-house serials.

### Binaries (R10)

Kconfig:

```
choice KYTHIRA_NODE_ID_KIND
    prompt "Node id type for the host binaries"
    default KYTHIRA_NODE_ID_NUMERIC
config KYTHIRA_NODE_ID_NUMERIC   # std::uint64_t
config KYTHIRA_NODE_ID_TEXT      # std::string, canonical composite text
endchoice
```

`generated/kythira/autoconf.hpp` exposes it, and one alias,
`kythira::host_node_id`, replaces the six hard-coded `std::uint64_t`
aliases in `cmd/`. Every config parser calls
`parse_host_node_id(std::string_view)`, which handles numeric text,
canonical text and `metadata:<provider>`.

## Compatibility

| Change | Who notices | Mitigation |
|---|---|---|
| Manager default `NodeId` becomes the composite type | Code that writes `aws_ec2_quorum_manager<>` | Write `<std::uint64_t>` to keep numeric mode; breaking-change footer |
| AWS string mode stores canonical text, not decimal | Callers instantiating AWS managers with `std::string` (none in tree) | Release note |
| New proto fields | Mixed-version clusters | Old field still written for numeric ids |
| CA URI SAN | Only textual-id clusters | Numeric ids keep DNS SANs |
| `rpc_tls_ready` version byte | Mixed-version CA clusters | Older replicas reject, never misread |

## Testing

- `composite_node_id_test.cpp`: R14.1, with rapidcheck properties for
  round-trip and order consistency.
- `node_id_traits_test.cpp`: R14.2.
- Per-manager composite cases in the existing mock suites; the AWS cases
  use the unit-test EC2 fake with the three ids from R13.1.
- `raft_composite_id_cluster_test.cpp`: three `kythira::node`s with
  `aws_ec2_node_id` over the simulator, including a forced catch-up.
- Per-transport `std::string`-target smoke tests next to each transport's
  existing unit test.
- LocalStack: re-run the two suites from R13.2 against stock 4.x.

## Open questions

1. Should `aws_ec2_node_id`'s scope also carry the partition (`aws`,
   `aws-cn`, `aws-us-gov`)? Region names are already unique across
   partitions, so the design leaves it out.
2. Account and subscription are excluded from scope (see above). Revisit
   if a cross-account cluster is ever required.
