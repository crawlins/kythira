// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

package kythira.kcl.sample;

import java.net.URI;
import software.amazon.awssdk.regions.Region;
import software.amazon.kinesis.leases.LeaseManagementConfig;

/**
 * KCL 3.x has no Kythira lease layer: the layer implements KCL 2.x's lease interfaces, and KCL 3.x
 * keeps its leases, worker metrics and coordinator state in DynamoDB tables. Run this build with
 * {@code KCL_LEASES=dynamodb}.
 */
final class KythiraLeases {
    private KythiraLeases() {}

    static void install(LeaseManagementConfig leases, URI endpoint, String application, Region region) {
        throw new IllegalStateException(
                "the KCL 3.x sample has no Kythira lease layer; set KCL_LEASES=dynamodb");
    }
}
