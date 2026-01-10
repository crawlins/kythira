#define BOOST_TEST_MODULE ConceptTest
#include <boost/test/included/unit_test.hpp>

#include <network_simulator/network_simulator.hpp>
#include <string>
#include <netinet/in.h>

using namespace network_simulator;

// Test that various types satisfy the address concept
BOOST_AUTO_TEST_CASE(test_address_concept, * boost::unit_test::timeout(15)) {
    // std::string should satisfy address concept
    static_assert(address<std::string>, "std::string should satisfy address concept");
    
    // unsigned long should satisfy address concept
    static_assert(address<unsigned long>, "unsigned long should satisfy address concept");
    
    // IPv4Address wrapper should satisfy address concept
    static_assert(address<IPv4Address>, "IPv4Address should satisfy address concept");
    
    // IPv6Address wrapper should satisfy address concept
    static_assert(address<IPv6Address>, "IPv6Address should satisfy address concept");
    
    BOOST_TEST(true);
}

// Test that various types satisfy the port concept
BOOST_AUTO_TEST_CASE(test_port_concept, * boost::unit_test::timeout(15)) {
    // unsigned short should satisfy port concept
    static_assert(port<unsigned short>, "unsigned short should satisfy port concept");
    
    // std::string should satisfy port concept
    static_assert(port<std::string>, "std::string should satisfy port concept");
    
    BOOST_TEST(true);
}

// Test constants
namespace {
    constexpr const char* test_src_addr = "192.168.1.1";
    constexpr unsigned short test_src_port = 8080;
    constexpr const char* test_dst_addr = "192.168.1.2";
    constexpr unsigned short test_dst_port = 9090;
    constexpr const char* test_payload_str = "Hello, World!";
    constexpr auto test_latency = std::chrono::milliseconds{100};
    constexpr double test_reliability = 0.95;
}

// Test Message type
BOOST_AUTO_TEST_CASE(test_message_type, * boost::unit_test::timeout(15)) {
    std::vector<std::byte> payload;
    for (char c : std::string(test_payload_str)) {
        payload.push_back(static_cast<std::byte>(c));
    }
    
    Message<DefaultNetworkTypes> msg(
        test_src_addr,
        test_src_port,
        test_dst_addr,
        test_dst_port,
        payload
    );
    
    BOOST_TEST(msg.source_address() == test_src_addr);
    BOOST_TEST(msg.source_port() == test_src_port);
    BOOST_TEST(msg.destination_address() == test_dst_addr);
    BOOST_TEST(msg.destination_port() == test_dst_port);
    BOOST_TEST(msg.payload().size() == payload.size());
}

// Test NetworkEdge type
BOOST_AUTO_TEST_CASE(test_network_edge_type, * boost::unit_test::timeout(15)) {
    NetworkEdge edge(test_latency, test_reliability);
    
    BOOST_TEST(edge.latency() == test_latency);
    BOOST_TEST(edge.reliability() == test_reliability);
    
    // Verify it satisfies the network_edge concept
    static_assert(network_edge<NetworkEdge>, "NetworkEdge should satisfy network_edge concept");
}

// Test Endpoint type
BOOST_AUTO_TEST_CASE(test_endpoint_type, * boost::unit_test::timeout(15)) {
    Endpoint<DefaultNetworkTypes> ep1(test_src_addr, test_src_port);
    Endpoint<DefaultNetworkTypes> ep2(test_src_addr, test_src_port);
    Endpoint<DefaultNetworkTypes> ep3(test_dst_addr, test_src_port);
    
    BOOST_TEST(ep1.address == test_src_addr);
    BOOST_TEST(ep1.port == test_src_port);
    BOOST_TEST((ep1 == ep2));
    BOOST_TEST((ep1 != ep3));
    
    // Test hashing
    std::hash<Endpoint<DefaultNetworkTypes>> hasher;
    auto hash1 = hasher(ep1);
    auto hash2 = hasher(ep2);
    BOOST_TEST(hash1 == hash2);
}

// Test IPv4Address wrapper
BOOST_AUTO_TEST_CASE(test_ipv4_address_wrapper, * boost::unit_test::timeout(15)) {
    in_addr addr1{};
    addr1.s_addr = htonl(0xC0A80101);  // 192.168.1.1
    
    in_addr addr2{};
    addr2.s_addr = htonl(0xC0A80101);  // 192.168.1.1
    
    in_addr addr3{};
    addr3.s_addr = htonl(0xC0A80102);  // 192.168.1.2
    
    IPv4Address ipv4_1(addr1);
    IPv4Address ipv4_2(addr2);
    IPv4Address ipv4_3(addr3);
    
    // Test equality
    BOOST_TEST((ipv4_1 == ipv4_2));
    BOOST_TEST((ipv4_1 != ipv4_3));
    
    // Test hashing
    std::hash<IPv4Address> hasher;
    auto hash1 = hasher(ipv4_1);
    auto hash2 = hasher(ipv4_2);
    BOOST_TEST(hash1 == hash2);
}

// Test IPv6Address wrapper
BOOST_AUTO_TEST_CASE(test_ipv6_address_wrapper, * boost::unit_test::timeout(15)) {
    in6_addr addr1{};
    addr1.s6_addr[0] = 0x20;
    addr1.s6_addr[1] = 0x01;
    addr1.s6_addr[15] = 0x01;
    
    in6_addr addr2{};
    addr2.s6_addr[0] = 0x20;
    addr2.s6_addr[1] = 0x01;
    addr2.s6_addr[15] = 0x01;
    
    in6_addr addr3{};
    addr3.s6_addr[0] = 0x20;
    addr3.s6_addr[1] = 0x01;
    addr3.s6_addr[15] = 0x02;
    
    IPv6Address ipv6_1(addr1);
    IPv6Address ipv6_2(addr2);
    IPv6Address ipv6_3(addr3);
    
    // Test equality
    BOOST_TEST((ipv6_1 == ipv6_2));
    BOOST_TEST((ipv6_1 != ipv6_3));
    
    // Test hashing
    std::hash<IPv6Address> hasher;
    auto hash1 = hasher(ipv6_1);
    auto hash2 = hasher(ipv6_2);
    BOOST_TEST(hash1 == hash2);
}

// Test SimpleFuture type satisfies future concept
BOOST_AUTO_TEST_CASE(test_simple_future_concept, * boost::unit_test::timeout(15)) {
    // Test that SimpleFuture satisfies the future concept
    static_assert(future<SimpleFuture<bool>, bool>, "SimpleFuture<bool> should satisfy future concept");
    static_assert(future<SimpleFuture<int>, int>, "SimpleFuture<int> should satisfy future concept");
    static_assert(future<SimpleFuture<std::string>, std::string>, "SimpleFuture<std::string> should satisfy future concept");
    
    // Test basic functionality
    SimpleFuture<int> fut(42);
    BOOST_TEST(fut.isReady());
    BOOST_TEST(fut.get() == 42);
    
    // Test then() chaining
    auto fut2 = fut.then([](int x) { return x * 2; });
    BOOST_TEST(fut2.get() == 84);
}

// Test Message concept satisfaction
BOOST_AUTO_TEST_CASE(test_message_concept_satisfaction, * boost::unit_test::timeout(15)) {
    using TestMessage = Message<DefaultNetworkTypes>;
    
    // Verify Message satisfies the message concept
    static_assert(message<TestMessage, std::string, unsigned short>, 
                  "Message<DefaultNetworkTypes> should satisfy message concept");
    
    BOOST_TEST(true);
}

// Test DefaultNetworkTypes satisfies network_simulator_types concept
BOOST_AUTO_TEST_CASE(test_default_network_types_concept, * boost::unit_test::timeout(15)) {
    // This is the main test for the network_simulator_types concept
    static_assert(network_simulator_types<DefaultNetworkTypes>, 
                  "DefaultNetworkTypes should satisfy network_simulator_types concept");
    
    // Verify individual type constraints
    static_assert(address<DefaultNetworkTypes::address_type>, 
                  "DefaultNetworkTypes::address_type should satisfy address concept");
    
    static_assert(port<DefaultNetworkTypes::port_type>, 
                  "DefaultNetworkTypes::port_type should satisfy port concept");
    
    static_assert(future<DefaultNetworkTypes::future_bool_type, bool>, 
                  "DefaultNetworkTypes::future_bool_type should satisfy future concept");
    
    static_assert(future<DefaultNetworkTypes::future_message_type, DefaultNetworkTypes::message_type>, 
                  "DefaultNetworkTypes::future_message_type should satisfy future concept");
    
    static_assert(future<DefaultNetworkTypes::future_connection_type, std::shared_ptr<DefaultNetworkTypes::connection_type>>, 
                  "DefaultNetworkTypes::future_connection_type should satisfy future concept");
    
    static_assert(future<DefaultNetworkTypes::future_listener_type, std::shared_ptr<DefaultNetworkTypes::listener_type>>, 
                  "DefaultNetworkTypes::future_listener_type should satisfy future concept");
    
    static_assert(future<DefaultNetworkTypes::future_bytes_type, std::vector<std::byte>>, 
                  "DefaultNetworkTypes::future_bytes_type should satisfy future concept");
    
    BOOST_TEST(true);
}

// Test alternative Types implementation with different address/port types
BOOST_AUTO_TEST_CASE(test_alternative_types_concept, * boost::unit_test::timeout(15)) {
    // Define an alternative Types implementation using IPv4 addresses and string ports
    struct IPv4NetworkTypes {
        using address_type = IPv4Address;
        using port_type = std::string;
        using message_type = Message<IPv4NetworkTypes>;
        using connection_type = Connection<IPv4NetworkTypes>;
        using listener_type = Listener<IPv4NetworkTypes>;
        using node_type = NetworkNode<IPv4NetworkTypes>;
        
        using future_bool_type = SimpleFuture<bool>;
        using future_message_type = SimpleFuture<message_type>;
        using future_connection_type = SimpleFuture<std::shared_ptr<connection_type>>;
        using future_listener_type = SimpleFuture<std::shared_ptr<listener_type>>;
        using future_bytes_type = SimpleFuture<std::vector<std::byte>>;
    };
    
    // Verify this alternative implementation also satisfies the concept
    static_assert(network_simulator_types<IPv4NetworkTypes>, 
                  "IPv4NetworkTypes should satisfy network_simulator_types concept");
    
    BOOST_TEST(true);
}

// Test that in_addr and in6_addr satisfy address concept through wrappers
BOOST_AUTO_TEST_CASE(test_native_address_types, * boost::unit_test::timeout(15)) {
    // Test that the raw types don't satisfy the concept (they lack hash specialization)
    // but our wrappers do
    static_assert(address<IPv4Address>, "IPv4Address wrapper should satisfy address concept");
    static_assert(address<IPv6Address>, "IPv6Address wrapper should satisfy address concept");
    
    // Test construction and basic operations
    in_addr raw_ipv4{};
    raw_ipv4.s_addr = htonl(0xC0A80101);  // 192.168.1.1
    IPv4Address ipv4_addr(raw_ipv4);
    
    in6_addr raw_ipv6{};
    raw_ipv6.s6_addr[0] = 0x20;
    raw_ipv6.s6_addr[1] = 0x01;
    IPv6Address ipv6_addr(raw_ipv6);
    
    // Test that they can be used in hash containers
    std::hash<IPv4Address> ipv4_hasher;
    std::hash<IPv6Address> ipv6_hasher;
    
    auto ipv4_hash = ipv4_hasher(ipv4_addr);
    auto ipv6_hash = ipv6_hasher(ipv6_addr);
    
    // Just verify the hash functions don't crash
    BOOST_TEST(ipv4_hash == ipv4_hash);  // Always true, just to use the hash
    BOOST_TEST(ipv6_hash == ipv6_hash);  // Always true, just to use the hash
}
