// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

package evidence;

import com.amazonaws.services.dynamodbv2.streamsadapter.AmazonDynamoDBStreamsAdapterClient;
import com.amazonaws.services.dynamodbv2.streamsadapter.StreamsSchedulerFactory;
import com.amazonaws.services.dynamodbv2.streamsadapter.model.DynamoDBStreamsProcessRecordsInput;
import com.amazonaws.services.dynamodbv2.streamsadapter.polling.DynamoDBStreamsPollingConfig;
import com.amazonaws.services.dynamodbv2.streamsadapter.processor.DynamoDBStreamsShardRecordProcessor;
import java.net.URI;
import java.util.Map;
import java.util.UUID;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import software.amazon.awssdk.auth.credentials.AwsBasicCredentials;
import software.amazon.awssdk.auth.credentials.StaticCredentialsProvider;
import software.amazon.awssdk.regions.Region;
import software.amazon.awssdk.services.cloudwatch.CloudWatchAsyncClient;
import software.amazon.awssdk.services.dynamodb.DynamoDbAsyncClient;
import software.amazon.awssdk.services.dynamodb.model.AttributeDefinition;
import software.amazon.awssdk.services.dynamodb.model.AttributeValue;
import software.amazon.awssdk.services.dynamodb.model.BillingMode;
import software.amazon.awssdk.services.dynamodb.model.KeySchemaElement;
import software.amazon.awssdk.services.dynamodb.model.KeyType;
import software.amazon.awssdk.services.dynamodb.model.ScalarAttributeType;
import software.amazon.awssdk.services.dynamodb.model.StreamSpecification;
import software.amazon.awssdk.services.dynamodb.model.StreamViewType;
import software.amazon.awssdk.services.dynamodb.model.TableDescription;
import software.amazon.awssdk.services.dynamodb.model.TableStatus;
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
 *
 * <p>With {@code SOURCE_TABLE} set, it runs on a real stream instead (task 16,
 * Requirement 14.5, kcl_on_kythira.sh): it creates that table with a stream,
 * writes {@code ITEMS} items before and while the worker runs, and fails
 * unless the worker sees every one exactly once.
 */
public final class Main {
    static final CountDownLatch PROCESSED = new CountDownLatch(1);
    /** Records seen per item key (the real-stream flow). */
    static final Map<String, Integer> SEEN = new ConcurrentHashMap<>();

    static final class Processor implements DynamoDBStreamsShardRecordProcessor {
        @Override public void initialize(InitializationInput in) { }

        @Override
        public void processRecords(DynamoDBStreamsProcessRecordsInput in) {
            if (!in.records().isEmpty()) {
                in.records().forEach(r -> {
                    var keys = r.getRecord().dynamodb().keys();
                    if (keys != null && keys.containsKey("pk")) {
                        SEEN.merge(keys.get("pk").s(), 1, Integer::sum);
                    }
                });
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

        String source = System.getenv("SOURCE_TABLE");
        int items = Integer.parseInt(System.getenv().getOrDefault("ITEMS", "60"));
        String streamArn = "arn:aws:dynamodb:us-east-1:123456789012:table/evidence/stream/"
                + "2026-11-09T00:00:00.000";
        if (source != null) {
            streamArn = createSource(dynamo, source);
            write(dynamo, source, 0, items / 2);
        }
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
        if (source != null && got) {
            // The rest go to a shard the worker is already reading.
            write(dynamo, source, items / 2, items);
            long deadline = System.currentTimeMillis() + 120_000L;
            while (SEEN.size() < items && System.currentTimeMillis() < deadline) {
                Thread.sleep(500L);
            }
        }
        Thread.sleep(12_000L);
        scheduler.startGracefulShutdown().get(60, TimeUnit.SECONDS);
        if (source != null) {
            long twice = SEEN.values().stream().filter(n -> n > 1).count();
            boolean ok = got && SEEN.size() == items && twice == 0;
            System.out.println("streams adapter on " + source + ": " + SEEN.size() + " of " + items
                    + " items seen, " + twice + " more than once");
            System.exit(ok ? 0 : 1);
        }
        System.out.println(got ? "streams adapter evidence flow done" : "streams adapter: no records processed");
        System.exit(got ? 0 : 1);
    }

    /** Creates {@code name} with a NEW_IMAGE stream (or finds it) and returns the stream's ARN. */
    static String createSource(DynamoDbAsyncClient dynamo, String name) throws Exception {
        try {
            dynamo.createTable(b -> b.tableName(name)
                    .attributeDefinitions(AttributeDefinition.builder().attributeName("pk")
                            .attributeType(ScalarAttributeType.S).build())
                    .keySchema(KeySchemaElement.builder().attributeName("pk").keyType(KeyType.HASH).build())
                    .billingMode(BillingMode.PAY_PER_REQUEST)
                    .streamSpecification(StreamSpecification.builder().streamEnabled(true)
                            .streamViewType(StreamViewType.NEW_IMAGE).build())).get();
        } catch (java.util.concurrent.ExecutionException e) {
            if (!e.getCause().getClass().getSimpleName().equals("ResourceInUseException")) {
                throw e;
            }
        }
        long deadline = System.currentTimeMillis() + 60_000L;
        while (true) {
            TableDescription t = dynamo.describeTable(b -> b.tableName(name)).get().table();
            if (t.tableStatus() == TableStatus.ACTIVE && t.latestStreamArn() != null) {
                return t.latestStreamArn();
            }
            if (System.currentTimeMillis() > deadline) {
                throw new IllegalStateException(name + " did not become ACTIVE");
            }
            Thread.sleep(500L);
        }
    }

    /** Puts items {@code from} up to {@code to}. */
    static void write(DynamoDbAsyncClient dynamo, String table, int from, int to) throws Exception {
        for (int i = from; i < to; ++i) {
            Map<String, AttributeValue> item = Map.of(
                    "pk", AttributeValue.builder().s(String.format("item-%04d", i)).build(),
                    "n", AttributeValue.builder().n(Integer.toString(i)).build());
            dynamo.putItem(b -> b.tableName(table).item(item)).get();
        }
    }
}
