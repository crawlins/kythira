// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

package kythira.kpl;

import com.amazonaws.services.kinesis.producer.KinesisProducer;
import com.amazonaws.services.kinesis.producer.KinesisProducerConfiguration;
import com.amazonaws.services.kinesis.producer.UserRecordResult;
import java.net.URI;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Set;
import java.util.concurrent.Future;
import java.util.concurrent.TimeUnit;

/**
 * User records through KPL into an existing stream, aggregated.
 *
 * <p>{@code KplProducer <https://host:port> <stream> <count> <tag> [keys]}, with credentials from
 * the default chain. Record {@code i} carries {@code <tag>-<i>} under partition key {@code pk-<i %
 * keys>} (keys defaults to 20). KPL speaks HTTPS only; the certificate is not verified, since the
 * scenario's TLS front is self-signed. STS {@code GetCallerIdentity}, which KPL calls before its
 * first write to build the stream ARN, goes to the same endpoint: Kythira answers it there.
 *
 * <p>A record {@code <tag>-warmup} goes first, alone. Prints one JSON line: how many of the
 * {@code count} user records were acknowledged, and in how many Kinesis records (distinct sequence
 * numbers) they landed. Fewer Kinesis records than user records means
 * KPL aggregated. Exits non-zero if any record failed.
 */
public final class KplProducer {
    private KplProducer() {}

    public static void main(String[] args) throws Exception {
        if (args.length < 4) {
            System.err.println("usage: KplProducer <https://host:port> <stream> <count> <tag> [keys]");
            System.exit(2);
        }
        URI endpoint = URI.create(args[0]);
        String stream = args[1];
        int count = Integer.parseInt(args[2]);
        String tag = args[3];
        int keys = args.length > 4 ? Integer.parseInt(args[4]) : 20;
        KinesisProducerConfiguration config = new KinesisProducerConfiguration()
                .setRegion(System.getenv().getOrDefault("AWS_REGION", "us-east-1"))
                .setKinesisEndpoint(endpoint.getHost())
                .setKinesisPort(endpoint.getPort())
                // Without these KPL asks sts.<region>.amazonaws.com, even
                // with a custom Kinesis endpoint (tests/evidence/README.md).
                .setStsEndpoint(endpoint.getHost())
                .setStsPort(endpoint.getPort())
                .setVerifyCertificate(false)
                .setMetricsLevel("none")
                .setAggregationEnabled(true)
                .setRecordMaxBufferedTime(200)
                .setRequestTimeout(10000)
                .setLogLevel(System.getenv().getOrDefault("KPL_LOG_LEVEL", "warning"));
        KinesisProducer producer = new KinesisProducer(config);
        int failed = 0;
        Set<String> kinesisRecords = new HashSet<>();
        try {
            // KPL aggregates only records whose shard its shard map can
            // predict, and builds a stream's pipeline (STS, then ListShards)
            // on that stream's first record. So one record first, then a
            // pause for the shard map, then the batch.
            UserRecordResult warm = producer.addUserRecord(stream, "pk-warmup",
                    ByteBuffer.wrap((tag + "-warmup").getBytes(StandardCharsets.UTF_8)))
                    .get(120, TimeUnit.SECONDS);
            if (!warm.isSuccessful()) {
                throw new IllegalStateException("the first record failed: " + warm.getAttempts());
            }
            Thread.sleep(2000);
            List<Future<UserRecordResult>> results = new ArrayList<>();
            for (int i = 0; i < count; ++i) {
                byte[] data = (tag + "-" + i).getBytes(StandardCharsets.UTF_8);
                results.add(producer.addUserRecord(stream, "pk-" + (i % keys), ByteBuffer.wrap(data)));
            }
            for (Future<UserRecordResult> f : results) {
                UserRecordResult r = f.get(120, TimeUnit.SECONDS);
                if (r.isSuccessful()) {
                    kinesisRecords.add(r.getShardId() + "/" + r.getSequenceNumber());
                } else {
                    ++failed;
                    if (failed <= 5) {
                        System.err.println("record failed: " + r.getAttempts().get(r.getAttempts().size() - 1)
                                .getErrorCode() + " " + r.getAttempts().get(r.getAttempts().size() - 1)
                                .getErrorMessage());
                    }
                }
            }
        } finally {
            producer.flushSync();
            producer.destroy();
        }
        System.out.println("{\"produced\": " + (count - failed) + ", \"failed\": " + failed
                + ", \"kinesis_records\": " + kinesisRecords.size() + "}");
        System.exit(failed == 0 ? 0 : 1);
    }
}
