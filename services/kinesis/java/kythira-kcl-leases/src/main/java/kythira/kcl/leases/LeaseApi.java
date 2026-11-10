// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

package kythira.kcl.leases;

import com.fasterxml.jackson.databind.node.ObjectNode;
import java.util.List;
import java.util.Optional;
import java.util.OptionalLong;

/**
 * One application's leases on Kythira's Lease API ({@code /v1/leases/<application>/}).
 *
 * <p>Every lease is a JSON object with a version, the log index of the write that last changed it.
 * Writes are conditional on that version, which is all a refresher needs to build KCL's
 * conditions on top: read, check, write if the version has not moved.
 *
 * <p>{@link HttpLeaseApi} talks to a gateway; tests use an in-memory implementation.
 */
public interface LeaseApi {

    /** A lease and the version it was read at. */
    record Versioned(String key, long version, ObjectNode item) {}

    /** One page of a list; {@code nextStartAfter} is null on the last page. */
    record Page(List<Versioned> leases, String nextStartAfter) {}

    /** What a write requires of the lease there now. */
    sealed interface Condition {
        /** Whatever is there, or nothing. */
        record Any() implements Condition {}

        /** No lease under the key. */
        record Absent() implements Condition {}

        /** The lease is at exactly this version. */
        record AtVersion(long version) implements Condition {}

        Condition ANY = new Any();
        Condition ABSENT = new Absent();

        static Condition at(long version) {
            return new AtVersion(version);
        }
    }

    /** Reads one lease; empty when there is none. */
    Optional<Versioned> get(String key) throws LeaseApiException;

    /** Lists leases in key order, after {@code startAfter} (null: from the first). */
    Page list(String startAfter, int limit) throws LeaseApiException;

    /** Writes a whole lease. Returns the new version, or empty when the condition failed. */
    OptionalLong put(String key, ObjectNode item, Condition condition) throws LeaseApiException;

    /**
     * Deletes a lease. Returns true when it was deleted, false when the condition failed or there
     * was no lease.
     */
    boolean delete(String key, Condition condition) throws LeaseApiException;
}
