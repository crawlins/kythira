# Protobuf vs. JSON Serializer Performance Comparison

**Last Updated**: July 30, 2026

## Overview

This document records the benchmark that compares the two byte-level
`rpc_serializer` implementations `kythira` ships for its HTTP/CoAP/TCP
transports:

- **JSON backend** — `kythira::json_rpc_serializer<Data>`
  (`include/raft/json_serializer.hpp`), the project's default. Encodes each RPC
  as a `boost::json` object; binary fields (`command`, snapshot `data`) are
  base64-encoded into JSON strings.
- **Protobuf backend** — `kythira::protobuf_rpc_serializer<Data>`
  (`include/raft/protobuf_serializer.hpp`), an optional Protocol Buffers
  implementation (see `.kiro/specs/protobuf-rpc-serializer/`). Encodes each RPC
  with generated message classes from `proto/raft_messages.proto`, prefixed by a
  one-byte message-type tag; binary fields pass through as native protobuf
  `bytes` (no base64).

Both satisfy the same `rpc_serializer` concept and expose the same
`serialize`/`deserialize_*`/`name()` surface, so switching a transport from one
to the other is a one-line `Types::serializer_type` change with no
transport-code change. This benchmark exists to justify that switch with data
(Requirement 8), not to change the default: `json_rpc_serializer` remains the
default `serializer_type`.

## Methodology

The benchmark (`tests/protobuf_json_benchmark_test.cpp`, CTest target
`protobuf_json_benchmark_test`) measures `append_entries_request` — the hottest
and most size-sensitive Raft RPC, since it carries the replicated log entries.
For each `(entry count, per-entry command size)` scenario it reports:

- **Payload size** — bytes produced by `serialize()` for one representative
  request (protobuf includes its one-byte tag).
- **Serialize latency** — mean microseconds per `serialize()` call over 2000
  iterations.
- **Deserialize latency** — mean microseconds per `deserialize_append_entries_request()`
  call over 2000 iterations.

Command bytes are random across the full `0x00`–`0xFF` range, so JSON pays its
base64 expansion (≈ 4/3×) on exactly the data protobuf carries verbatim.

Run it with:

```sh
ctest --test-dir build -R protobuf_json_benchmark_test --output-on-failure
```

## Results

Representative run (release/`-O2`, x86-64, GCC 13, protobuf 3.21). Absolute
latencies are machine-dependent; the **ratios** are the durable takeaway.
`size_ratio` is protobuf ÷ JSON payload size (lower is smaller).

```
entries  cmd_bytes | json_size proto_size size_ratio | json_ser proto_ser | json_des proto_des (us/op)
------------------------------------------------------------------------------------------------------
      0          0 |       131         15      0.115 |    1.084     0.149 |    1.020     0.134
      1         16 |       207         40      0.193 |    2.275     0.526 |    1.949     0.612
      1        256 |       527        282      0.535 |    3.482     0.457 |    2.912     0.940
      8         64 |      1258        599      0.476 |   12.012     1.317 |   10.197     2.625
      8        512 |      6026       4199      0.697 |   29.393     3.674 |   27.411    11.126
     64         64 |      9154       4687      0.512 |   96.918     9.064 |   73.865    25.056
     64        512 |     47298      33487      0.708 |  227.953    23.138 |  187.345    90.438
```

## Interpretation

- **Payload size**: protobuf is consistently smaller — from ~8.7× smaller for a
  tiny heartbeat (no per-field JSON keys, no quoting) to ~1.4× smaller for large
  batches of large commands (where base64's 4/3× expansion dominates the JSON
  cost). For the small/empty `AppendEntries` heartbeats that dominate steady-
  state Raft traffic, the reduction is largest.
- **Serialize latency**: protobuf serializes ~3–10× faster across the matrix —
  no DOM construction, no base64 encode, no string formatting.
- **Deserialize latency**: protobuf parses ~2–8× faster — generated field
  parsers versus `boost::json`'s DOM parse plus base64 decode. The gap narrows
  for large-command batches, where both are dominated by copying the raw bytes.
- The one-byte message-type tag and the `NodeIdValue` `oneof` indirection add a
  small, bounded per-message overhead that is already included in the numbers
  above and is negligible next to the wins.

## Conclusion

For every scenario measured, `protobuf_rpc_serializer` produces smaller payloads
and lower serialize/deserialize latency than `json_rpc_serializer`, with the
largest relative wins on the small messages that dominate steady-state Raft
traffic. The trade-off is an optional build dependency (`protobuf`/`protoc`) and
a non-human-readable wire format. `json_rpc_serializer` stays the default;
`protobuf_rpc_serializer` is available as a drop-in `Types::serializer_type` for
deployments that want the wire-efficiency win without adopting a full gRPC stack.
