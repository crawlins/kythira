// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

package kythira.kcl.leases;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.node.ArrayNode;
import com.fasterxml.jackson.databind.node.JsonNodeFactory;
import com.fasterxml.jackson.databind.node.ObjectNode;
import java.math.BigDecimal;
import java.util.ArrayList;
import java.util.Base64;
import java.util.Iterator;
import java.util.LinkedHashMap;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Set;
import software.amazon.awssdk.core.SdkBytes;
import software.amazon.awssdk.services.dynamodb.model.AttributeAction;
import software.amazon.awssdk.services.dynamodb.model.AttributeValue;
import software.amazon.awssdk.services.dynamodb.model.AttributeValueUpdate;
import software.amazon.awssdk.services.dynamodb.model.ComparisonOperator;
import software.amazon.awssdk.services.dynamodb.model.ExpectedAttributeValue;

/**
 * Leases as DynamoDB items, stored in the Lease API as DynamoDB JSON ({@code {"S": "w1"}}, {@code
 * {"N": "3"}}, ...).
 *
 * <p>Keeping KCL's own item shape lets the refresher use KCL's {@code LeaseSerializer} for every
 * lease type, multi-stream leases included, and evaluate the serializer's legacy {@code Expected}
 * conditions and {@code AttributeUpdates} itself, exactly as DynamoDB would, before a
 * version-conditional write.
 */
final class DynamoItems {
    private static final JsonNodeFactory NODES = JsonNodeFactory.instance;

    private DynamoItems() {}

    static JsonNode toJson(AttributeValue v) {
        ObjectNode o = NODES.objectNode();
        switch (v.type()) {
            case S -> o.put("S", v.s());
            case N -> o.put("N", v.n());
            case B -> o.put("B", Base64.getEncoder().encodeToString(v.b().asByteArray()));
            case BOOL -> o.put("BOOL", v.bool());
            case NUL -> o.put("NULL", true);
            case SS -> {
                ArrayNode a = o.putArray("SS");
                v.ss().forEach(a::add);
            }
            case NS -> {
                ArrayNode a = o.putArray("NS");
                v.ns().forEach(a::add);
            }
            case BS -> {
                ArrayNode a = o.putArray("BS");
                v.bs().forEach(b -> a.add(Base64.getEncoder().encodeToString(b.asByteArray())));
            }
            case L -> {
                ArrayNode a = o.putArray("L");
                v.l().forEach(e -> a.add(toJson(e)));
            }
            case M -> o.set("M", toJson(v.m()));
            default -> throw new IllegalArgumentException("unsupported attribute type " + v.type());
        }
        return o;
    }

    static ObjectNode toJson(Map<String, AttributeValue> item) {
        ObjectNode o = NODES.objectNode();
        // Sorted, so equal items serialize equally.
        item.keySet().stream().sorted().forEach(k -> o.set(k, toJson(item.get(k))));
        return o;
    }

    private static List<String> strings(JsonNode a) {
        List<String> out = new ArrayList<>();
        a.forEach(e -> out.add(e.asText()));
        return out;
    }

    static AttributeValue fromJson(JsonNode n) {
        if (!n.isObject() || n.size() != 1) {
            throw new IllegalArgumentException("not a DynamoDB JSON value: " + n);
        }
        Map.Entry<String, JsonNode> e = n.fields().next();
        JsonNode v = e.getValue();
        return switch (e.getKey()) {
            case "S" -> AttributeValue.fromS(v.asText());
            case "N" -> AttributeValue.fromN(v.asText());
            case "B" -> AttributeValue.fromB(SdkBytes.fromByteArray(Base64.getDecoder().decode(v.asText())));
            case "BOOL" -> AttributeValue.fromBool(v.asBoolean());
            case "NULL" -> AttributeValue.fromNul(true);
            case "SS" -> AttributeValue.fromSs(strings(v));
            case "NS" -> AttributeValue.fromNs(strings(v));
            case "BS" -> AttributeValue.fromBs(strings(v).stream()
                    .map(s -> SdkBytes.fromByteArray(Base64.getDecoder().decode(s)))
                    .toList());
            case "L" -> {
                List<AttributeValue> l = new ArrayList<>();
                v.forEach(x -> l.add(fromJson(x)));
                yield AttributeValue.fromL(l);
            }
            case "M" -> AttributeValue.fromM(fromJsonItem((ObjectNode) v));
            default -> throw new IllegalArgumentException("unknown DynamoDB type " + e.getKey());
        };
    }

    static Map<String, AttributeValue> fromJsonItem(ObjectNode item) {
        Map<String, AttributeValue> out = new LinkedHashMap<>();
        Iterator<Map.Entry<String, JsonNode>> it = item.fields();
        while (it.hasNext()) {
            Map.Entry<String, JsonNode> e = it.next();
            out.put(e.getKey(), fromJson(e.getValue()));
        }
        return out;
    }

    /** DynamoDB's equality: numbers compare by value, sets ignore order. */
    static boolean sameValue(AttributeValue a, AttributeValue b) {
        if (a.type() != b.type()) {
            return false;
        }
        return switch (a.type()) {
            case N -> new BigDecimal(a.n()).compareTo(new BigDecimal(b.n())) == 0;
            case SS -> Set.copyOf(a.ss()).equals(Set.copyOf(b.ss()));
            case NS -> numberSet(a.ns()).equals(numberSet(b.ns()));
            case BS -> Set.copyOf(a.bs()).equals(Set.copyOf(b.bs()));
            default -> Objects.equals(a, b);
        };
    }

    private static Set<BigDecimal> numberSet(List<String> ns) {
        Set<BigDecimal> out = new LinkedHashSet<>();
        ns.forEach(n -> out.add(new BigDecimal(n).stripTrailingZeros()));
        return out;
    }

    /**
     * Whether {@code item} (null: no item) meets a legacy {@code Expected} map: each attribute
     * equal to its value, or absent where {@code Exists} is false.
     */
    static boolean meets(Map<String, AttributeValue> item, Map<String, ExpectedAttributeValue> expected) {
        for (Map.Entry<String, ExpectedAttributeValue> e : expected.entrySet()) {
            ExpectedAttributeValue x = e.getValue();
            AttributeValue have = item == null ? null : item.get(e.getKey());
            if (x.comparisonOperator() != null && x.comparisonOperator() != ComparisonOperator.EQ) {
                throw new IllegalArgumentException("unsupported comparison " + x.comparisonOperator());
            }
            if (Boolean.FALSE.equals(x.exists())) {
                if (have != null) {
                    return false;
                }
                continue;
            }
            AttributeValue want = x.value() != null ? x.value()
                    : x.attributeValueList() != null && !x.attributeValueList().isEmpty()
                            ? x.attributeValueList().get(0) : null;
            if (want == null) {
                if (have == null) {
                    return false;
                }
                continue;
            }
            if (have == null || !sameValue(have, want)) {
                return false;
            }
        }
        return true;
    }

    /** Applies legacy {@code AttributeUpdates} to a copy of {@code item}, as DynamoDB does. */
    static Map<String, AttributeValue> apply(
            Map<String, AttributeValue> item, Map<String, AttributeValueUpdate> updates) {
        Map<String, AttributeValue> out = new LinkedHashMap<>(item);
        for (Map.Entry<String, AttributeValueUpdate> e : updates.entrySet()) {
            String name = e.getKey();
            AttributeValueUpdate u = e.getValue();
            AttributeAction action = u.action() == null ? AttributeAction.PUT : u.action();
            AttributeValue v = u.value();
            switch (action) {
                case PUT -> {
                    if (v == null) {
                        throw new IllegalArgumentException("PUT without a value for " + name);
                    }
                    out.put(name, v);
                }
                case DELETE -> {
                    AttributeValue have = out.get(name);
                    if (v == null || have == null) {
                        out.remove(name);
                    } else {
                        AttributeValue left = removeFromSet(have, v);
                        if (left == null) {
                            out.remove(name);
                        } else {
                            out.put(name, left);
                        }
                    }
                }
                case ADD -> {
                    if (v == null) {
                        throw new IllegalArgumentException("ADD without a value for " + name);
                    }
                    out.put(name, add(out.get(name), v));
                }
                default -> throw new IllegalArgumentException("unsupported action " + action);
            }
        }
        return out;
    }

    private static AttributeValue add(AttributeValue have, AttributeValue v) {
        switch (v.type()) {
            case N -> {
                BigDecimal base = have == null ? BigDecimal.ZERO : new BigDecimal(have.n());
                return AttributeValue.fromN(base.add(new BigDecimal(v.n())).toPlainString());
            }
            case SS -> {
                Set<String> s = new LinkedHashSet<>(have == null ? List.of() : have.ss());
                s.addAll(v.ss());
                return AttributeValue.fromSs(new ArrayList<>(s));
            }
            case NS -> {
                Set<String> s = new LinkedHashSet<>(have == null ? List.of() : have.ns());
                s.addAll(v.ns());
                return AttributeValue.fromNs(new ArrayList<>(s));
            }
            default -> throw new IllegalArgumentException("ADD of " + v.type());
        }
    }

    private static AttributeValue removeFromSet(AttributeValue have, AttributeValue v) {
        if (have.type() != v.type()) {
            throw new IllegalArgumentException("DELETE of " + v.type() + " from " + have.type());
        }
        switch (v.type()) {
            case SS -> {
                List<String> left = new ArrayList<>(have.ss());
                left.removeAll(v.ss());
                return left.isEmpty() ? null : AttributeValue.fromSs(left);
            }
            case NS -> {
                List<String> left = new ArrayList<>(have.ns());
                left.removeAll(v.ns());
                return left.isEmpty() ? null : AttributeValue.fromNs(left);
            }
            default -> throw new IllegalArgumentException("DELETE of a value from " + v.type());
        }
    }
}
