variable "arch" {
  type        = string
  description = "Target architecture: \"amd64\" or \"arm64\"."

  validation {
    condition     = contains(["amd64", "arm64"], var.arch)
    error_message = "arch must be \"amd64\" or \"arm64\"."
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
