// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

package kythira.flink;

import java.util.Properties;
import org.apache.flink.api.common.eventtime.WatermarkStrategy;
import org.apache.flink.api.common.serialization.SimpleStringSchema;
import org.apache.flink.configuration.Configuration;
import org.apache.flink.connector.aws.config.AWSConfigConstants;
import org.apache.flink.connector.kinesis.sink.KinesisStreamsSink;
import org.apache.flink.connector.kinesis.source.KinesisStreamsSource;
import org.apache.flink.connector.kinesis.source.config.KinesisSourceConfigOptions;
import org.apache.flink.streaming.api.environment.StreamExecutionEnvironment;

/**
 * Copies every record of one Kinesis stream to another through Flink.
 *
 * <p>{@code FlinkPipe <endpoint> <account> <in-stream> <out-stream>}, with credentials from the
 * environment and the region from {@code AWS_REGION} (us-east-1 by default). The connector's
 * source names its stream by ARN, so the account the ARN carries is an argument. The source starts
 * at the trim horizon and polls; the sink sends PutRecords batches, keyed by each record's own
 * text. HTTP/1.1, as the scenario's nodes serve plaintext. Runs until killed.
 */
public final class FlinkPipe {
    private FlinkPipe() {}

    public static void main(String[] args) throws Exception {
        if (args.length < 4) {
            System.err.println("usage: FlinkPipe <endpoint> <account> <in-stream> <out-stream>");
            System.exit(2);
        }
        String endpoint = args[0];
        String account = args[1];
        String in = args[2];
        String out = args[3];
        String region = System.getenv().getOrDefault("AWS_REGION", "us-east-1");

        Configuration source = new Configuration();
        source.setString(AWSConfigConstants.AWS_REGION, region);
        source.setString(AWSConfigConstants.AWS_ENDPOINT, endpoint);
        source.setString(AWSConfigConstants.AWS_CREDENTIALS_PROVIDER, "ENV_VAR");
        source.setString(AWSConfigConstants.HTTP_PROTOCOL_VERSION, "HTTP1_1");
        source.set(KinesisSourceConfigOptions.STREAM_INITIAL_POSITION,
                KinesisSourceConfigOptions.InitialPosition.TRIM_HORIZON);

        Properties sink = new Properties();
        sink.setProperty(AWSConfigConstants.AWS_REGION, region);
        sink.setProperty(AWSConfigConstants.AWS_ENDPOINT, endpoint);
        sink.setProperty(AWSConfigConstants.AWS_CREDENTIALS_PROVIDER, "ENV_VAR");
        sink.setProperty(AWSConfigConstants.HTTP_PROTOCOL_VERSION, "HTTP1_1");

        String arn = "arn:aws:kinesis:" + region + ":" + account + ":stream/" + in;
        KinesisStreamsSource<String> kinesisIn = KinesisStreamsSource.<String>builder()
                .setStreamArn(arn)
                .setSourceConfig(source)
                .setDeserializationSchema(new SimpleStringSchema())
                .build();
        KinesisStreamsSink<String> kinesisOut = KinesisStreamsSink.<String>builder()
                .setKinesisClientProperties(sink)
                .setSerializationSchema(new SimpleStringSchema())
                .setPartitionKeyGenerator(record -> record)
                .setStreamName(out)
                .setMaxTimeInBufferMS(500)
                .build();

        StreamExecutionEnvironment env = StreamExecutionEnvironment.getExecutionEnvironment();
        env.setParallelism(2);
        env.fromSource(kinesisIn, WatermarkStrategy.noWatermarks(), "kinesis-in")
                .returns(String.class)
                .sinkTo(kinesisOut)
                .name("kinesis-out");
        System.out.println("FLINK starting " + in + " -> " + out);
        System.out.flush();
        env.execute("kythira-flink-pipe");
    }
}
