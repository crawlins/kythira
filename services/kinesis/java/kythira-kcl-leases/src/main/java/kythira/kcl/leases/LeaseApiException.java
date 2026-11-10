// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

package kythira.kcl.leases;

/** A Lease API call that did not get an answer the caller can act on. */
public class LeaseApiException extends Exception {
    private static final long serialVersionUID = 1L;

    private final int status;
    private final String type;

    public LeaseApiException(int status, String type, String message) {
        super(status + " " + type + ": " + message);
        this.status = status;
        this.type = type;
    }

    public LeaseApiException(String message, Throwable cause) {
        super(message, cause);
        this.status = 0;
        this.type = "ClientError";
    }

    /** The HTTP status, or 0 when no answer arrived. */
    public int status() {
        return status;
    }

    /** The error's {@code __type}, such as {@code AccessDeniedException}. */
    public String type() {
        return type;
    }

    /** Worth retrying: no answer, a throttle, or the server's own failure. */
    public boolean retryable() {
        return status == 0 || status == 429 || status >= 500;
    }
}
