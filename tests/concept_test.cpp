#define BOOST_TEST_MODULE ConceptTest
#include <boost/test/included/unit_test.hpp>

#include <network_simulator/network_simulator.hpp>
#include <string>
#include <netinet/in.h>

using namespace network_simulator;

// Test that various types satisfy the address concept
BOOST_AUTO_TEST_CASE(test_address_concept) {
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
BOOST_AUTO_TEST_CASE(test_port_concept) {
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
BOOST_AUTO_TEST_CASE(test_message_type) {
    std::vector<std::byte> payload;
    for (char c : std::string(test_payload_str)) {
        payload.push_back(static_cast<std::byte>(c));
    }
    
    Message<std::string, unsigned short> msg(
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
BOOST_AUTO_TEST_CASE(test_network_edge_type) {
    NetworkEdge edge(test_latency, test_reliability);
    
    BOOST_TEST(edge.latency() == test_latency);
    BOOST_TEST(edge.reliability() == test_reliability);
    
    // Verify it satisfies the network_edge concept
    static_assert(network_edge<NetworkEdge>, "NetworkEdge should satisfy network_edge concept");
}

// Test Endpoint type
BOOST_AUTO_TEST_CASE(test_endpoint_type) {
    Endpoint<std::string, unsigned short> ep1(test_src_addr, test_src_port);
    Endpoint<std::string, unsigned short> ep2(test_src_addr, test_src_port);
    Endpoint<std::string, unsigned short> ep3(test_dst_addr, test_src_port);
    
    BOOST_TEST(ep1.address() == test_src_addr);
    BOOST_TEST(ep1.port() == test_src_port);
    BOOST_TEST((ep1 == ep2));
    BOOST_TEST((ep1 != ep3));
    
    // Test hashing
    std::hash<Endpoint<std::string, unsigned short>> hasher;
    auto hash1 = hasher(ep1);
    auto hash2 = hasher(ep2);
    BOOST_TEST(hash1 == hash2);
}

// Test IPv4Address wrapper
BOOST_AUTO_TEST_CASE(test_ipv4_address_wrapper) {
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
BOOST_AUTO_TEST_CASE(test_ipv6_address_wrapper) {
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
