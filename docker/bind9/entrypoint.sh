#!/bin/sh
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# Generates a TSIG key (once per /tsig volume), makes it the only credential
# named.conf.local accepts for dynamic updates, publishes it to /tsig for
# the discovery nodes, then runs named in the foreground.
#
# /tsig is a compose volume the nodes mount read-only. The nodes depend on
# this container being healthy, and the healthcheck only passes once named
# is serving, so the key is always written before any node reads it. The
# key is generated at first start, so nothing about it is fixed in the image
# or the tree, as in scripts/dns-test-server.sh for the non-container test.
# The key name is fixed because named.conf.local names it in allow-update.

set -eu

key_name=kythira-update.
key_dir=/tsig

if [ -s "$key_dir/secret" ]; then
    # A restart of this container keeps the key the running nodes already
    # read; a new one would make every later update (dns_sd's freshness
    # refreshes, deregistration) fail with BADKEY.
    secret=$(cat "$key_dir/secret")
else
    secret=$(tsig-keygen -a hmac-sha256 "$key_name" | sed -n 's/.*secret "\(.*\)";.*/\1/p')
    if [ -z "$secret" ]; then
        echo "bind9 entrypoint: could not read the secret tsig-keygen wrote" >&2
        exit 1
    fi
    mkdir -p "$key_dir"
    (
        umask 077
        printf '%s' "$secret" > "$key_dir/secret"
    )
fi
printf '%s' "$key_name" > "$key_dir/name"

cat > /etc/bind/tsig.key <<KEY
key "$key_name" {
    algorithm hmac-sha256;
    secret "$secret";
};
KEY
chown root:bind /etc/bind/tsig.key
chmod 0640 /etc/bind/tsig.key

named-checkconf /etc/bind/named.conf
exec /usr/sbin/named -f -u bind -c /etc/bind/named.conf
