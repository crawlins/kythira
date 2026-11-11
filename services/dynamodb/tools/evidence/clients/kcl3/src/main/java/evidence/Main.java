// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

package evidence;

import java.net.URI;
import java.util.UUID;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import software.amazon.awssdk.auth.credentials.AwsBasicCredentials;
import software.amazon.awssdk.auth.credentials.StaticCredentialsProvider;
import software.amazon.awssdk.http.Protocol;
import software.amazon.awssdk.http.nio.netty.NettyNioAsyncHttpClient;
import software.amazon.awssdk.regions.Region;
import software.amazon.awssdk.services.cloudwatch.CloudWatchAsyncClient;
import software.amazon.awssdk.services.dynamodb.DynamoDbAsyncClient;
import software.amazon.awssdk.services.kinesis.KinesisAsyncClient;
import software.amazon.kinesis.common.ConfigsBuilder;
import software.amazon.kinesis.common.InitialPositionInStream;
import software.amazon.kinesis.common.InitialPositionInStreamExtended;
import software.amazon.kinesis.coordinator.Scheduler;
import software.amazon.kinesis.lifecycle.events.InitializationInput;
import software.amazon.kinesis.lifecycle.events.LeaseLostInput;
import software.amazon.kinesis.lifecycle.events.ProcessRecordsInput;
import software.amazon.kinesis.lifecycle.events.ShardEndedInput;
import software.amazon.kinesis.lifecycle.events.ShutdownRequestedInput;
import software.amazon.kinesis.metrics.NullMetricsFactory;
import software.amazon.kinesis.processor.ShardRecordProcessor;
import software.amazon.kinesis.retrieval.polling.PollingConfig;

/**
 * The KCL 3.x evidence flow (task 1): one worker takes the stream's only
 * shard, processes its records, checkpoints, and shuts down gracefully. What
 * it sends to DynamoDB (lease, coordinator-state and worker-metrics tables,
 * lease taking and renewal, leader election, worker metrics,
 * checkpoints) is the evidence; Kinesis calls go to the same stub.
 */
public final class Main {
    static final CountDownLatch PROCESSED = new CountDownLatch(1);

    static final class Processor implements ShardRecordProcessor {
        @Override public void initialize(InitializationInput in) { }

        @Override
        public void processRecords(ProcessRecordsInput in) {
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
                System.getenv().getOrDefault("EVIDENCE_KEY", "EVIDENCE-kcl3"),
                "evidence-secret-not-a-credential"));
        KinesisAsyncClient kinesis = KinesisAsyncClient.builder().endpointOverride(endpoint)
                .region(Region.US_EAST_1).credentialsProvider(creds)
                .httpClientBuilder(NettyNioAsyncHttpClient.builder().protocol(Protocol.HTTP1_1)).build();
        DynamoDbAsyncClient dynamo = DynamoDbAsyncClient.builder().endpointOverride(endpoint)
                .region(Region.US_EAST_1).credentialsProvider(creds).build();
        CloudWatchAsyncClient cloudWatch = CloudWatchAsyncClient.builder().endpointOverride(endpoint)
                .region(Region.US_EAST_1).credentialsProvider(creds).build();

        String stream = "evidence-stream";
        ConfigsBuilder cb = new ConfigsBuilder(stream, "evidence-kcl3-app", kinesis, dynamo, cloudWatch,
                "worker-" + UUID.randomUUID(), Processor::new);
        Scheduler scheduler = new Scheduler(
                cb.checkpointConfig(),
                cb.coordinatorConfig(),
                cb.leaseManagementConfig().failoverTimeMillis(10_000L).shardSyncIntervalMillis(5_000L),
                cb.lifecycleConfig(),
                cb.metricsConfig().metricsFactory(new NullMetricsFactory()),
                cb.processorConfig(),
                cb.retrievalConfig()
                        .initialPositionInStreamExtended(InitialPositionInStreamExtended
                                .newInitialPosition(InitialPositionInStream.TRIM_HORIZON))
                        .retrievalSpecificConfig(new PollingConfig(stream, kinesis)
                                .idleTimeBetweenReadsInMillis(500L)));
        Thread t = new Thread(scheduler, "kcl-scheduler");
        t.setDaemon(true);
        t.start();
        // KCL 3.x needs about a minute before its first record (two 10 s
        // waits for the lease table's GSI, then a 20 s assignment round);
        // a slower runtime, such as rootless Podman, adds to that.
        boolean got = PROCESSED.await(180, TimeUnit.SECONDS);
        // One more renewal round before shutting down, so it is recorded.
        Thread.sleep(12_000L);
        scheduler.startGracefulShutdown().get(60, TimeUnit.SECONDS);
        System.out.println(got ? "kcl3 evidence flow done" : "kcl3: no records processed");
        System.exit(got ? 0 : 1);
    }
}
