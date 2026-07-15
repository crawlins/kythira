#!/usr/bin/env bash
# Runs INSIDE the Packer builder instance (uploaded by the "file"
# provisioner, executed by the "shell" provisioner as root via sudo).
# Installs software only — no secrets, no per-node configuration.
# See .kiro/specs/ca-cluster-node-ami/requirements.md Requirement 5.
set -euo pipefail

echo "[provision] installing runtime packages"
export DEBIAN_FRONTEND=noninteractive
apt-get update -q
apt-get install -y --no-install-recommends libssl3 curl
rm -rf /var/lib/apt/lists/*

echo "[provision] installing ca_cluster_node binary"
install -m 0755 -o root -g root /tmp/ca_cluster_node /usr/local/bin/ca_cluster_node
rm -f /tmp/ca_cluster_node

echo "[provision] creating ca-cluster-node system user"
if ! id -u ca-cluster-node >/dev/null 2>&1; then
    useradd --system --no-create-home --shell /usr/sbin/nologin ca-cluster-node
fi

echo "[provision] creating data and config directories"
install -d -m 0750 -o ca-cluster-node -g ca-cluster-node /var/lib/ca_cluster_node
install -d -m 0750 -o ca-cluster-node -g ca-cluster-node /etc/ca_cluster_node
# Deliberately left empty: the CA passphrase file and the RPC-TLS first-boot
# credential pair are installed per-instance at launch time, never baked
# into the AMI (Requirement 5).

echo "[provision] installing systemd unit"
install -m 0644 -o root -g root /tmp/ca_cluster_node.service \
    /etc/systemd/system/ca_cluster_node.service
rm -f /tmp/ca_cluster_node.service
systemctl daemon-reload
systemctl enable ca_cluster_node
# Deliberately NOT started here — /etc/default/ca_cluster_node (the unit's
# EnvironmentFile) does not exist yet; starting now would just crash-loop.

echo "[provision] AMI hygiene"
# Standard golden-AMI cleanup so every instance launched from this AMI gets
# a fresh identity rather than inheriting the builder instance's.
rm -f /etc/machine-id
touch /etc/machine-id
rm -f /etc/ssh/ssh_host_*_key /etc/ssh/ssh_host_*_key.pub
if command -v cloud-init >/dev/null 2>&1; then
    cloud-init clean --logs || true
fi
rm -f /home/ubuntu/.bash_history /root/.bash_history

echo "[provision] done"
