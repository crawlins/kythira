packer {
  required_plugins {
    amazon = {
      version = ">= 1.3.0"
      source  = "github.com/hashicorp/amazon"
    }
  }
}

locals {
  timestamp = formatdate("YYYYMMDD-hhmmss", timestamp())
  short_sha = substr(var.git_sha, 0, 7)
  ami_name  = "kythira-ca-cluster-node-${local.short_sha}-${var.arch}-${local.timestamp}"

  instance_type = var.builder_instance_type != "" ? var.builder_instance_type : (
    var.arch == "arm64" ? "t4g.micro" : "t3.micro"
  )

  common_tags = {
    "kythira:component" = "ca-cluster-node"
    "kythira:git-sha"   = var.git_sha
    "kythira:arch"      = var.arch
    "kythira:built-by"  = "packer"
  }
}

# Requirement 3.2 — Canonical's published SSM parameter, never a literal AMI
# ID, so this always resolves to Canonical's current Ubuntu 24.04 image.
data "amazon-parameterstore" "ubuntu" {
  name   = "/aws/service/canonical/ubuntu/server/24.04/stable/current/${var.arch}/hvm/ebs-gp3/ami-id"
  region = var.region
}

source "amazon-ebs" "ca_cluster_node" {
  region        = var.region
  source_ami    = data.amazon-parameterstore.ubuntu.value
  instance_type = local.instance_type
  ssh_username  = var.ssh_username
  ami_name      = local.ami_name

  ami_description = "kythira ca_cluster_node - commit ${var.git_sha}, ${var.arch}"

  tags = merge(local.common_tags, {
    "Name"             = local.ami_name
    "kythira:base-ami" = data.amazon-parameterstore.ubuntu.value
  })
  run_tags = merge(local.common_tags, {
    "Name" = "kythira-ca-cluster-node-ami-build-${var.arch}"
  })
  snapshot_tags = local.common_tags

  # 8 GiB matches Ubuntu 24.04's own default root volume size — the binary
  # and its runtime libs need only a few MB more, so the default is not
  # shrunk or grown.
  launch_block_device_mappings {
    device_name           = "/dev/sda1"
    volume_size           = 8
    volume_type           = "gp3"
    delete_on_termination = true
  }
}

build {
  sources = ["source.amazon-ebs.ca_cluster_node"]

  # Requirement 5 — exactly three inputs cross into the builder instance:
  # the compiled binary, the unmodified systemd unit, and the provisioning
  # script itself. None of the three references or contains a secret.
  provisioner "file" {
    source      = var.binary_path
    destination = "/tmp/ca_cluster_node"
  }

  provisioner "file" {
    source      = "${path.root}/../../docker/ca_cluster_node/ca_cluster_node.service"
    destination = "/tmp/ca_cluster_node.service"
  }

  provisioner "file" {
    source      = "${path.root}/scripts/provision.sh"
    destination = "/tmp/provision.sh"
  }

  provisioner "shell" {
    inline = [
      "chmod +x /tmp/provision.sh",
      "sudo /tmp/provision.sh",
    ]
  }

  post-processor "manifest" {
    output     = "packer-manifest.json"
    strip_path = true
    custom_data = {
      arch    = var.arch
      git_sha = var.git_sha
    }
  }
}
