// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

package kythira.kcl.leases;

import com.fasterxml.jackson.databind.node.ObjectNode;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.OptionalLong;
import java.util.TreeMap;
import java.util.concurrent.atomic.AtomicInteger;

/**
 * The Lease API's semantics in memory: versions from one counter (the server uses the log index,
 * which also only grows), conditions on them, key-ordered pages.
 */
final class InMemoryLeaseApi implements LeaseApi {
    private final TreeMap<String, Versioned> leases = new TreeMap<>();
    private long index = 0;
    final AtomicInteger gets = new AtomicInteger();
    final AtomicInteger puts = new AtomicInteger();
    /** Called before each put is decided: lets a test race another writer in. */
    Runnable beforePut = () -> {};

    @Override
    public synchronized Optional<Versioned> get(String key) {
        gets.incrementAndGet();
        ++index;
        return Optional.ofNullable(leases.get(key)).map(v -> new Versioned(v.key(), v.version(), v.item().deepCopy()));
    }

    @Override
    public synchronized Page list(String startAfter, int limit) {
        ++index;
        Map<String, Versioned> tail = startAfter == null ? leases : leases.tailMap(startAfter, false);
        List<Versioned> out = new ArrayList<>();
        String next = null;
        for (Versioned v : tail.values()) {
            if (out.size() == limit) {
                next = out.get(out.size() - 1).key();
                break;
            }
            out.add(new Versioned(v.key(), v.version(), v.item().deepCopy()));
        }
        return new Page(out, next);
    }

    private boolean holds(String key, Condition c) {
        Versioned have = leases.get(key);
        if (c instanceof Condition.Absent) {
            return have == null;
        }
        if (c instanceof Condition.AtVersion v) {
            return have != null && have.version() == v.version();
        }
        return true;
    }

    @Override
    public OptionalLong put(String key, ObjectNode item, Condition condition) {
        beforePut.run();
        synchronized (this) {
            puts.incrementAndGet();
            ++index;
            if (!holds(key, condition)) {
                return OptionalLong.empty();
            }
            leases.put(key, new Versioned(key, index, item.deepCopy()));
            return OptionalLong.of(index);
        }
    }

    @Override
    public synchronized boolean delete(String key, Condition condition) {
        ++index;
        if (!leases.containsKey(key) || !holds(key, condition)) {
            return false;
        }
        leases.remove(key);
        return true;
    }

    synchronized int size() {
        return leases.size();
    }
}
