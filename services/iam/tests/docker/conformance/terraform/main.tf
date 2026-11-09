# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# The Terraform conformance configuration (.kiro/specs/iam-compatible-auth/
# task 24, Requirement 15.2): users, groups, roles, policies, attachments,
# permissions boundaries and an OIDC provider, applied and then destroyed
# against the conformance stack by iam_conformance_test, with the AWS
# provider pinned. The run fails on any error, and the harness also fails it
# on any NotImplemented the provider meets on the way.
#
# Credentials are the account root's (AWS_ACCESS_KEY_ID and
# AWS_SECRET_ACCESS_KEY from the harness); IAM and STS both go to the leader
# named by var.endpoint, trusted through AWS_CA_BUNDLE.

terraform {
  required_version = "= 1.9.8"
  required_providers {
    aws = {
      source  = "hashicorp/aws"
      version = "= 5.70.0"
    }
  }
}

variable "endpoint" {
  type        = string
  description = "The IAM leader's https URL, for IAM and STS"
}

provider "aws" {
  region                      = "us-east-1"
  skip_metadata_api_check     = true
  skip_region_validation      = true
  skip_credentials_validation = false
  skip_requesting_account_id  = false
  endpoints {
    iam = var.endpoint
    sts = var.endpoint
  }
}

data "aws_caller_identity" "me" {}

locals {
  account = data.aws_caller_identity.me.account_id
}

# A permissions boundary and two customer managed policies, one with a
# second version.
resource "aws_iam_policy" "boundary" {
  name        = "tf-boundary"
  path        = "/tf/"
  description = "the most any tf- principal may do"
  policy = jsonencode({
    Version   = "2012-10-17"
    Statement = [{ Effect = "Allow", Action = ["s3:*", "iam:Get*", "iam:List*", "sts:*"], Resource = "*" }]
  })
  tags = { suite = "conformance" }
}

resource "aws_iam_policy" "read" {
  name = "tf-read"
  policy = jsonencode({
    Version   = "2012-10-17"
    Statement = [{ Sid = "Read", Effect = "Allow", Action = ["s3:GetObject", "s3:ListBucket"], Resource = "*" }]
  })
}

# Users: one with a boundary, tags and an inline policy, one in a group.
resource "aws_iam_user" "alice" {
  name                 = "tf-alice"
  path                 = "/tf/"
  permissions_boundary = aws_iam_policy.boundary.arn
  tags                 = { team = "storage" }
}

resource "aws_iam_user" "bob" {
  name = "tf-bob"
}

resource "aws_iam_user_policy" "alice_inline" {
  name = "tf-alice-inline"
  user = aws_iam_user.alice.name
  policy = jsonencode({
    Version   = "2012-10-17"
    Statement = [{ Effect = "Allow", Action = "s3:PutObject", Resource = "arn:aws:s3:::tf-bucket/*" }]
  })
}

resource "aws_iam_user_policy_attachment" "alice_read" {
  user       = aws_iam_user.alice.name
  policy_arn = aws_iam_policy.read.arn
}

# A group with a member, an inline policy and an attachment.
resource "aws_iam_group" "readers" {
  name = "tf-readers"
  path = "/tf/"
}

resource "aws_iam_group_membership" "readers" {
  name  = "tf-readers-membership"
  group = aws_iam_group.readers.name
  users = [aws_iam_user.bob.name]
}

resource "aws_iam_group_policy" "readers_inline" {
  name  = "tf-readers-inline"
  group = aws_iam_group.readers.name
  policy = jsonencode({
    Version   = "2012-10-17"
    Statement = [{ Effect = "Allow", Action = "s3:ListAllMyBuckets", Resource = "*" }]
  })
}

resource "aws_iam_group_policy_attachment" "readers_read" {
  group      = aws_iam_group.readers.name
  policy_arn = aws_iam_policy.read.arn
}

# An OIDC provider and a role it may assume, with a boundary, an inline
# policy, a customer and an AWS managed attachment, and an instance profile.
resource "aws_iam_openid_connect_provider" "issuer" {
  url             = "https://oidc-issuer/tf"
  client_id_list  = ["sts.amazonaws.com"]
  thumbprint_list = ["0000000000000000000000000000000000000000"]
  tags            = { suite = "conformance" }
}

resource "aws_iam_role" "web" {
  name                 = "tf-web"
  path                 = "/tf/"
  max_session_duration = 7200
  permissions_boundary = aws_iam_policy.boundary.arn
  assume_role_policy = jsonencode({
    Version = "2012-10-17"
    Statement = [{
      Effect    = "Allow"
      Principal = { Federated = aws_iam_openid_connect_provider.issuer.arn }
      Action    = "sts:AssumeRoleWithWebIdentity"
      Condition = { StringEquals = { "oidc-issuer/tf:aud" = "sts.amazonaws.com" } }
    }]
  })
  tags = { suite = "conformance" }
}

resource "aws_iam_role" "ec2" {
  name = "tf-ec2"
  assume_role_policy = jsonencode({
    Version   = "2012-10-17"
    Statement = [{ Effect = "Allow", Principal = { Service = "ec2.amazonaws.com" }, Action = "sts:AssumeRole" }]
  })
}

resource "aws_iam_role_policy" "web_inline" {
  name = "tf-web-inline"
  role = aws_iam_role.web.id
  policy = jsonencode({
    Version   = "2012-10-17"
    Statement = [{ Effect = "Allow", Action = "s3:GetObject", Resource = "arn:aws:s3:::tf-bucket/*" }]
  })
}

resource "aws_iam_role_policy_attachment" "web_read" {
  role       = aws_iam_role.web.name
  policy_arn = aws_iam_policy.read.arn
}

resource "aws_iam_role_policy_attachment" "ec2_managed" {
  role       = aws_iam_role.ec2.name
  policy_arn = "arn:aws:iam::aws:policy/ReadOnlyAccess"
}

resource "aws_iam_instance_profile" "ec2" {
  name = "tf-ec2"
  role = aws_iam_role.ec2.name
}

output "account" {
  value = local.account
}

output "role_arn" {
  value = aws_iam_role.web.arn
}
