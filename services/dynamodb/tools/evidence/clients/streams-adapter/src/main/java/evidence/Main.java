// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

package evidence;

import com.amazonaws.services.dynamodbv2.streamsadapter.AmazonDynamoDBStreamsAdapterClient;
import com.amazonaws.services.dynamodbv2.streamsadapter.StreamsSchedulerFactory;
import com.amazonaws.services.dynamodbv2.streamsadapter.model.DynamoDBStreamsProcessRecordsInput;
import com.amazonaws.services.dynamodbv2.streamsadapter.polling.DynamoDBStreamsPollingConfig;
import com.amazonaws.services.dynamodbv2.streamsadapter.processor.DynamoDBStreamsShardRecordProcessor;
import java.net.URI;
import java.util.UUID;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import software.amazon.awssdk.auth.credentials.AwsBasicCredentials;
import software.amazon.awssdk.auth.credentials.StaticCredentialsProvider;
import software.amazon.awssdk.regions.Region;
import software.amazon.awssdk.services.cloudwatch.CloudWatchAsyncClient;
import software.amazon.awssdk.services.dynamodb.DynamoDbAsyncClient;
import software.amazon.awssdk.services.dynamodb.streams.DynamoDbStreamsClient;
import software.amazon.kinesis.common.ConfigsBuilder;
import software.amazon.kinesis.common.InitialPositionInStream;
import software.amazon.kinesis.common.InitialPositionInStreamExtended;
import software.amazon.kinesis.coordinator.Scheduler;
import software.amazon.kinesis.lifecycle.events.InitializationInput;
import software.amazon.kinesis.lifecycle.events.LeaseLostInput;
import software.amazon.kinesis.lifecycle.events.ShardEndedInput;
import software.amazon.kinesis.lifecycle.events.ShutdownRequestedInput;
import software.amazon.kinesis.metrics.NullMetricsFactory;
import software.amazon.kinesis.processor.StreamTracker;

/**
 * The Streams Kinesis Adapter evidence flow (task 1): one KCL 3.x worker
 * consumes a DynamoDB stream's shard through the adapter, checkpoints and
 * shuts down. The evidence is both the DynamoDB Streams calls and the lease
 * traffic.
 */
public final class Main {
    static final CountDownLatch PROCESSED = new CountDownLatch(1);

    static final class Processor implements DynamoDBStreamsShardRecordProcessor {
        @Override public void initialize(InitializationInput in) { }

        @Override
        public void processRecords(DynamoDBStreamsProcessRecordsInput in) {
            if (!in.records().isEmpty()) {
                try {
                    in.checkpointer().checkpoint();
                } catch (Exception e) {
                    throw new RuntimeException(e);
                }
                PROCESSED.countDown();
            }
        }

        @Override public void leaseLost(LeaseLostInput in) { }

        @Override
        public void shardEnded(ShardEndedInput in) {
            try {
                in.checkpointer().checkpoint();
            } catch (Exception e) {
                throw new RuntimeException(e);
            }
        }

        @Override
        public void shutdownRequested(ShutdownRequestedInput in) {
            try {
                in.checkpointer().checkpoint();
            } catch (Exception e) {
                throw new RuntimeException(e);
            }
        }
    }

    public static void main(String[] args) throws Exception {
        URI endpoint = URI.create(System.getenv().getOrDefault("ENDPOINT", "http://localhost:8000"));
        StaticCredentialsProvider creds = StaticCredentialsProvider.create(AwsBasicCredentials.create(
                "EVIDENCE-streams-adapter", "evidence-secret-not-a-credential"));
        DynamoDbStreamsClient streams = DynamoDbStreamsClient.builder().endpointOverride(endpoint)
                .region(Region.US_EAST_1).credentialsProvider(creds).build();
        AmazonDynamoDBStreamsAdapterClient adapter =
                new AmazonDynamoDBStreamsAdapterClient(streams, Region.US_EAST_1);
        DynamoDbAsyncClient dynamo = DynamoDbAsyncClient.builder().endpointOverride(endpoint)
                .region(Region.US_EAST_1).credentialsProvider(creds).build();
        CloudWatchAsyncClient cloudWatch = CloudWatchAsyncClient.builder().endpointOverride(endpoint)
                .region(Region.US_EAST_1).credentialsProvider(creds).build();

        String streamArn = "arn:aws:dynamodb:us-east-1:123456789012:table/evidence/stream/"
                + "2026-11-09T00:00:00.000";
        StreamTracker tracker = StreamsSchedulerFactory.createSingleStreamTracker(streamArn,
                InitialPositionInStreamExtended.newInitialPosition(InitialPositionInStream.TRIM_HORIZON));
        ConfigsBuilder cb = new ConfigsBuilder(tracker, "evidence-streams-app", adapter, dynamo,
                cloudWatch, "worker-" + UUID.randomUUID(), Processor::new);
        Scheduler scheduler = StreamsSchedulerFactory.createScheduler(
                cb.checkpointConfig(),
                cb.coordinatorConfig(),
                cb.leaseManagementConfig().failoverTimeMillis(10_000L).shardSyncIntervalMillis(5_000L),
                cb.lifecycleConfig(),
                cb.metricsConfig().metricsFactory(new NullMetricsFactory()),
                cb.processorConfig(),
                cb.retrievalConfig().retrievalSpecificConfig(
                        new DynamoDBStreamsPollingConfig(adapter)),
                adapter);
        Thread t = new Thread(scheduler, "kcl-scheduler");
        t.setDaemon(true);
        t.start();
        boolean got = PROCESSED.await(90, TimeUnit.SECONDS);
        Thread.sleep(12_000L);
        scheduler.startGracefulShutdown().get(60, TimeUnit.SECONDS);
        System.out.println(got ? "streams adapter evidence flow done" : "streams adapter: no records processed");
        System.exit(got ? 0 : 1);
    }
}
