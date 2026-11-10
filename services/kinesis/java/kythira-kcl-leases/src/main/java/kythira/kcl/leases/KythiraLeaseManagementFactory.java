// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

package kythira.kcl.leases;

import software.amazon.kinesis.leases.LeaseManagementConfig;
import software.amazon.kinesis.leases.LeaseSerializer;
import software.amazon.kinesis.leases.dynamodb.DynamoDBLeaseManagementFactory;
import software.amazon.kinesis.leases.dynamodb.DynamoDBLeaseRefresher;
import software.amazon.kinesis.leases.dynamodb.DynamoDBLeaseSerializer;
import software.amazon.kinesis.leases.dynamodb.DynamoDBMultiStreamLeaseSerializer;

/**
 * KCL 2.x's lease management with leases on Kythira.
 *
 * <p>Everything is KCL's own (lease coordinator, taker, renewer, shard sync, cleanup) except the
 * refresher, which is {@link KythiraLeaseRefresher}. Install it on the application's {@code
 * LeaseManagementConfig} before building the scheduler:
 *
 * <pre>{@code
 * ConfigsBuilder configs = new ConfigsBuilder(stream, application, kinesis, dynamo, cloudWatch,
 *         workerId, processorFactory);
 * LeaseManagementConfig leases = configs.leaseManagementConfig();
 * LeaseApi api = new HttpLeaseApi(endpoint, application, credentials, region);
 * leases.leaseManagementFactory(KythiraLeaseManagementFactory.create(leases, api, false));
 * }</pre>
 *
 * <p>The {@code DynamoDbAsyncClient} the config carries is never called; KCL requires one to
 * exist.
 */
public class KythiraLeaseManagementFactory extends DynamoDBLeaseManagementFactory {
    private final String application;
    private final LeaseApi api;
    private final LeaseSerializer serializer;
    private KythiraLeaseRefresher refresher;

    /**
     * The factory KCL would build from {@code config}, with leases on {@code api}.
     *
     * @param multiStream whether the application consumes several streams (KCL's multi-stream
     *     mode), which picks the lease serializer
     */
    public static KythiraLeaseManagementFactory create(
            LeaseManagementConfig config, LeaseApi api, boolean multiStream) {
        LeaseSerializer serializer = multiStream
                ? new DynamoDBMultiStreamLeaseSerializer()
                : new DynamoDBLeaseSerializer();
        return new KythiraLeaseManagementFactory(config, api, serializer, multiStream);
    }

    protected KythiraLeaseManagementFactory(
            LeaseManagementConfig c, LeaseApi api, LeaseSerializer serializer, boolean multiStream) {
        super(
                c.kinesisClient(),
                c.dynamoDBClient(),
                c.tableName(),
                c.workerIdentifier(),
                c.executorService(),
                c.failoverTimeMillis(),
                c.enablePriorityLeaseAssignment(),
                c.epsilonMillis(),
                c.maxLeasesForWorker(),
                c.maxLeasesToStealAtOneTime(),
                c.maxLeaseRenewalThreads(),
                c.cleanupLeasesUponShardCompletion(),
                c.ignoreUnexpectedChildShards(),
                c.shardSyncIntervalMillis(),
                c.consistentReads(),
                c.listShardsBackoffTimeInMillis(),
                c.maxListShardsRetryAttempts(),
                c.maxCacheMissesBeforeReload(),
                c.listShardsCacheAllowedAgeInSeconds(),
                c.cacheMissWarningModulus(),
                c.initialLeaseTableReadCapacity(),
                c.initialLeaseTableWriteCapacity(),
                c.hierarchicalShardSyncer(),
                c.tableCreatorCallback(),
                c.dynamoDbRequestTimeout(),
                c.billingMode(),
                c.leaseTableDeletionProtectionEnabled(),
                c.leaseTablePitrEnabled(),
                c.tags(),
                serializer,
                c.customShardDetectorProvider(),
                multiStream,
                c.leaseCleanupConfig());
        this.application = c.tableName();
        this.api = api;
        this.serializer = serializer;
    }

    /**
     * One refresher for the coordinator, shard sync and cleanup, so they share its cache of lease
     * versions.
     */
    @Override
    public synchronized DynamoDBLeaseRefresher createLeaseRefresher() {
        if (refresher == null) {
            refresher = new KythiraLeaseRefresher(application, api, serializer);
        }
        return refresher;
    }
}
