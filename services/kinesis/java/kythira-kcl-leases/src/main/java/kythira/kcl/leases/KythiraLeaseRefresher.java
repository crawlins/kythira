// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

package kythira.kcl.leases;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.OptionalLong;
import java.util.concurrent.ConcurrentHashMap;
import java.util.logging.Level;
import java.util.logging.Logger;
import software.amazon.awssdk.services.dynamodb.model.AttributeValue;
import software.amazon.awssdk.services.dynamodb.model.AttributeValueUpdate;
import software.amazon.awssdk.services.dynamodb.model.ExpectedAttributeValue;
import software.amazon.kinesis.common.StreamIdentifier;
import software.amazon.kinesis.leases.Lease;
import software.amazon.kinesis.leases.LeaseSerializer;
import software.amazon.kinesis.leases.UpdateField;
import software.amazon.kinesis.leases.dynamodb.DynamoDBLeaseRefresher;
import software.amazon.kinesis.leases.exceptions.DependencyException;
import software.amazon.kinesis.leases.exceptions.ProvisionedThroughputException;
import software.amazon.kinesis.retrieval.kpl.ExtendedSequenceNumber;

/**
 * KCL 2.x's {@code LeaseRefresher} on Kythira's Lease API.
 *
 * <p>Every method keeps the contract of KCL's {@link DynamoDBLeaseRefresher}, which this class
 * extends only because {@code LeaseManagementFactory.createLeaseRefresher()} is declared to
 * return one: no DynamoDB client is held and every method is overridden.
 *
 * <p>Leases are KCL's own DynamoDB items ({@link LeaseSerializer}), stored as DynamoDB JSON. A
 * conditional update reads the lease and its version, checks the serializer's expectations on
 * it, applies the serializer's updates, and writes the result only if the version has not moved;
 * if it has, it reads again and re-checks. That is DynamoDB's conditional update, built on one
 * compare-and-set, so renewals, takes, evictions and checkpoints race exactly as they do on AWS.
 *
 * <p>The version each lease was last seen at is cached, so the usual case (a worker renewing the
 * leases it holds) is a single write; a stale cache costs one extra read.
 */
public class KythiraLeaseRefresher extends DynamoDBLeaseRefresher {
    private static final Logger LOG = Logger.getLogger(KythiraLeaseRefresher.class.getName());
    private static final int PAGE = 1000;
    private static final int MAX_ATTEMPTS = 8;
    private static final String STREAM_NAME = "streamName";

    private final LeaseApi api;
    private final LeaseSerializer serializer;
    private final Map<String, LeaseApi.Versioned> seen = new ConcurrentHashMap<>();

    /**
     * @param application the KCL application name, which names the lease set
     * @param api the application's leases
     * @param serializer KCL's serializer for the lease type in use: {@code DynamoDBLeaseSerializer},
     *     or {@code DynamoDBMultiStreamLeaseSerializer} for a multi-stream application
     */
    public KythiraLeaseRefresher(String application, LeaseApi api, LeaseSerializer serializer) {
        super(application, null, serializer, true);
        this.api = api;
        this.serializer = serializer;
    }

    private static DependencyException dependency(String what, LeaseApiException e)
            throws ProvisionedThroughputException {
        if (e.status() == 429) {
            throw new ProvisionedThroughputException(e);
        }
        return new DependencyException("lease API " + what + " failed", e);
    }

    private Lease lease(LeaseApi.Versioned v) {
        seen.put(v.key(), v);
        return serializer.fromDynamoRecord(DynamoItems.fromJsonItem(v.item()));
    }

    private Optional<LeaseApi.Versioned> fetch(String key) throws DependencyException,
            ProvisionedThroughputException {
        try {
            Optional<LeaseApi.Versioned> v = api.get(key);
            if (v.isPresent()) {
                seen.put(key, v.get());
            } else {
                seen.remove(key);
            }
            return v;
        } catch (LeaseApiException e) {
            throw dependency("get " + key, e);
        }
    }

    /**
     * DynamoDB's {@code UpdateItem} with legacy {@code Expected} and {@code AttributeUpdates}:
     * true when the expectations held and the update is written, false when they did not hold.
     */
    boolean conditionalUpdate(String key, Map<String, ExpectedAttributeValue> expected,
            Map<String, AttributeValueUpdate> updates)
            throws DependencyException, ProvisionedThroughputException {
        LeaseApi.Versioned current = seen.get(key);
        boolean fromCache = current != null;
        for (int attempt = 0; attempt < MAX_ATTEMPTS; ++attempt) {
            if (current == null && !fromCache) {
                current = fetch(key).orElse(null);
            }
            Map<String, AttributeValue> item =
                    current == null ? null : DynamoItems.fromJsonItem(current.item());
            if (!DynamoItems.meets(item, expected)) {
                if (fromCache) {
                    // The cache may be behind; only the store's answer counts.
                    fromCache = false;
                    current = null;
                    continue;
                }
                return false;
            }
            // Like DynamoDB, an update of an absent item whose expectations hold creates it.
            Map<String, AttributeValue> base =
                    item == null ? new HashMap<>(serializer.getDynamoHashKey(key)) : item;
            Map<String, AttributeValue> next = DynamoItems.apply(base, updates);
            LeaseApi.Condition condition = current == null
                    ? LeaseApi.Condition.ABSENT
                    : LeaseApi.Condition.at(current.version());
            OptionalLong written;
            try {
                written = api.put(key, DynamoItems.toJson(next), condition);
            } catch (LeaseApiException e) {
                seen.remove(key);
                throw dependency("update " + key, e);
            }
            if (written.isPresent()) {
                seen.put(key, new LeaseApi.Versioned(key, written.getAsLong(), DynamoItems.toJson(next)));
                return true;
            }
            // Someone wrote in between: read again and re-check.
            fromCache = false;
            current = null;
        }
        throw new DependencyException("lease " + key + " kept changing under " + MAX_ATTEMPTS
                + " conditional updates", null);
    }

    // ---- the table: there is none to create ---------------------------------

    @Override
    @Deprecated
    public boolean createLeaseTableIfNotExists(Long readCapacity, Long writeCapacity) {
        return false;
    }

    @Override
    public boolean createLeaseTableIfNotExists() {
        return false;
    }

    @Override
    public boolean leaseTableExists() {
        return true;
    }

    @Override
    public boolean waitUntilLeaseTableExists(long secondsBetweenPolls, long timeoutSeconds) {
        return true;
    }

    // ---- reads ----------------------------------------------------------------

    private List<Lease> listAll(StreamIdentifier stream)
            throws DependencyException, ProvisionedThroughputException {
        List<Lease> out = new ArrayList<>();
        String after = null;
        do {
            LeaseApi.Page page;
            try {
                page = api.list(after, PAGE);
            } catch (LeaseApiException e) {
                throw dependency("list", e);
            }
            for (LeaseApi.Versioned v : page.leases()) {
                if (stream != null) {
                    var name = v.item().path(STREAM_NAME).path("S");
                    if (!name.isTextual() || !name.asText().equals(stream.serialize())) {
                        continue;
                    }
                }
                out.add(lease(v));
            }
            after = page.nextStartAfter();
        } while (after != null);
        return out;
    }

    @Override
    public List<Lease> listLeasesForStream(StreamIdentifier streamIdentifier)
            throws DependencyException, ProvisionedThroughputException {
        return listAll(streamIdentifier);
    }

    @Override
    public List<Lease> listLeases() throws DependencyException, ProvisionedThroughputException {
        return listAll(null);
    }

    @Override
    public boolean isLeaseTableEmpty() throws DependencyException, ProvisionedThroughputException {
        try {
            return api.list(null, 1).leases().isEmpty();
        } catch (LeaseApiException e) {
            throw dependency("list", e);
        }
    }

    @Override
    public Lease getLease(String leaseKey) throws DependencyException, ProvisionedThroughputException {
        return fetch(leaseKey).map(this::lease).orElse(null);
    }

    @Override
    public ExtendedSequenceNumber getCheckpoint(String leaseKey)
            throws DependencyException, ProvisionedThroughputException {
        Lease l = getLease(leaseKey);
        return l == null ? null : l.checkpoint();
    }

    // ---- writes ---------------------------------------------------------------

    @Override
    public boolean createLeaseIfNotExists(Lease lease)
            throws DependencyException, ProvisionedThroughputException {
        var item = DynamoItems.toJson(serializer.toDynamoRecord(lease));
        try {
            OptionalLong v = api.put(lease.leaseKey(), item, LeaseApi.Condition.ABSENT);
            if (v.isEmpty()) {
                LOG.fine(() -> "Did not create lease " + lease.leaseKey() + " because it already existed");
                return false;
            }
            seen.put(lease.leaseKey(), new LeaseApi.Versioned(lease.leaseKey(), v.getAsLong(), item));
            LOG.info(() -> "Created lease: " + lease);
            return true;
        } catch (LeaseApiException e) {
            throw dependency("create " + lease.leaseKey(), e);
        }
    }

    @Override
    public boolean renewLease(Lease lease) throws DependencyException, ProvisionedThroughputException {
        if (!conditionalUpdate(lease.leaseKey(), serializer.getDynamoLeaseCounterExpectation(lease),
                serializer.getDynamoLeaseCounterUpdate(lease))) {
            // As KCL does: a renewal whose answer was lost may have landed.
            Lease now = getLease(lease.leaseKey());
            if (now == null || lease.leaseOwner() == null
                    || !lease.leaseOwner().equals(now.leaseOwner())
                    || !Long.valueOf(lease.leaseCounter() + 1).equals(now.leaseCounter())) {
                return false;
            }
            LOG.info(() -> "Detected spurious renewal failure for lease with key "
                    + lease.leaseKey() + ", but recovered");
        }
        lease.leaseCounter(lease.leaseCounter() + 1);
        return true;
    }

    @Override
    public boolean takeLease(Lease lease, String owner)
            throws DependencyException, ProvisionedThroughputException {
        String oldOwner = lease.leaseOwner();
        Map<String, AttributeValueUpdate> updates = serializer.getDynamoLeaseCounterUpdate(lease);
        updates.putAll(serializer.getDynamoTakeLeaseUpdate(lease, owner));
        if (!conditionalUpdate(lease.leaseKey(), serializer.getDynamoLeaseCounterExpectation(lease),
                updates)) {
            return false;
        }
        lease.leaseCounter(lease.leaseCounter() + 1);
        lease.leaseOwner(owner);
        if (oldOwner != null && !oldOwner.equals(owner)) {
            lease.ownerSwitchesSinceCheckpoint(lease.ownerSwitchesSinceCheckpoint() + 1);
        }
        LOG.info(() -> "Transferred lease " + lease.leaseKey() + " ownership from " + oldOwner
                + " to " + owner);
        return true;
    }

    @Override
    public boolean evictLease(Lease lease) throws DependencyException, ProvisionedThroughputException {
        Map<String, AttributeValueUpdate> updates = serializer.getDynamoLeaseCounterUpdate(lease);
        updates.putAll(serializer.getDynamoEvictLeaseUpdate(lease));
        if (!conditionalUpdate(lease.leaseKey(), serializer.getDynamoLeaseOwnerExpectation(lease),
                updates)) {
            return false;
        }
        lease.leaseOwner(null);
        lease.leaseCounter(lease.leaseCounter() + 1);
        LOG.info(() -> "Evicted lease with leaseKey " + lease.leaseKey());
        return true;
    }

    @Override
    public boolean updateLease(Lease lease) throws DependencyException, ProvisionedThroughputException {
        Map<String, AttributeValueUpdate> updates = serializer.getDynamoLeaseCounterUpdate(lease);
        updates.putAll(serializer.getDynamoUpdateLeaseUpdate(lease));
        if (!conditionalUpdate(lease.leaseKey(), serializer.getDynamoLeaseCounterExpectation(lease),
                updates)) {
            return false;
        }
        lease.leaseCounter(lease.leaseCounter() + 1);
        return true;
    }

    @Override
    public void updateLeaseWithMetaInfo(Lease lease, UpdateField updateField)
            throws DependencyException, ProvisionedThroughputException {
        if (!conditionalUpdate(lease.leaseKey(),
                serializer.getDynamoExistentExpectation(lease.leaseKey()),
                serializer.getDynamoUpdateLeaseUpdate(lease, updateField))) {
            LOG.log(Level.WARNING, () -> "Lease update failed for lease with key "
                    + lease.leaseKey() + " because the lease did not exist at the time of the update");
        }
    }

    @Override
    public void deleteLease(Lease lease) throws DependencyException, ProvisionedThroughputException {
        try {
            api.delete(lease.leaseKey(), LeaseApi.Condition.ANY);
            seen.remove(lease.leaseKey());
        } catch (LeaseApiException e) {
            throw dependency("delete " + lease.leaseKey(), e);
        }
    }

    @Override
    public void deleteAll() throws DependencyException, ProvisionedThroughputException {
        for (Lease l : listLeases()) {
            deleteLease(l);
        }
    }
}
