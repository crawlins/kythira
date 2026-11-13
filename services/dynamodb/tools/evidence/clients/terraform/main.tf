# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# The Terraform conformance client (.kiro/specs/dynamodb-compatible-api/
# task 24): the AWS provider's aws_dynamodb_table and
# aws_dynamodb_table_item against Kythira. run_terraform.sh applies it,
# checks that a second plan finds nothing to change (every attribute the
# provider reads back matches what it wrote), changes the table in place,
# and destroys it.

terraform {
  required_version = ">= 1.9.0"
  required_providers {
    aws = {
      source  = "hashicorp/aws"
      version = "5.80.0"
    }
  }
}

variable "endpoint" {
  type = string
}

# The read capacity the table is applied with; run_terraform.sh raises it
# to check an in-place update.
variable "read_capacity" {
  type    = number
  default = 5
}

provider "aws" {
  region     = "us-east-1"
  access_key = "EVIDENCE-terraform"
  secret_key = "evidence-secret-not-a-credential"

  skip_credentials_validation = true
  skip_metadata_api_check     = true
  skip_requesting_account_id  = true
  skip_region_validation      = true

  endpoints {
    dynamodb = var.endpoint
  }
}

resource "aws_dynamodb_table" "orders" {
  name           = "evidence-terraform"
  billing_mode   = "PROVISIONED"
  read_capacity  = var.read_capacity
  write_capacity = 5
  hash_key       = "pk"
  range_key      = "sk"

  attribute {
    name = "pk"
    type = "S"
  }
  attribute {
    name = "sk"
    type = "N"
  }
  attribute {
    name = "status"
    type = "S"
  }

  global_secondary_index {
    name            = "by-status"
    hash_key        = "status"
    range_key       = "sk"
    projection_type = "KEYS_ONLY"
    read_capacity   = 5
    write_capacity  = 5
  }

  local_secondary_index {
    name            = "by-sk-local"
    range_key       = "status"
    projection_type = "ALL"
  }

  ttl {
    attribute_name = "expires"
    enabled        = true
  }

  stream_enabled   = true
  stream_view_type = "NEW_AND_OLD_IMAGES"

  deletion_protection_enabled = false

  tags = {
    team = "evidence"
  }
}

resource "aws_dynamodb_table_item" "first" {
  table_name = aws_dynamodb_table.orders.name
  hash_key   = aws_dynamodb_table.orders.hash_key
  range_key  = aws_dynamodb_table.orders.range_key
  item = jsonencode({
    pk     = { S = "order#1" }
    sk     = { N = "1" }
    status = { S = "open" }
  })
}
