// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

package kythira.kcl.sample;

import java.net.URI;
import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.util.UUID;
import software.amazon.awssdk.auth.credentials.AwsCredentialsProvider;
import software.amazon.awssdk.auth.credentials.DefaultCredentialsProvider;
import software.amazon.awssdk.auth.credentials.ProfileCredentialsProvider;
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
import software.amazon.kinesis.leases.LeaseManagementConfig;
import software.amazon.kinesis.lifecycle.events.InitializationInput;
import software.amazon.kinesis.lifecycle.events.LeaseLostInput;
import software.amazon.kinesis.lifecycle.events.ProcessRecordsInput;
import software.amazon.kinesis.lifecycle.events.ShardEndedInput;
import software.amazon.kinesis.lifecycle.events.ShutdownRequestedInput;
import software.amazon.kinesis.metrics.NullMetricsFactory;
import software.amazon.kinesis.processor.ShardRecordProcessor;
import software.amazon.kinesis.processor.ShardRecordProcessorFactory;
import software.amazon.kinesis.processor.SingleStreamTracker;
import software.amazon.kinesis.retrieval.KinesisClientRecord;
import software.amazon.kinesis.retrieval.fanout.FanOutConfig;
import software.amazon.kinesis.retrieval.polling.PollingConfig;

/**
 * A KCL worker on Kythira Kinesis, with its leases on the cluster or in a DynamoDB table.
 *
 * <p>{@code SampleConsumer <endpoint> <stream> <application> [worker-id]}, with credentials from
 * the default chain and the region from {@code AWS_REGION} (us-east-1 by default).
 *
 * <p>{@code KCL_LEASES} picks where the leases live: {@code kythira} (the default) is the Kythira
 * lease API on the same endpoint (task 9, KCL 2.x only); {@code dynamodb} is KCL's own lease
 * table, in the DynamoDB API at {@code KCL_DYNAMODB_ENDPOINT}: Kythira's DynamoDB or DynamoDB
 * Local, or AWS DynamoDB in the region when it is unset (task 16). The same source builds against KCL 2.x ({@code sample}) and KCL 3.x
 * ({@code sample3}).
 *
 * <p>Prints one line per event on stdout, for the scenario to check:
 *
 * <pre>
 * EVENT &lt;millis&gt; &lt;worker&gt; init &lt;shard&gt;
 * EVENT &lt;millis&gt; &lt;worker&gt; record &lt;shard&gt; &lt;sequence&gt; &lt;data&gt;
 * EVENT &lt;millis&gt; &lt;worker&gt; lost &lt;shard&gt;
 * EVENT &lt;millis&gt; &lt;worker&gt; ended &lt;shard&gt;
 * </pre>
 *
 * <p>It checkpoints after every batch, and runs until killed.
 */
public final class SampleConsumer {
    private SampleConsumer() {}

    static synchronized void event(String worker, String what, String shard, String rest) {
        System.out.println("EVENT " + System.currentTimeMillis() + " " + worker + " " + what + " " + shard
                + (rest.isEmpty() ? "" : " " + rest));
        System.out.flush();
    }

    static final class Processor implements ShardRecordProcessor {
        private final String worker;
        private String shard = "?";

        Processor(String worker) {
            this.worker = worker;
        }

        @Override
        public void initialize(InitializationInput input) {
            shard = input.shardId();
            event(worker, "init", shard, "");
        }

        @Override
        public void processRecords(ProcessRecordsInput input) {
            for (KinesisClientRecord r : input.records()) {
                String data = StandardCharsets.UTF_8.decode(r.data()).toString();
                event(worker, "record", shard, r.sequenceNumber() + " " + data);
            }
            try {
                if (!input.records().isEmpty()) {
                    input.checkpointer().checkpoint();
                }
            } catch (Exception e) {
                event(worker, "checkpoint-failed", shard, e.getClass().getSimpleName());
            }
        }

        @Override
        public void leaseLost(LeaseLostInput input) {
            event(worker, "lost", shard, "");
        }

        @Override
        public void shardEnded(ShardEndedInput input) {
            event(worker, "ended", shard, "");
            try {
                input.checkpointer().checkpoint();
            } catch (Exception e) {
                event(worker, "checkpoint-failed", shard, e.getClass().getSimpleName());
            }
        }

        @Override
        public void shutdownRequested(ShutdownRequestedInput input) {
            try {
                input.checkpointer().checkpoint();
            } catch (Exception e) {
                event(worker, "checkpoint-failed", shard, e.getClass().getSimpleName());
            }
        }
    }

    public static void main(String[] args) throws Exception {
        if (args.length < 3) {
            System.err.println("usage: SampleConsumer <endpoint> <stream> <application> [worker-id]");
            System.exit(2);
        }
        URI endpoint = URI.create(args[0]);
        String stream = args[1];
        String application = args[2];
        String worker = args.length > 3 ? args[3] : UUID.randomUUID().toString();
        Region region = Region.of(System.getenv().getOrDefault("AWS_REGION", "us-east-1"));
        long failover = Long.parseLong(System.getenv().getOrDefault("KCL_FAILOVER_MILLIS", "10000"));

        // Polling over HTTP/1.1 by default; KCL_RETRIEVAL=fanout uses enhanced
        // fan-out (task 13), which subscribes over HTTP/2.
        boolean fanOut = "fanout".equals(System.getenv().getOrDefault("KCL_RETRIEVAL", "polling"));
        KinesisAsyncClient kinesis = KinesisAsyncClient.builder()
                .endpointOverride(endpoint)
                .region(region)
                .credentialsProvider(DefaultCredentialsProvider.builder().build())
                .httpClientBuilder(NettyNioAsyncHttpClient.builder()
                        .protocol(fanOut ? Protocol.HTTP2 : Protocol.HTTP1_1)
                        .connectionTimeout(Duration.ofSeconds(5)))
                .build();
        String leaseStore = System.getenv().getOrDefault("KCL_LEASES", "kythira");
        if (!leaseStore.equals("kythira") && !leaseStore.equals("dynamodb")) {
            System.err.println("KCL_LEASES must be kythira or dynamodb, not " + leaseStore);
            System.exit(2);
        }
        // KCL_DYNAMODB_ENDPOINT unset or empty with KCL_LEASES=dynamodb means AWS DynamoDB in
        // the region. With the Kythira lease API, KCL still insists on a DynamoDB client but
        // never calls it.
        String dynamoEndpoint = System.getenv().get("KCL_DYNAMODB_ENDPOINT");
        if (dynamoEndpoint != null && dynamoEndpoint.isBlank()) {
            dynamoEndpoint = null;
        }
        if (leaseStore.equals("kythira")) {
            dynamoEndpoint = "http://127.0.0.1:1";
        }
        // KCL_DYNAMODB_PROFILE names a shared-credentials profile for the lease tables alone.
        // Against AWS DynamoDB the lease calls then sign with AWS credentials, while the
        // Kinesis calls keep the Kythira client key from the environment, which the default
        // chain would otherwise hand to both.
        String dynamoProfile = System.getenv().get("KCL_DYNAMODB_PROFILE");
        AwsCredentialsProvider dynamoCredentials =
                dynamoProfile == null || dynamoProfile.isBlank()
                        ? DefaultCredentialsProvider.builder().build()
                        : ProfileCredentialsProvider.create(dynamoProfile);
        var dynamoBuilder = DynamoDbAsyncClient.builder()
                .region(region)
                .credentialsProvider(dynamoCredentials);
        if (dynamoEndpoint != null) {
            dynamoBuilder.endpointOverride(URI.create(dynamoEndpoint));
        }
        DynamoDbAsyncClient dynamo = dynamoBuilder.build();
        // KCL insists on this one too; NullMetricsFactory means it is never called.
        CloudWatchAsyncClient cloudWatch = CloudWatchAsyncClient.builder()
                .endpointOverride(URI.create("http://127.0.0.1:1"))
                .region(region)
                .credentialsProvider(DefaultCredentialsProvider.builder().build())
                .build();

        ShardRecordProcessorFactory factory = () -> new Processor(worker);
        SingleStreamTracker tracker = new SingleStreamTracker(stream,
                InitialPositionInStreamExtended.newInitialPosition(InitialPositionInStream.TRIM_HORIZON));
        ConfigsBuilder configs = new ConfigsBuilder(tracker, application, kinesis, dynamo, cloudWatch,
                worker, factory);

        LeaseManagementConfig leases = configs.leaseManagementConfig().failoverTimeMillis(failover);
        if (leaseStore.equals("kythira")) {
            KythiraLeases.install(leases, endpoint, application, region);
        }

        Scheduler scheduler = new Scheduler(
                configs.checkpointConfig(),
                configs.coordinatorConfig(),
                leases,
                configs.lifecycleConfig(),
                configs.metricsConfig().metricsFactory(new NullMetricsFactory()),
                configs.processorConfig(),
                configs.retrievalConfig().retrievalSpecificConfig(fanOut
                        ? new FanOutConfig(kinesis).streamName(stream).applicationName(application)
                        : new PollingConfig(stream, kinesis).idleTimeBetweenReadsInMillis(250)));
        Runtime.getRuntime().addShutdownHook(new Thread(() -> {
            try {
                scheduler.startGracefulShutdown().get();
            } catch (Exception e) {
                // Exiting anyway.
            }
        }));
        scheduler.run();
    }
}
