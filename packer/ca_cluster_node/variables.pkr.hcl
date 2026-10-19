# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

variable "arch" {
  type        = string
  description = "Target architecture: \"amd64\" or \"arm64\"."

  validation {
    condition     = contains(["amd64", "arm64"], var.arch)
    error_message = "Arch must be \"amd64\" or \"arm64\"."
  }
}

variable "region" {
  type        = string
  default     = "us-east-1"
  description = "AWS region to build and register the AMI in."
}

variable "binary_path" {
  type        = string
  description = "Local path to the extract-binary.sh output for this arch."
}

variable "chaos_node_binary_path" {
  type        = string
  default     = ""
  description = "Local path to a chaos_node binary to install as well (build.sh --with-chaos-node), for a test AMI. Empty installs none."
}

variable "git_sha" {
  type        = string
  description = "Full commit SHA the binary was built from (used in tags and the AMI name)."
}

variable "builder_instance_type" {
  type        = string
  default     = ""
  description = "Override the Packer builder instance type. Empty string resolves to t3.micro (amd64) / t4g.micro (arm64)."
}

variable "ssh_username" {
  type        = string
  default     = "ubuntu"
  description = "SSH username Packer uses to provision the temporary builder instance."
}
