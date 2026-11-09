// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

package kythira.evidence;

import java.net.URI;
import java.util.List;
import software.amazon.awssdk.core.SdkBytes;
import software.amazon.awssdk.http.urlconnection.UrlConnectionHttpClient;
import software.amazon.awssdk.regions.Region;
import software.amazon.awssdk.services.kinesis.KinesisClient;
import software.amazon.awssdk.services.kinesis.model.PutRecordsRequestEntry;
import software.amazon.awssdk.services.kinesis.model.Record;
import software.amazon.awssdk.services.kinesis.model.ShardIteratorType;

/** The evidence scenario through aws-sdk-java-v2. Usage: JavaSdkV2Client ENDPOINT */
public final class JavaSdkV2Client {
    private JavaSdkV2Client() {}

    public static void main(String[] args) {
        try (KinesisClient k = KinesisClient.builder()
                .endpointOverride(URI.create(args[0]))
                .region(Region.US_EAST_1)
                .httpClient(UrlConnectionHttpClient.create())
                .build()) {
            k.createStream(b -> b.streamName("evidence").shardCount(1));
            k.describeStreamSummary(b -> b.streamName("evidence"));
            k.listShards(b -> b.streamName("evidence"));
            k.putRecord(b -> b.streamName("evidence").partitionKey("pk-1")
                    .data(SdkBytes.fromUtf8String("hello")));
            k.putRecords(b -> b.streamName("evidence").records(
                    PutRecordsRequestEntry.builder().partitionKey("pk-2")
                            .data(SdkBytes.fromUtf8String("one")).build(),
                    PutRecordsRequestEntry.builder().partitionKey("pk-3").explicitHashKey("42")
                            .data(SdkBytes.fromUtf8String("two")).build()));
            String it = k.getShardIterator(b -> b.streamName("evidence")
                    .shardId("shardId-000000000000")
                    .shardIteratorType(ShardIteratorType.TRIM_HORIZON)).shardIterator();
            List<Record> records = k.getRecords(b -> b.shardIterator(it).limit(100)).records();
            if (records.size() != 3 || !records.get(0).data().asUtf8String().equals("hello")) {
                throw new IllegalStateException("unexpected records: " + records);
            }
            k.deleteStream(b -> b.streamName("evidence").enforceConsumerDeletion(true));
        }
    }
}
