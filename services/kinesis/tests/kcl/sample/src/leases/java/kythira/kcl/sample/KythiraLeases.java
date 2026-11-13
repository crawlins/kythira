// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

package kythira.kcl.sample;

import java.net.URI;
import kythira.kcl.leases.HttpLeaseApi;
import kythira.kcl.leases.KythiraLeaseManagementFactory;
import kythira.kcl.leases.LeaseApi;
import software.amazon.awssdk.auth.credentials.DefaultCredentialsProvider;
import software.amazon.awssdk.regions.Region;
import software.amazon.kinesis.leases.LeaseManagementConfig;

/**
 * Puts a worker's leases on the Kythira Kinesis lease API (task 9) instead of a DynamoDB table.
 *
 * <p>The one place the sample uses the lease layer, so the KCL 3.x build of the same consumer
 * ({@code ../sample3}) can replace this class: the lease layer implements KCL 2.x's interfaces.
 */
final class KythiraLeases {
    private KythiraLeases() {}

    static void install(LeaseManagementConfig leases, URI endpoint, String application, Region region) {
        LeaseApi api = new HttpLeaseApi(endpoint, application,
                DefaultCredentialsProvider.builder().build(), region.id());
        leases.leaseManagementFactory(KythiraLeaseManagementFactory.create(leases, api, false));
    }
}
