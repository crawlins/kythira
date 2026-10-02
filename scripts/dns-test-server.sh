#!/usr/bin/env bash
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# Start or stop a throwaway BIND 9 server for the DNS discovery integration
# tests (tests/dns_discovery_bind_integration_test.cpp).
#
# Usage:
#   dns-test-server.sh start <state-dir> <port> [named-executable]
#   dns-test-server.sh stop  <state-dir>
#
# `start` writes a named.conf into <state-dir>, listens on 127.0.0.1:<port>
# only, and serves two dynamic zones, example.test. and
# cluster.example.test., that accept RFC 2136 UPDATEs signed with a freshly
# generated TSIG key and nothing else. The key's name and base64 secret are
# written to <state-dir>/tsig.name and <state-dir>/tsig.secret for the test
# to read; nothing about it is fixed in the source tree. `stop` kills the
# server recorded in <state-dir>/named.pid.
#
# Registered by tests/CMakeLists.txt as a CTest fixture, so `ctest` starts the
# server before the integration test and stops it afterwards. Nothing here
# needs root: the port is unprivileged and named runs as the invoking user.
#
# Ubuntu's bind9 package ships an AppArmor profile for /usr/sbin/named that
# only allows /etc/bind, /var/cache/bind and similar paths, so named started
# from here cannot read a config in a build directory and exits at once. CI
# unloads that profile before running the tests (see the dns-discovery-build
# job in .github/workflows/ci.yml). Locally, run
#   sudo apparmor_parser -R /etc/apparmor.d/usr.sbin.named
# once, or set the profile to complain mode.

set -euo pipefail

usage() {
    echo "usage: $0 start <state-dir> <port> [named-executable]" >&2
    echo "       $0 stop  <state-dir>" >&2
    exit 2
}

[[ $# -ge 2 ]] || usage
action=$1
state_dir=$2

stop_server() {
    local pid_file="$state_dir/named.pid"
    [[ -f $pid_file ]] || return 0
    local pid
    pid=$(cat "$pid_file")
    if kill "$pid" 2>/dev/null; then
        for _ in $(seq 1 50); do
            kill -0 "$pid" 2>/dev/null || break
            sleep 0.1
        done
        kill -9 "$pid" 2>/dev/null || true
    fi
    rm -f "$pid_file"
}

case $action in
start)
    [[ $# -ge 3 ]] || usage
    port=$3
    named=${4:-named}
    tsig_keygen=$(dirname "$(command -v "$named")")/tsig-keygen
    [[ -x $tsig_keygen ]] || tsig_keygen=tsig-keygen

    # A leftover server from an interrupted run would hold the port.
    stop_server
    rm -rf "$state_dir"
    mkdir -p "$state_dir"

    key_name=kythira-test.
    "$tsig_keygen" -a hmac-sha256 "$key_name" > "$state_dir/tsig.key"
    secret=$(sed -n 's/.*secret "\(.*\)";.*/\1/p' "$state_dir/tsig.key")
    [[ -n $secret ]] || { echo "could not read the secret tsig-keygen wrote" >&2; exit 1; }
    printf '%s' "$key_name" > "$state_dir/tsig.name"
    printf '%s' "$secret" > "$state_dir/tsig.secret"

    for zone in example.test cluster.example.test; do
        cat > "$state_dir/$zone.zone" <<EOF
\$TTL 30
@   IN SOA ns.$zone. hostmaster.$zone. 1 3600 600 86400 30
    IN NS  ns.$zone.
ns  IN A   127.0.0.1
EOF
    done

    cat > "$state_dir/named.conf" <<EOF
include "$state_dir/tsig.key";

options {
    directory "$state_dir";
    pid-file "$state_dir/named.pid";
    session-keyfile "$state_dir/session.key";
    managed-keys-directory "$state_dir";
    listen-on port $port { 127.0.0.1; };
    listen-on-v6 { none; };
    recursion no;
    dnssec-validation no;
    notify no;
};

logging {
    channel to_file { file "$state_dir/named.log"; severity info; print-time yes; };
    category default { to_file; };
    category update { to_file; };
    category update-security { to_file; };
};

zone "example.test." {
    type primary;
    file "$state_dir/example.test.zone";
    allow-update { key "$key_name"; };
};

zone "cluster.example.test." {
    type primary;
    file "$state_dir/cluster.example.test.zone";
    allow-update { key "$key_name"; };
};
EOF

    # named daemonises itself and writes the pid file once it is serving.
    if ! "$named" -c "$state_dir/named.conf" -4 > "$state_dir/named.stderr" 2>&1; then
        cat "$state_dir/named.stderr" >&2
        echo "named failed to start; on Ubuntu see this script's header about AppArmor" >&2
        exit 1
    fi
    for _ in $(seq 1 100); do
        if [[ -s $state_dir/named.pid ]] && grep -q "running" "$state_dir/named.log" 2>/dev/null; then
            echo "named listening on 127.0.0.1:$port (state in $state_dir)"
            exit 0
        fi
        sleep 0.1
    done
    cat "$state_dir/named.stderr" "$state_dir/named.log" >&2 2>/dev/null || true
    echo "named did not report running within 10s; on Ubuntu see this script's header about AppArmor" >&2
    stop_server
    exit 1
    ;;
stop)
    stop_server
    ;;
*)
    usage
    ;;
esac
