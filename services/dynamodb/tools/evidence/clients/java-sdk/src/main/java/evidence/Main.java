// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

package evidence;

import java.net.URI;
import java.util.List;
import java.util.Map;
import software.amazon.awssdk.auth.credentials.AwsBasicCredentials;
import software.amazon.awssdk.auth.credentials.StaticCredentialsProvider;
import software.amazon.awssdk.core.SdkBytes;
import software.amazon.awssdk.enhanced.dynamodb.DynamoDbEnhancedClient;
import software.amazon.awssdk.enhanced.dynamodb.DynamoDbTable;
import software.amazon.awssdk.enhanced.dynamodb.Expression;
import software.amazon.awssdk.enhanced.dynamodb.Key;
import software.amazon.awssdk.enhanced.dynamodb.TableSchema;
import software.amazon.awssdk.enhanced.dynamodb.mapper.annotations.DynamoDbBean;
import software.amazon.awssdk.enhanced.dynamodb.mapper.annotations.DynamoDbPartitionKey;
import software.amazon.awssdk.enhanced.dynamodb.mapper.annotations.DynamoDbSortKey;
import software.amazon.awssdk.enhanced.dynamodb.model.QueryConditional;
import software.amazon.awssdk.http.urlconnection.UrlConnectionHttpClient;
import software.amazon.awssdk.regions.Region;
import software.amazon.awssdk.services.dynamodb.DynamoDbClient;
import software.amazon.awssdk.services.dynamodb.model.*;

/** The aws-sdk-java-v2 evidence flow (task 1); see pom.xml. */
public final class Main {
    private static final String TABLE = "evidence-java";

    private static AttributeValue s(String v) {
        return AttributeValue.builder().s(v).build();
    }

    private static AttributeValue n(String v) {
        return AttributeValue.builder().n(v).build();
    }

    /** The enhanced client's bean. */
    @DynamoDbBean
    public static final class Order {
        private String customer;
        private Integer number;
        private String status;

        @DynamoDbPartitionKey
        public String getCustomer() { return customer; }
        public void setCustomer(String v) { customer = v; }

        @DynamoDbSortKey
        public Integer getNumber() { return number; }
        public void setNumber(Integer v) { number = v; }

        public String getStatus() { return status; }
        public void setStatus(String v) { status = v; }
    }

    public static void main(String[] args) {
        String endpoint = System.getenv().getOrDefault("ENDPOINT", "http://localhost:8000");
        DynamoDbClient ddb = DynamoDbClient.builder()
                .endpointOverride(URI.create(endpoint))
                .region(Region.US_EAST_1)
                .httpClient(UrlConnectionHttpClient.create())
                .credentialsProvider(StaticCredentialsProvider.create(AwsBasicCredentials.create(
                        "EVIDENCE-java-sdk", "evidence-secret-not-a-credential")))
                .build();

        ddb.createTable(r -> r.tableName(TABLE)
                .attributeDefinitions(
                        AttributeDefinition.builder().attributeName("pk").attributeType(ScalarAttributeType.S).build(),
                        AttributeDefinition.builder().attributeName("sk").attributeType(ScalarAttributeType.N).build(),
                        AttributeDefinition.builder().attributeName("gsk").attributeType(ScalarAttributeType.S).build())
                .keySchema(
                        KeySchemaElement.builder().attributeName("pk").keyType(KeyType.HASH).build(),
                        KeySchemaElement.builder().attributeName("sk").keyType(KeyType.RANGE).build())
                .globalSecondaryIndexes(GlobalSecondaryIndex.builder()
                        .indexName("by-gsk")
                        .keySchema(KeySchemaElement.builder().attributeName("gsk").keyType(KeyType.HASH).build())
                        .projection(Projection.builder().projectionType(ProjectionType.KEYS_ONLY).build())
                        .build())
                .billingMode(BillingMode.PAY_PER_REQUEST)
                .streamSpecification(StreamSpecification.builder()
                        .streamEnabled(true).streamViewType(StreamViewType.NEW_AND_OLD_IMAGES).build())
                .deletionProtectionEnabled(false));
        ddb.waiter().waitUntilTableExists(r -> r.tableName(TABLE));

        ddb.putItem(r -> r.tableName(TABLE)
                .item(Map.of("pk", s("p1"), "sk", n("1"), "gsk", s("g"),
                        "bin", AttributeValue.builder().b(SdkBytes.fromUtf8String("x")).build(),
                        "nums", AttributeValue.builder().ns("1", "2").build()))
                .conditionExpression("attribute_not_exists(pk)")
                .returnConsumedCapacity(ReturnConsumedCapacity.INDEXES)
                .returnItemCollectionMetrics(ReturnItemCollectionMetrics.SIZE));
        ddb.getItem(r -> r.tableName(TABLE).key(Map.of("pk", s("p1"), "sk", n("1"))).consistentRead(true));
        ddb.updateItem(r -> r.tableName(TABLE).key(Map.of("pk", s("p1"), "sk", n("1")))
                .updateExpression("SET #c = :c ADD nums :more")
                .expressionAttributeNames(Map.of("#c", "count"))
                .expressionAttributeValues(Map.of(":c", n("3"),
                        ":more", AttributeValue.builder().ns("3").build()))
                .returnValues(ReturnValue.ALL_NEW));
        ddb.query(r -> r.tableName(TABLE).indexName("by-gsk")
                .keyConditionExpression("gsk = :g")
                .expressionAttributeValues(Map.of(":g", s("g"))));
        for (var page : ddb.scanPaginator(r -> r.tableName(TABLE).limit(1))) {
            page.count();
        }
        ddb.batchWriteItem(r -> r.requestItems(Map.of(TABLE, List.of(
                WriteRequest.builder().putRequest(p -> p.item(Map.of("pk", s("b"), "sk", n("1")))).build(),
                WriteRequest.builder().deleteRequest(d -> d.key(Map.of("pk", s("b"), "sk", n("2")))).build()))));
        ddb.transactWriteItems(r -> r.transactItems(
                TransactWriteItem.builder().put(p -> p.tableName(TABLE)
                        .item(Map.of("pk", s("t"), "sk", n("1")))
                        .conditionExpression("attribute_not_exists(pk)")).build(),
                TransactWriteItem.builder().conditionCheck(c -> c.tableName(TABLE)
                        .key(Map.of("pk", s("p1"), "sk", n("1")))
                        .conditionExpression("attribute_exists(pk)")).build()));

        // Enhanced client: bean mapping, its expressions and paginated query.
        DynamoDbEnhancedClient enhanced = DynamoDbEnhancedClient.builder().dynamoDbClient(ddb).build();
        DynamoDbTable<Order> orders = enhanced.table(TABLE, TableSchema.fromBean(Order.class));
        Order o = new Order();
        o.setCustomer("c1");
        o.setNumber(7);
        o.setStatus("NEW");
        orders.putItem(o);
        orders.getItem(Key.builder().partitionValue("c1").sortValue(7).build());
        o.setStatus("SHIPPED");
        orders.updateItem(r -> r.item(o).conditionExpression(Expression.builder()
                .expression("#s = :old").putExpressionName("#s", "status")
                .putExpressionValue(":old", s("NEW")).build()));
        orders.query(QueryConditional.keyEqualTo(k -> k.partitionValue("c1"))).items().forEach(x -> { });
        orders.deleteItem(Key.builder().partitionValue("c1").sortValue(7).build());

        ddb.deleteTable(r -> r.tableName(TABLE));
        System.out.println("java sdk evidence flow done");
    }
}
