# Enhanced fan-out

Task 13 of `.kiro/specs/kinesis-compatible-streams/` (Requirement 13).

## Consumers

A stream can have up to 20 registered consumers. Each one lives in the
stream's catalog entry, so registering one is a single replicated catalog
change, like tagging.

- **`RegisterStreamConsumer`** needs an ACTIVE stream.
  - The consumer can subscribe straight away, so it is ACTIVE from the
    start. AWS shows CREATING for a few seconds, and clients already wait
    for ACTIVE.
  - Registering a name that is already taken answers
    `ResourceInUseException`. The 21st consumer answers
    `LimitExceededException`; with the `count-limits` switch relaxed the
    cap is 1000.
  - AWS annotates `kinesis:RegisterStreamConsumer` as a control-plane
    write. Under static keys, that puts it outside the `read-write` role.
    A KCL application in fan-out mode registers its own consumer, so with
    static keys it needs an `admin` key. Under IAM, grant the action
    itself.
- **Consumer ARNs.** A consumer ARN is
  `<stream ARN>/consumer/<name>:<creation time in seconds>`. A consumer
  deregistered and then registered again under the same name gets a new
  ARN, and the old one stops working.
- **`DescribeStreamConsumer` and `DeregisterStreamConsumer`** take either
  the `ConsumerARN`, or the `StreamARN` and `ConsumerName`. The front end
  cannot name the resource for the second form, since the ARN carries the
  creation time, so the handler authorizes it once it has found the
  consumer.
- **`ListStreamConsumers`** lists consumers by name, `MaxResults` at a time
  (100 when absent). A `NextToken` from another stream answers
  `ExpiredNextTokenException`.
- **Tags and policies.** `TagResource`, `UntagResource`,
  `ListTagsForResource` and the resource policy calls take a consumer ARN.
  A consumer's tags and policy are its own, apart from the stream's, and
  authorization sees them.
- **`DeleteStream`** refuses a stream with consumers
  (`ResourceInUseException`) unless `EnforceConsumerDeletion` is set.

## `SubscribeToShard`

A subscription pushes one shard's records to one consumer. The answer is an
event stream (`application/vnd.amazon.eventstream`). It opens with an
`initial-response` event, then sends `SubscribeToShardEvent`s. Each event
holds:

- the records read since the last event;
- `MillisBehindLatest`;
- `ContinuationSequenceNumber`, the sequence number of the last record
  sent. A client that resubscribes `AFTER_SEQUENCE_NUMBER` with it misses
  nothing.

How a subscription runs:

- **Where it reads.** The node that takes the request serves it from its
  own replica's committed segments, the way `GetRecords` does. A leader
  change does not touch it. It keeps a copy of the stream's catalog entry
  and refreshes it every second.
- **Pacing.** Each subscription has its own token bucket of 2 MiB/s, AWS's
  per-consumer, per-shard rate. With the `throttling` switch off it is not
  paced. It sends an event as soon as records
  arrive, and an empty one after 5 seconds of nothing.
- **Its lifetime.** A subscription lasts 5 minutes, as on AWS, and then the
  stream ends. Clients resubscribe from their continuation number.
- **One at a time.** A consumer can hold one subscription per shard. A
  second one answers `ResourceInUseException`, until the first ends.
- **The end of a shard.** When a split or merge closes the shard, the last
  event sends a null `ContinuationSequenceNumber` and the `ChildShards`, and
  then the stream ends.
- **Errors.** A subscription ends with an exception event when:
  - the consumer is deregistered, or the stream is deleted
    (`ResourceNotFoundException`);
  - its shard is gone (`ResourceNotFoundException`);
  - a read fails, for example because this node's replica was removed
    (`InternalFailureException`).

  The client then resubscribes, through any node.
- **Encodings.** Events are encoded the way the request was: JSON, or CBOR
  for a CBOR request (the Java SDK's default).

### Settings

`kinesis_service_config` holds the numbers above:

| Field | Default |
|---|---|
| `_subscription_lifetime` | 5 minutes |
| `_subscription_bytes_per_s` | 2 MiB |
| `_subscription_heartbeat` | 5 seconds |
| `_subscription_poll` | 100 ms (how often it looks for new records) |
| `_subscription_recheck` | 1 second (how often it rereads the catalog) |

## Transports

An event stream is a response body that is still being written, so the
transport pulls it. The front end hands back an `event_source` with the
head. The gateway's streaming route (`gateway::stream`) takes
`SubscribeToShard`, and passes every other request on to the ordinary
handler:

- **HTTP/1.1.** The core HTTPS server sends the events as chunks of a
  chunked body, then closes the connection. boto3 subscribes this way.
- **HTTP/2.** The Java SDK (and so KCL 2.x) subscribes over HTTP/2, and it
  multiplexes all of a worker's shards on one connection. The core server
  speaks HTTP/2 through nghttp2 (`include/raft/h2_server_session.hpp`):
  - over TLS when ALPN picks `h2`;
  - in plaintext when a client opens with the HTTP/2 preface (h2c with
    prior knowledge).

  Each subscription is a stream on the connection, with its events as DATA
  frames. A worker thread pulls the events and the connection thread sends
  them. A client that resets the stream cancels its subscription. The daemon
  turns HTTP/2 on whenever the build has nghttp2. Without nghttp2 the
  server speaks HTTP/1.1 only.

`kinesis_fanout_subscriptions` in the Prometheus metrics counts the open
subscriptions per stream. Records sent to subscriptions count in the
shard's outgoing bytes and records, and each event's `MillisBehindLatest`
sets the iterator age, as on AWS.

## Limits and differences

- **Subscriptions are counted per node.** Each node keeps its own registry
  of open subscriptions. A client could hold a second subscription for the
  same consumer and shard through another node. AWS refuses it.
- **Consumers are ACTIVE at once**, as described above. Deregistering takes
  effect at once too: there is no DELETING state.
- **Throughput.** The 2 MiB/s bucket is per subscription. Nothing limits
  the total a node sends.

## Not yet checked against AWS

Each of these is our reading of AWS's documentation. None has been checked
against a recording of AWS:

- the refusal types and their messages;
- the empty event after 5 quiet seconds;
- the exception event types that end a subscription.

## Tests

`kinesis_fanout_test` has 10 cases:

- consumer registration, describe, paging and deregistration, with the
  limits and refusals;
- the `count-limits` switch lifting the cap of 20;
- consumer ARNs taking tags and policies, and `DeleteStream`'s
  `EnforceConsumerDeletion`;
- records and continuation numbers on an open subscription, empty events,
  and one subscription per consumer and shard;
- the refusals;
- the stream ending on a split with child shards, and with an exception
  once the consumer goes;
- the subscription's lifetime;
- the gateway over HTTP/1.1 chunked, and over HTTP/2 with two shards on one
  connection;
- `consumer_command` through the codec.

`h2_server_test` covers the core server's HTTP/2: h2c and h2 over TLS,
multiplexing, streamed bodies next to ordinary requests, client resets,
the body limit and shutdown.
