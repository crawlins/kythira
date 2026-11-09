// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

package kythira.evidence;

import com.amazonaws.services.kinesis.producer.KinesisProducer;
import com.amazonaws.services.kinesis.producer.KinesisProducerConfiguration;
import com.amazonaws.services.kinesis.producer.UserRecordResult;
import java.net.URI;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.Future;

/**
 * Ten user records through KPL with aggregation on, flushed once. Usage:
 * KplClient ENDPOINT (https://host:port; the certificate is not verified).
 */
public final class KplClient {
    private KplClient() {}

    public static void main(String[] args) throws Exception {
        URI endpoint = URI.create(args[0]);
        KinesisProducerConfiguration config = new KinesisProducerConfiguration()
                .setRegion("us-east-1")
                .setKinesisEndpoint(endpoint.getHost())
                .setKinesisPort(endpoint.getPort())
                // KPL resolves the stream ARN with STS GetCallerIdentity
                // before its first PutRecords; point that at the stub too, so
                // nothing leaves 127.0.0.1.
                .setStsEndpoint(endpoint.getHost())
                .setStsPort(endpoint.getPort())
                .setVerifyCertificate(false)
                .setMetricsLevel("none")
                .setAggregationEnabled(true)
                .setRecordMaxBufferedTime(100)
                .setLogLevel(System.getenv().getOrDefault("KPL_LOG_LEVEL", "warning"));
        KinesisProducer producer = new KinesisProducer(config);
        try {
            // Let the first ListShards finish: KPL aggregates only records
            // whose shard its shard map can predict, and sends the rest one
            // by one.
            Thread.sleep(2000);
            List<Future<UserRecordResult>> results = new ArrayList<>();
            for (int i = 0; i < 10; ++i) {
                results.add(producer.addUserRecord("evidence", "pk-" + i,
                        ByteBuffer.wrap(("record " + i).getBytes(StandardCharsets.UTF_8))));
            }
            // No explicit flush: a flush sends each buffered record as it
            // stands, while letting RecordMaxBufferedTime expire is what a
            // steady producer does, and is when KPL aggregates.
            for (Future<UserRecordResult> r : results) {
                if (!r.get().isSuccessful()) {
                    throw new IllegalStateException("a record failed: " + r.get().getAttempts());
                }
            }
        } finally {
            producer.destroy();
        }
    }
}
