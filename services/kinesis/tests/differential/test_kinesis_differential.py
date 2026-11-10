#!/usr/bin/env python3
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

"""Unit tests for kinesis_differential.py's normaliser and comparison.

These run with no network and no boto3: they check that what legitimately
differs between two runs (sequence digits, iterators, account, stream name)
compares equal, and that what must match (identity of sequence numbers,
error types, data) does not.
"""

import datetime
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import kinesis_differential as kd  # noqa: E402


class FakeError:
    def __init__(self, code, status, message):
        self.response = {
            "Error": {"Code": code, "Message": message},
            "ResponseMetadata": {"HTTPStatusCode": status},
        }


class NormaliserTest(unittest.TestCase):
    def test_sequence_numbers_become_tokens_in_order_of_first_appearance(self):
        n = kd.Normaliser()
        got = n.value({"Records": [
            {"SequenceNumber": "4960000000000000000000000002"},
            {"SequenceNumber": "4960000000000000000000000001"},
            {"SequenceNumber": "4960000000000000000000000002"},
        ]})
        self.assertEqual([r["SequenceNumber"] for r in got["Records"]],
                         ["SEQ#0", "SEQ#1", "SEQ#0"])
        self.assertEqual(n.raw["SEQ#1"], "4960000000000000000000000001")

    def test_two_runs_with_different_digits_but_the_same_identity_agree(self):
        a, b = kd.Normaliser(), kd.Normaliser()
        ra = {"StartingSequenceNumber": "1", "EndingSequenceNumber": "9"}
        rb = {"StartingSequenceNumber": "1000", "EndingSequenceNumber": "2000"}
        self.assertEqual(a.value(ra), b.value(rb))

    def test_reusing_a_number_where_aws_used_a_new_one_is_a_difference(self):
        a, b = kd.Normaliser(), kd.Normaliser()
        ra = {"StartingSequenceNumber": "1", "EndingSequenceNumber": "9"}
        rb = {"StartingSequenceNumber": "5", "EndingSequenceNumber": "5"}
        self.assertNotEqual(a.value(ra), b.value(rb))

    def test_placeholders_and_dropped_metadata(self):
        n = kd.Normaliser()
        got = n.value({
            "ShardIterator": "AAAA",
            "NextShardIterator": "BBBB",
            "NextToken": "tok",
            "MillisBehindLatest": 12,
            "StreamCreationTimestamp": datetime.datetime(2026, 11, 10),
            "ResponseMetadata": {"RequestId": "x"},
        })
        self.assertEqual(got, {
            "MillisBehindLatest": "<millis>",
            "NextShardIterator": "<iterator>",
            "NextToken": "<token>",
            "ShardIterator": "<iterator>",
            "StreamCreationTimestamp": "<time>",
        })

    def test_data_is_compared_byte_for_byte(self):
        n = kd.Normaliser()
        self.assertEqual(n.value({"Data": b"\x00hi"}), {"Data": "b64:AGhp"})
        self.assertNotEqual(n.value({"Data": b"hi"}), n.value({"Data": b"ho"}))

    def test_learned_names_are_replaced_longest_first(self):
        n = kd.Normaliser()
        n.learn("kythira-diff-1", "STREAM")
        n.learn("kythira-diff-1-abc", "STREAM")
        n.learn("123456789012", "ACCOUNT")
        n.learn("", "NOTHING")
        arn = "arn:aws:kinesis:us-east-1:123456789012:stream/kythira-diff-1-abc"
        self.assertEqual(n.text(arn), "arn:aws:kinesis:us-east-1:ACCOUNT:stream/STREAM")

    def test_the_clock_in_a_message_is_masked(self):
        n = kd.Normaliser()
        self.assertEqual(
            n.text("timestampInMillis: 4102444800000, currentTimestampInMillis: 1794323365879"),
            "timestampInMillis: 4102444800000, currentTimestampInMillis: <now>")

    def test_errors_keep_type_status_and_normalised_message(self):
        n = kd.Normaliser()
        n.learn("kythira-diff-9-ff", "STREAM")
        got = n.error(FakeError("ResourceNotFoundException", 400,
                                "Stream kythira-diff-9-ff not found"))
        self.assertEqual(got, {"error": "ResourceNotFoundException", "status": 400,
                               "message": "Stream STREAM not found"})


class RequestTest(unittest.TestCase):
    def test_symbolic_sequence_references_resolve_per_run(self):
        req = {"ShardId": "shardId-000000000000",
               "StartingSequenceNumber": {"$seq": "SEQ#3"}}
        self.assertEqual(kd._resolve(req, {"SEQ#3": "777"}),
                         {"ShardId": "shardId-000000000000",
                          "StartingSequenceNumber": "777"})
        self.assertEqual(kd._symbolic(req, kd.Normaliser()), req)

    def test_big_requests_are_summarised(self):
        n = kd.Normaliser()
        got = kd._symbolic({"Records": [{"Data": b"x" * 100}] * 60}, n)
        self.assertEqual(got, {"Records": {"$list": 60, "first": {"Data": {"$bytes": 100}}}})

    def test_snake_case_matches_boto3_method_names(self):
        self.assertEqual(kd._snake("PutRecords"), "put_records")
        self.assertEqual(kd._snake("DescribeStreamSummary"), "describe_stream_summary")


def step(op, request, response):
    return {"op": op, "request": request, "response": response}


class CompareTest(unittest.TestCase):
    def test_identical_runs_have_no_failures_or_notes(self):
        s = [step("PutRecord", {"PartitionKey": "k0"}, {"SequenceNumber": "SEQ#0"})]
        self.assertEqual(kd.compare(s, s), ([], []))

    def test_a_different_value_fails_with_its_path(self):
        a = [step("ListShards", {}, {"Shards": [{"ShardId": "shardId-000000000000"}]})]
        b = [step("ListShards", {}, {"Shards": [{"ShardId": "shardId-000000000001"}]})]
        failures, _ = kd.compare(a, b)
        self.assertEqual(len(failures), 1)
        self.assertIn("Shards[0].ShardId", failures[0])

    def test_missing_and_extra_fields_fail(self):
        a = [step("DescribeStreamSummary", {}, {"A": 1})]
        b = [step("DescribeStreamSummary", {}, {"B": 1})]
        failures, _ = kd.compare(a, b)
        self.assertEqual(len(failures), 2)

    def test_message_wording_is_a_note_unless_strict(self):
        a = [step("GetRecords", {}, {"error": "ExpiredIteratorException", "status": 400,
                                     "message": "Iterator expired"})]
        b = [step("GetRecords", {}, {"error": "ExpiredIteratorException", "status": 400,
                                     "message": "The iterator has expired"})]
        failures, notes = kd.compare(a, b)
        self.assertEqual((len(failures), len(notes)), (0, 1))
        failures, notes = kd.compare(a, b, strict_messages=True)
        self.assertEqual((len(failures), len(notes)), (1, 0))

    def test_a_different_error_type_or_status_fails(self):
        a = [step("SplitShard", {}, {"error": "LimitExceededException", "status": 400,
                                     "message": ""})]
        b = [step("SplitShard", {}, {"error": "InvalidArgumentException", "status": 400,
                                     "message": ""})]
        failures, _ = kd.compare(a, b)
        self.assertEqual(len(failures), 1)

    def test_allow_list_turns_a_failure_into_a_note(self):
        a = [step("ListShards", {}, {"X": 1})]
        b = [step("ListShards", {}, {"X": 2})]
        failures, notes = kd.compare(a, b, allow=[{"op": "ListShards", "path": "X"}])
        self.assertEqual(failures, [])
        self.assertEqual(len(notes), 1)
        self.assertTrue(notes[0].startswith("allowed "))

    def test_diverging_requests_stop_the_comparison(self):
        a = [step("PutRecord", {"PartitionKey": "k0"}, {}), step("ListShards", {}, {"X": 1})]
        b = [step("PutRecord", {"PartitionKey": "k1"}, {}), step("ListShards", {}, {"X": 2})]
        failures, _ = kd.compare(a, b)
        self.assertEqual(len(failures), 1)
        self.assertIn("requests diverge", failures[0])

    def test_different_step_counts_fail(self):
        s = step("ListShards", {}, {})
        failures, _ = kd.compare([s, s], [s])
        self.assertEqual(failures, ["step counts differ: recorded 2, got 1"])

    def test_list_length_differences_are_reported_once(self):
        self.assertEqual(kd.diff({"R": [1, 2]}, {"R": [1]}), [("R.length", 2, 1)])


class RetryTest(unittest.TestCase):
    def test_only_a_not_yet_active_status_is_stale(self):
        self.assertTrue(kd._stale_status({
            "Code": "ResourceInUseException",
            "Message": "Stream s under account 1 not ACTIVE or UPDATING, instead in state CREATING"}))
        self.assertTrue(kd._stale_status({
            "Code": "ResourceInUseException", "Message": "Stream s is in UPDATING state"}))
        self.assertFalse(kd._stale_status({
            "Code": "ResourceInUseException", "Message": "Stream s already exists"}))
        self.assertFalse(kd._stale_status({
            "Code": "LimitExceededException", "Message": "UPDATING"}))


class RecordingsTest(unittest.TestCase):
    def test_a_directory_yields_its_seed_recordings_in_order(self):
        with tempfile.TemporaryDirectory() as d:
            for f in ("aws-seed-2.json", "aws-seed-1.json", "allow.json", "notes.md"):
                open(os.path.join(d, f), "w").close()
            self.assertEqual([os.path.basename(p) for p in kd.recordings(d)],
                             ["aws-seed-1.json", "aws-seed-2.json"])
            self.assertEqual(kd.recordings(os.path.join(d, "x.json")),
                             [os.path.join(d, "x.json")])

    def test_an_empty_directory_is_an_error(self):
        with tempfile.TemporaryDirectory() as d:
            with self.assertRaises(SystemExit):
                kd.recordings(d)


if __name__ == "__main__":
    unittest.main()
