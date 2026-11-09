// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

package evidence;

import com.amazonaws.services.dynamodbv2.AcquireLockOptions;
import com.amazonaws.services.dynamodbv2.AmazonDynamoDBLockClient;
import com.amazonaws.services.dynamodbv2.AmazonDynamoDBLockClientOptions;
import com.amazonaws.services.dynamodbv2.CreateDynamoDBTableOptions;
import com.amazonaws.services.dynamodbv2.GetLockOptions;
import com.amazonaws.services.dynamodbv2.LockItem;
import com.amazonaws.services.dynamodbv2.SendHeartbeatOptions;
import java.net.URI;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.util.Optional;
import java.util.concurrent.TimeUnit;
import software.amazon.awssdk.auth.credentials.AwsBasicCredentials;
import software.amazon.awssdk.auth.credentials.StaticCredentialsProvider;
import software.amazon.awssdk.http.urlconnection.UrlConnectionHttpClient;
import software.amazon.awssdk.regions.Region;
import software.amazon.awssdk.services.dynamodb.DynamoDbClient;
import software.amazon.awssdk.services.dynamodb.model.ProvisionedThroughput;

/**
 * The DynamoDB lock client evidence flow (task 1): create the lock table,
 * acquire a lock with data, heartbeat it, read it, release it, and acquire
 * one with a sort key.
 */
public final class Main {
    private static final ProvisionedThroughput THROUGHPUT =
            ProvisionedThroughput.builder().readCapacityUnits(5L).writeCapacityUnits(5L).build();

    public static void main(String[] args) throws Exception {
        String endpoint = System.getenv().getOrDefault("ENDPOINT", "http://localhost:8000");
        DynamoDbClient ddb = DynamoDbClient.builder()
                .endpointOverride(URI.create(endpoint))
                .region(Region.US_EAST_1)
                .httpClient(UrlConnectionHttpClient.create())
                .credentialsProvider(StaticCredentialsProvider.create(AwsBasicCredentials.create(
                        "EVIDENCE-lock-client", "evidence-secret-not-a-credential")))
                .build();

        AmazonDynamoDBLockClient.createLockTableInDynamoDB(CreateDynamoDBTableOptions
                .builder(ddb, THROUGHPUT, "evidence-locks")
                .build());
        AmazonDynamoDBLockClient.createLockTableInDynamoDB(CreateDynamoDBTableOptions
                .builder(ddb, THROUGHPUT, "evidence-locks-sorted")
                .withSortKeyName("sortKey")
                .build());

        try (AmazonDynamoDBLockClient client = new AmazonDynamoDBLockClient(
                AmazonDynamoDBLockClientOptions.builder(ddb, "evidence-locks")
                        .withTimeUnit(TimeUnit.SECONDS)
                        .withLeaseDuration(10L)
                        .withHeartbeatPeriod(3L)
                        .withCreateHeartbeatBackgroundThread(false)
                        .withOwnerName("evidence-owner")
                        .build())) {
            client.assertLockTableExists();
            LockItem lock = client.acquireLock(AcquireLockOptions.builder("resource-1")
                    .withData(ByteBuffer.wrap("payload".getBytes(StandardCharsets.UTF_8)))
                    .withAdditionalTimeToWaitForLock(1L)
                    .withTimeUnit(TimeUnit.SECONDS)
                    .build());
            client.sendHeartbeat(SendHeartbeatOptions.builder(lock).build());
            client.getLock("resource-1", Optional.empty());
            client.getLockFromDynamoDB(GetLockOptions.builder("resource-1").build());
            client.releaseLock(lock);
            Optional<LockItem> again = client.tryAcquireLock(AcquireLockOptions.builder("resource-2")
                    .withDeleteLockOnRelease(false)
                    .build());
            again.ifPresent(client::releaseLock);
        }

        try (AmazonDynamoDBLockClient sorted = new AmazonDynamoDBLockClient(
                AmazonDynamoDBLockClientOptions.builder(ddb, "evidence-locks-sorted")
                        .withSortKeyName("sortKey")
                        .withCreateHeartbeatBackgroundThread(false)
                        .build())) {
            LockItem lock = sorted.acquireLock(AcquireLockOptions.builder("resource-3")
                    .withSortKey("part-a")
                    .build());
            sorted.releaseLock(lock);
        }
        System.out.println("lock client evidence flow done");
    }
}
