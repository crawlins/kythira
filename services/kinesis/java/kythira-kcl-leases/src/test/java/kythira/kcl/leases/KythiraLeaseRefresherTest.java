// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

package kythira.kcl.leases;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNotNull;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;
import org.junit.jupiter.api.Test;
import software.amazon.awssdk.services.dynamodb.model.AttributeAction;
import software.amazon.awssdk.services.dynamodb.model.AttributeValue;
import software.amazon.awssdk.services.dynamodb.model.AttributeValueUpdate;
import software.amazon.awssdk.services.dynamodb.model.ExpectedAttributeValue;
import software.amazon.kinesis.common.HashKeyRangeForLease;
import software.amazon.kinesis.common.StreamIdentifier;
import software.amazon.kinesis.leases.Lease;
import software.amazon.kinesis.leases.MultiStreamLease;
import software.amazon.kinesis.leases.UpdateField;
import software.amazon.kinesis.leases.dynamodb.DynamoDBLeaseSerializer;
import software.amazon.kinesis.leases.dynamodb.DynamoDBMultiStreamLeaseSerializer;
import software.amazon.kinesis.retrieval.kpl.ExtendedSequenceNumber;

class KythiraLeaseRefresherTest {

    private static Lease newLease(String key) {
        Lease l = new Lease();
        l.leaseKey(key);
        l.checkpoint(ExtendedSequenceNumber.TRIM_HORIZON);
        l.parentShardIds(Set.of("shardId-000000000009"));
        l.hashKeyRange(HashKeyRangeForLease.deserialize("0", "170141183460469231731687303715884105727"));
        return l;
    }

    private static KythiraLeaseRefresher refresher(InMemoryLeaseApi api) {
        return new KythiraLeaseRefresher("orders-app", api, new DynamoDBLeaseSerializer());
    }

    @Test
    void createsReadsAndRefusesDuplicates() throws Exception {
        InMemoryLeaseApi api = new InMemoryLeaseApi();
        KythiraLeaseRefresher r = refresher(api);
        assertTrue(r.isLeaseTableEmpty());
        assertTrue(r.createLeaseIfNotExists(newLease("shardId-000000000000")));
        assertFalse(r.createLeaseIfNotExists(newLease("shardId-000000000000")));
        Lease got = r.getLease("shardId-000000000000");
        assertNotNull(got);
        assertEquals(ExtendedSequenceNumber.TRIM_HORIZON, got.checkpoint());
        assertEquals(Set.of("shardId-000000000009"), got.parentShardIds());
        assertEquals("170141183460469231731687303715884105727",
                got.hashKeyRangeForLease().serializedEndingHashKey());
        assertEquals(0L, got.leaseCounter());
        assertNull(got.leaseOwner());
        assertEquals(ExtendedSequenceNumber.TRIM_HORIZON, r.getCheckpoint("shardId-000000000000"));
        assertNull(r.getLease("shardId-000000000001"));
        assertFalse(r.isLeaseTableEmpty());
        assertTrue(r.leaseTableExists());
    }

    @Test
    void takeRenewAndLoseToAnotherWorker() throws Exception {
        InMemoryLeaseApi api = new InMemoryLeaseApi();
        KythiraLeaseRefresher a = refresher(api);
        KythiraLeaseRefresher b = refresher(api);
        a.createLeaseIfNotExists(newLease("shardId-0"));

        Lease mine = a.getLease("shardId-0");
        assertTrue(a.takeLease(mine, "worker-a"));
        assertEquals(1L, mine.leaseCounter());
        int getsBefore = api.gets.get();
        assertTrue(a.renewLease(mine));
        assertTrue(a.renewLease(mine));
        assertEquals(3L, mine.leaseCounter());
        // Renewing a lease this worker last wrote needs no read.
        assertEquals(getsBefore, api.gets.get());

        // Worker b sees the lease at counter 3 and steals it.
        Lease seen = b.getLease("shardId-0");
        assertTrue(b.takeLease(seen, "worker-b"));
        assertEquals("worker-b", seen.leaseOwner());
        assertEquals(1L, seen.ownerSwitchesSinceCheckpoint());
        // a's cache is stale: its renewal reads, sees the counter moved, and fails.
        assertFalse(a.renewLease(mine));
        Lease stored = a.getLease("shardId-0");
        assertEquals("worker-b", stored.leaseOwner());
        assertEquals(4L, stored.leaseCounter());
        assertEquals(1L, stored.ownerSwitchesSinceCheckpoint());
    }

    @Test
    void exactlyOneOfTwoRacingTakersWins() throws Exception {
        InMemoryLeaseApi api = new InMemoryLeaseApi();
        refresher(api).createLeaseIfNotExists(newLease("shardId-0"));
        for (int round = 0; round < 50; ++round) {
            KythiraLeaseRefresher a = refresher(api);
            KythiraLeaseRefresher b = refresher(api);
            Lease la = a.getLease("shardId-0");
            Lease lb = b.getLease("shardId-0");
            CountDownLatch go = new CountDownLatch(1);
            ExecutorService pool = Executors.newFixedThreadPool(2);
            Future<Boolean> fa = pool.submit(() -> {
                go.await();
                return a.takeLease(la, "worker-a");
            });
            Future<Boolean> fb = pool.submit(() -> {
                go.await();
                return b.takeLease(lb, "worker-b");
            });
            go.countDown();
            boolean wa = fa.get();
            boolean wb = fb.get();
            pool.shutdown();
            assertTrue(wa ^ wb, "round " + round + ": a=" + wa + " b=" + wb);
            Lease stored = a.getLease("shardId-0");
            assertEquals(wa ? "worker-a" : "worker-b", stored.leaseOwner());
        }
    }

    @Test
    void aWriteBetweenReadAndWriteIsRecheckedNotOverwritten() throws Exception {
        InMemoryLeaseApi api = new InMemoryLeaseApi();
        KythiraLeaseRefresher a = refresher(api);
        KythiraLeaseRefresher b = refresher(api);
        a.createLeaseIfNotExists(newLease("shardId-0"));
        Lease mine = a.getLease("shardId-0");
        assertTrue(a.takeLease(mine, "worker-a"));
        // Just before a's next write lands, b takes the lease.
        Lease other = b.getLease("shardId-0");
        api.beforePut = () -> {
            api.beforePut = () -> {};
            try {
                assertTrue(b.takeLease(other, "worker-b"));
            } catch (Exception e) {
                throw new AssertionError(e);
            }
        };
        assertFalse(a.renewLease(mine));
        assertEquals("worker-b", a.getLease("shardId-0").leaseOwner());
    }

    @Test
    void evictionNeedsTheOwnerAndKeepsSwitchCounts() throws Exception {
        InMemoryLeaseApi api = new InMemoryLeaseApi();
        KythiraLeaseRefresher r = refresher(api);
        r.createLeaseIfNotExists(newLease("shardId-0"));
        Lease l = r.getLease("shardId-0");
        r.takeLease(l, "worker-a");
        Lease wrong = r.getLease("shardId-0");
        wrong.leaseOwner("worker-z");
        assertFalse(r.evictLease(wrong));
        assertTrue(r.evictLease(l));
        assertNull(l.leaseOwner());
        Lease stored = r.getLease("shardId-0");
        assertNull(stored.leaseOwner());
        assertEquals(2L, stored.leaseCounter());
        assertEquals(0L, stored.ownerSwitchesSinceCheckpoint());
    }

    @Test
    void checkpointsAndMetaInfoUpdates() throws Exception {
        InMemoryLeaseApi api = new InMemoryLeaseApi();
        KythiraLeaseRefresher r = refresher(api);
        r.createLeaseIfNotExists(newLease("shardId-0"));
        Lease l = r.getLease("shardId-0");
        r.takeLease(l, "worker-a");
        l.checkpoint(new ExtendedSequenceNumber("49590338271490256608559692538361571095921575989136588898", 3L));
        l.pendingCheckpoint(new ExtendedSequenceNumber("49590338271490256608559692538361571095921575989136588999"));
        assertTrue(r.updateLease(l));
        Lease stored = r.getLease("shardId-0");
        assertEquals(l.checkpoint(), stored.checkpoint());
        assertEquals(l.pendingCheckpoint(), stored.pendingCheckpoint());
        // A checkpoint without a pending one removes the pending fields.
        l.pendingCheckpoint(null);
        assertTrue(r.updateLease(l));
        assertNull(r.getLease("shardId-0").pendingCheckpoint());

        l.childShardIds(List.of("shardId-1", "shardId-2"));
        r.updateLeaseWithMetaInfo(l, UpdateField.CHILD_SHARDS);
        assertEquals(Set.of("shardId-1", "shardId-2"), Set.copyOf(r.getLease("shardId-0").childShardIds()));
        // Never creates a lease that is gone.
        Lease gone = newLease("shardId-7");
        gone.childShardIds(List.of("shardId-8"));
        r.updateLeaseWithMetaInfo(gone, UpdateField.CHILD_SHARDS);
        assertNull(r.getLease("shardId-7"));
    }

    @Test
    void listsPageAndFilterByStream() throws Exception {
        InMemoryLeaseApi api = new InMemoryLeaseApi();
        KythiraLeaseRefresher r = refresher(api);
        for (int i = 0; i < 1005; ++i) {
            r.createLeaseIfNotExists(newLease(String.format("shardId-%012d", i)));
        }
        assertEquals(1005, r.listLeases().size());
        r.deleteLease(newLease("shardId-000000000000"));
        assertEquals(1004, r.listLeases().size());
        r.deleteAll();
        assertEquals(0, api.size());

        KythiraLeaseRefresher multi =
                new KythiraLeaseRefresher("multi-app", api, new DynamoDBMultiStreamLeaseSerializer());
        StreamIdentifier orders = StreamIdentifier.multiStreamInstance("111122223333:orders:1700000000");
        StreamIdentifier billing = StreamIdentifier.multiStreamInstance("111122223333:billing:1700000000");
        for (StreamIdentifier s : List.of(orders, billing)) {
            MultiStreamLease l = new MultiStreamLease();
            l.streamIdentifier(s.serialize());
            l.shardId("shardId-000000000000");
            l.leaseKey(MultiStreamLease.getLeaseKey(s.serialize(), "shardId-000000000000"));
            l.checkpoint(ExtendedSequenceNumber.LATEST);
            assertTrue(multi.createLeaseIfNotExists(l));
        }
        List<Lease> mine = multi.listLeasesForStream(orders);
        assertEquals(1, mine.size());
        MultiStreamLease m = (MultiStreamLease) mine.get(0);
        assertEquals(orders.serialize(), m.streamIdentifier());
        assertEquals("shardId-000000000000", m.shardId());
        assertEquals(2, multi.listLeases().size());
    }

    @Test
    void dynamoItemsRoundTripAndBehaveLikeDynamoDb() {
        Map<String, AttributeValue> item = Map.of(
                "s", AttributeValue.fromS("x"),
                "n", AttributeValue.fromN("1.50"),
                "ss", AttributeValue.fromSs(List.of("a", "b")),
                "b", AttributeValue.fromB(software.amazon.awssdk.core.SdkBytes.fromUtf8String("hi")),
                "m", AttributeValue.fromM(Map.of("l", AttributeValue.fromL(List.of(AttributeValue.fromBool(true))))));
        assertEquals(item, DynamoItems.fromJsonItem(DynamoItems.toJson(item)));

        // Numbers compare by value; Exists=false means absent.
        assertTrue(DynamoItems.meets(item, Map.of("n",
                ExpectedAttributeValue.builder().value(AttributeValue.fromN("1.5")).build())));
        assertFalse(DynamoItems.meets(item, Map.of("s",
                ExpectedAttributeValue.builder().exists(false).build())));
        assertTrue(DynamoItems.meets(null, Map.of("s",
                ExpectedAttributeValue.builder().exists(false).build())));

        Map<String, AttributeValue> next = DynamoItems.apply(item, Map.of(
                "n", AttributeValueUpdate.builder().action(AttributeAction.ADD).value(AttributeValue.fromN("1")).build(),
                "new", AttributeValueUpdate.builder().action(AttributeAction.ADD).value(AttributeValue.fromN("1")).build(),
                "s", AttributeValueUpdate.builder().action(AttributeAction.DELETE).build(),
                "ss", AttributeValueUpdate.builder().action(AttributeAction.DELETE).value(AttributeValue.fromSs(List.of("a"))).build()));
        assertEquals("2.50", next.get("n").n());
        assertEquals("1", next.get("new").n());
        assertFalse(next.containsKey("s"));
        assertEquals(List.of("b"), next.get("ss").ss());
    }

    @Test
    void pathSegmentsArePercentEncoded() {
        assertEquals("111122223333%3Aorders%3A1%3AshardId-0",
                HttpLeaseApi.encodeSegment("111122223333:orders:1:shardId-0"));
        assertEquals("a%2Fb%20c~", HttpLeaseApi.encodeSegment("a/b c~"));
    }
}
