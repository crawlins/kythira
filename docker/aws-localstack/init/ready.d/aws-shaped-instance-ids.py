# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# LocalStack ready hook: give EC2 instances the ID shape real AWS uses.
#
# aws_ec2_quorum_manager uses the instance ID itself as the node ID: the 17
# hex digits after "i-" parsed as a uint64. Real AWS IDs fit, because their
# first hex digit is always 0 (i-0 followed by 16 hex digits = 64 bits).
# LocalStack's EC2 (moto) draws all 17 digits at random, so 15 IDs in 16 have
# a non-zero first digit, overflow 64 bits, and every provision_node call
# fails in std::stoull. That is why tests/aws_quorum_manager_localstack_test.cpp
# could not pass a single EC2 case against LocalStack.
#
# Python files under /etc/localstack/init/ready.d run inside LocalStack's own
# process once it is ready, so this replaces moto's generator in place. Only
# the instance generator changes; every other resource keeps moto's IDs.

import random

import moto.ec2.models.instances as _instances

_HEX = "0123456789abcdef"


def _aws_shaped_instance_id() -> str:
    return "i-0" + "".join(random.choice(_HEX) for _ in range(16))


_instances.random_instance_id = _aws_shaped_instance_id
