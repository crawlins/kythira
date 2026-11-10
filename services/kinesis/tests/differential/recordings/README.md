# AWS Kinesis recordings

Each `aws-seed-N.json` is one run of `../kinesis_differential.py run --real-aws`
against real AWS Kinesis in us-east-1: the seeded operation mix and AWS's
normalised answers. The container scenario case
`differential_replay_matches_the_aws_recordings` replays each one against the
cluster and fails on any difference that `allow.json` does not list.

A recording costs real money, so a new one needs Clark's go each time. To
record:

```
AWS_PROFILE=<profile> python3 kinesis_differential.py run --real-aws \
    --region us-east-1 --seed N --steps 150 --out recordings/aws-seed-N.json
```

`allow.json` lists `{op, path, why}` entries for known, accepted differences.
Error messages are not compared unless `--strict-messages` is given; they
are printed as notes.
