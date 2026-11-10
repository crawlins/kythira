// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

package kythira.kcl.sample;

import java.net.URI;
import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.util.UUID;
import kythira.kcl.leases.HttpLeaseApi;
import kythira.kcl.leases.KythiraLeaseManagementFactory;
import kythira.kcl.leases.LeaseApi;
import software.amazon.awssdk.auth.credentials.DefaultCredentialsProvider;
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
import software.amazon.kinesis.retrieval.polling.PollingConfig;

/**
 * A KCL 2.x worker on Kythira: records and leases both on the cluster.
 *
 * <p>{@code SampleConsumer <endpoint> <stream> <application> [worker-id]}, with credentials from
 * the default chain and the region from {@code AWS_REGION} (us-east-1 by default). Prints one
 * line per event on stdout, for the scenario to check:
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

        // HTTP/1.1 polling: no enhanced fan-out in this sample.
        KinesisAsyncClient kinesis = KinesisAsyncClient.builder()
                .endpointOverride(endpoint)
                .region(region)
                .credentialsProvider(DefaultCredentialsProvider.builder().build())
                .httpClientBuilder(NettyNioAsyncHttpClient.builder()
                        .protocol(Protocol.HTTP1_1)
                        .connectionTimeout(Duration.ofSeconds(5)))
                .build();
        // KCL insists on these two; neither is ever called.
        DynamoDbAsyncClient dynamo = DynamoDbAsyncClient.builder()
                .endpointOverride(URI.create("http://127.0.0.1:1"))
                .region(region)
                .credentialsProvider(DefaultCredentialsProvider.builder().build())
                .build();
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
        LeaseApi api = new HttpLeaseApi(endpoint, application,
                DefaultCredentialsProvider.builder().build(), region.id());
        leases.leaseManagementFactory(KythiraLeaseManagementFactory.create(leases, api, false));

        Scheduler scheduler = new Scheduler(
                configs.checkpointConfig(),
                configs.coordinatorConfig(),
                leases,
                configs.lifecycleConfig(),
                configs.metricsConfig().metricsFactory(new NullMetricsFactory()),
                configs.processorConfig(),
                configs.retrievalConfig().retrievalSpecificConfig(
                        new PollingConfig(stream, kinesis).idleTimeBetweenReadsInMillis(250)));
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
