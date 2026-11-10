// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

package kythira.kcl.leases;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import com.fasterxml.jackson.databind.node.ObjectNode;
import java.io.IOException;
import java.net.URI;
import java.net.http.HttpClient;
import java.net.http.HttpRequest;
import java.net.http.HttpResponse;
import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.Optional;
import java.util.OptionalLong;
import java.util.Set;
import software.amazon.awssdk.auth.credentials.AwsCredentialsProvider;
import software.amazon.awssdk.http.ContentStreamProvider;
import software.amazon.awssdk.http.SdkHttpMethod;
import software.amazon.awssdk.http.SdkHttpRequest;
import software.amazon.awssdk.http.auth.aws.signer.AwsV4HttpSigner;
import software.amazon.awssdk.http.auth.spi.signer.SignedRequest;

/**
 * {@link LeaseApi} over HTTP to a Kythira Kinesis gateway, signed with SigV4 for the {@code
 * kinesis} service using the AWS SDK's signer and the application's credentials.
 *
 * <p>Any gateway of the cluster will do: it forwards to the catalog group's leader, so every read
 * and write is ordered by that group's log. Reads that fail without an answer are retried a few
 * times; writes are not, because a write whose answer was lost may have applied, and only the
 * refresher, which knows what it meant to write, can tell.
 */
public final class HttpLeaseApi implements LeaseApi {
    private static final ObjectMapper JSON = new ObjectMapper();
    // java.net.http refuses to set these; it writes them itself.
    private static final Set<String> RESTRICTED =
            Set.of("host", "content-length", "connection", "expect", "upgrade");

    private final URI endpoint;
    private final String application;
    private final AwsCredentialsProvider credentials;
    private final String region;
    private final HttpClient http;
    private final Duration timeout;
    private final AwsV4HttpSigner signer = AwsV4HttpSigner.create();

    /**
     * @param endpoint the gateway, such as {@code https://kinesis.example:4567}
     * @param application the KCL application name (on AWS, the lease table's name)
     * @param credentials the application's credentials
     * @param region the signing region the gateway expects
     */
    public HttpLeaseApi(
            URI endpoint, String application, AwsCredentialsProvider credentials, String region) {
        this(endpoint, application, credentials, region, HttpClient.newHttpClient(),
                Duration.ofSeconds(10));
    }

    public HttpLeaseApi(
            URI endpoint,
            String application,
            AwsCredentialsProvider credentials,
            String region,
            HttpClient http,
            Duration timeout) {
        String base = endpoint.toString();
        this.endpoint = URI.create(base.endsWith("/") ? base.substring(0, base.length() - 1) : base);
        this.application = application;
        this.credentials = credentials;
        this.region = region;
        this.http = http;
        this.timeout = timeout;
    }

    /** RFC 3986 percent-encoding of one path segment, as UTF-8. */
    static String encodeSegment(String s) {
        StringBuilder out = new StringBuilder();
        for (byte b : s.getBytes(StandardCharsets.UTF_8)) {
            int c = b & 0xff;
            boolean unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
                    || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~';
            if (unreserved) {
                out.append((char) c);
            } else {
                out.append('%').append(String.format(Locale.ROOT, "%02X", c));
            }
        }
        return out.toString();
    }

    private URI uri(String key, String query) {
        String path = "/v1/leases/" + application + "/" + (key == null ? "" : encodeSegment(key));
        return URI.create(endpoint + path + (query == null || query.isEmpty() ? "" : "?" + query));
    }

    private record Answer(int status, JsonNode body, Optional<String> etag) {}

    private Answer send(SdkHttpMethod method, URI uri, String body, String headerName,
            String headerValue) throws LeaseApiException {
        SdkHttpRequest.Builder b = SdkHttpRequest.builder().method(method).uri(uri);
        if (body != null) {
            b.putHeader("Content-Type", "application/json");
        }
        if (headerName != null) {
            b.putHeader(headerName, headerValue);
        }
        SdkHttpRequest unsigned = b.build();
        SignedRequest signed = signer.sign(r -> {
            r.identity(credentials.resolveCredentials())
                    .request(unsigned)
                    .putProperty(AwsV4HttpSigner.SERVICE_SIGNING_NAME, "kinesis")
                    .putProperty(AwsV4HttpSigner.REGION_NAME, region);
            if (body != null) {
                r.payload(ContentStreamProvider.fromUtf8String(body));
            }
        });
        HttpRequest.Builder req = HttpRequest.newBuilder(uri).timeout(timeout);
        signed.request().forEachHeader((name, values) -> {
            if (!RESTRICTED.contains(name.toLowerCase(Locale.ROOT))) {
                for (String v : values) {
                    req.header(name, v);
                }
            }
        });
        req.method(method.name(), body == null
                ? HttpRequest.BodyPublishers.noBody()
                : HttpRequest.BodyPublishers.ofString(body, StandardCharsets.UTF_8));
        try {
            HttpResponse<String> res = http.send(req.build(), HttpResponse.BodyHandlers.ofString());
            JsonNode parsed = res.body().isEmpty() ? JSON.createObjectNode() : JSON.readTree(res.body());
            return new Answer(res.statusCode(), parsed, res.headers().firstValue("ETag"));
        } catch (IOException e) {
            throw new LeaseApiException("lease API " + method + " " + uri.getPath() + " failed", e);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            throw new LeaseApiException("interrupted", e);
        }
    }

    private static LeaseApiException failure(Answer a) {
        String type = a.body().path("__type").asText("UnknownError");
        String message = a.body().path("message").asText("");
        return new LeaseApiException(a.status(), type, message);
    }

    /** Reads retry: they change nothing, so a lost answer costs only time. */
    private Answer read(URI uri) throws LeaseApiException {
        LeaseApiException last = null;
        for (int attempt = 0; attempt < 3; ++attempt) {
            try {
                Answer a = send(SdkHttpMethod.GET, uri, null, null, null);
                if (a.status() < 500 && a.status() != 429) {
                    return a;
                }
                last = failure(a);
            } catch (LeaseApiException e) {
                last = e;
            }
            try {
                Thread.sleep(100L << attempt);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
                throw new LeaseApiException("interrupted", e);
            }
        }
        throw last;
    }

    private static Versioned versioned(JsonNode n) {
        return new Versioned(n.path("key").asText(), n.path("version").asLong(),
                (ObjectNode) n.path("lease"));
    }

    @Override
    public Optional<Versioned> get(String key) throws LeaseApiException {
        Answer a = read(uri(key, null));
        if (a.status() == 404) {
            return Optional.empty();
        }
        if (a.status() != 200) {
            throw failure(a);
        }
        return Optional.of(versioned(a.body()));
    }

    @Override
    public Page list(String startAfter, int limit) throws LeaseApiException {
        String query = "limit=" + limit
                + (startAfter == null ? "" : "&start-after=" + encodeSegment(startAfter));
        Answer a = read(uri(null, query));
        if (a.status() != 200) {
            throw failure(a);
        }
        List<Versioned> out = new ArrayList<>();
        for (JsonNode n : a.body().path("leases")) {
            out.add(versioned(n));
        }
        JsonNode next = a.body().get("nextStartAfter");
        return new Page(out, next == null || next.isNull() ? null : next.asText());
    }

    private static String[] conditionHeader(Condition c) {
        if (c instanceof Condition.Absent) {
            return new String[] {"If-None-Match", "*"};
        }
        if (c instanceof Condition.AtVersion v) {
            return new String[] {"If-Match", "\"" + v.version() + "\""};
        }
        return new String[] {null, null};
    }

    @Override
    public OptionalLong put(String key, ObjectNode item, Condition condition)
            throws LeaseApiException {
        String[] h = conditionHeader(condition);
        Answer a;
        try {
            a = send(SdkHttpMethod.PUT, uri(key, null), JSON.writeValueAsString(item), h[0], h[1]);
        } catch (com.fasterxml.jackson.core.JsonProcessingException e) {
            throw new LeaseApiException("lease does not serialize", e);
        }
        if (a.status() == 412) {
            return OptionalLong.empty();
        }
        if (a.status() != 200) {
            throw failure(a);
        }
        return OptionalLong.of(a.body().path("version").asLong());
    }

    @Override
    public boolean delete(String key, Condition condition) throws LeaseApiException {
        String[] h = conditionHeader(condition);
        Answer a = send(SdkHttpMethod.DELETE, uri(key, null), null, h[0], h[1]);
        if (a.status() == 412 || a.status() == 404) {
            return false;
        }
        if (a.status() != 200) {
            throw failure(a);
        }
        return true;
    }
}
