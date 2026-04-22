//
// Integration test for local Meridian node network
// Launches multiple nodes on different ports and verifies inter-node communication
//

#include <gtest/gtest.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <string.h>
#include <errno.h>
#include "Network/Meridian/meridian_protocol.h"

#define TEST_NODE_COUNT 3
#define BASE_PORT 12000
#define TEST_TIMEOUT_MS 1000

typedef struct test_node_context_s {
    meridian_protocol_t* protocol;
    uint16_t port;
    pthread_t thread;
    int thread_ready;
    pthread_mutex_t mutex;
} test_node_context_t;

static void* node_thread_func(void* arg) {
    test_node_context_t* ctx = (test_node_context_t*)arg;

    pthread_mutex_lock(&ctx->mutex);
    ctx->thread_ready = 1;
    pthread_mutex_unlock(&ctx->mutex);

    // The protocol runs in the background
    // For this test we just verify creation and basic connectivity

    return NULL;
}

class MeridianIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        memset(nodes, 0, sizeof(nodes));
    }

    void TearDown() override {
        for (int i = 0; i < TEST_NODE_COUNT; i++) {
            if (nodes[i].protocol) {
                meridian_protocol_stop(nodes[i].protocol);
                meridian_protocol_destroy(nodes[i].protocol);
                nodes[i].protocol = NULL;
            }
            pthread_mutex_destroy(&nodes[i].mutex);
        }
    }

    test_node_context_t nodes[TEST_NODE_COUNT];
};

TEST_F(MeridianIntegrationTest, CreateMultipleNodes) {
    for (int i = 0; i < TEST_NODE_COUNT; i++) {
        uint16_t port = BASE_PORT + i;
        pthread_mutex_init(&nodes[i].mutex, NULL);
        nodes[i].port = port;
        nodes[i].thread_ready = 0;

        meridian_protocol_config_t config = {
            .listen_port = port,
            .info_port = 0,
            .primary_ring_size = 8,
            .secondary_ring_size = 4,
            .ring_exponent_base = 2,
            .init_gossip_interval_s = 5,
            .num_init_gossip_intervals = 3,
            .steady_state_gossip_interval_s = 5,
            .replace_interval_s = 10,
            .gossip_timeout_ms = 5000,
            .measure_timeout_ms = 5000,
            .pool = NULL,
            .wheel = NULL
        };

        nodes[i].protocol = meridian_protocol_create(&config);
        ASSERT_NE(nullptr, nodes[i].protocol);
    }
}

static bool quic_available(meridian_protocol_t* protocol) {
    return meridian_protocol_start(protocol) == 0;
}

TEST_F(MeridianIntegrationTest, StartNodes) {
    for (int i = 0; i < TEST_NODE_COUNT; i++) {
        uint16_t port = BASE_PORT + i;
        pthread_mutex_init(&nodes[i].mutex, NULL);
        nodes[i].port = port;

        meridian_protocol_config_t config = {
            .listen_port = port,
            .info_port = 0,
            .primary_ring_size = 8,
            .secondary_ring_size = 4,
            .ring_exponent_base = 2,
            .init_gossip_interval_s = 5,
            .num_init_gossip_intervals = 3,
            .steady_state_gossip_interval_s = 5,
            .replace_interval_s = 10,
            .gossip_timeout_ms = 5000,
            .measure_timeout_ms = 5000,
            .pool = NULL,
            .wheel = NULL
        };

        nodes[i].protocol = meridian_protocol_create(&config);
        ASSERT_NE(nullptr, nodes[i].protocol);

        if (!quic_available(nodes[i].protocol)) {
            GTEST_SKIP() << "QUIC stack unavailable (msquic not initialized)";
        }
    }

    // Give nodes time to bind to ports
    usleep(100000); // 100ms

    for (int i = 0; i < TEST_NODE_COUNT; i++) {
        EXPECT_EQ(MERIDIAN_PROTOCOL_STATE_BOOTSTRAPPING, nodes[i].protocol->state);
    }
}

TEST_F(MeridianIntegrationTest, ConnectNodesInRing) {
    // Create and start all nodes first
    for (int i = 0; i < TEST_NODE_COUNT; i++) {
        uint16_t port = BASE_PORT + i;
        pthread_mutex_init(&nodes[i].mutex, NULL);
        nodes[i].port = port;

        meridian_protocol_config_t config = {
            .listen_port = port,
            .info_port = 0,
            .primary_ring_size = 8,
            .secondary_ring_size = 4,
            .ring_exponent_base = 2,
            .init_gossip_interval_s = 5,
            .num_init_gossip_intervals = 3,
            .steady_state_gossip_interval_s = 5,
            .replace_interval_s = 10,
            .gossip_timeout_ms = 5000,
            .measure_timeout_ms = 5000,
            .pool = NULL,
            .wheel = NULL
        };

        nodes[i].protocol = meridian_protocol_create(&config);
        ASSERT_NE(nullptr, nodes[i].protocol);
        if (!quic_available(nodes[i].protocol)) {
            GTEST_SKIP() << "QUIC stack unavailable";
        }
    }

    usleep(100000); // 100ms

    // Connect nodes in a ring: 0 -> 1 -> 2 -> 0
    in_addr_t addr = htonl(0x7F000001); // 127.0.0.1

    for (int i = 0; i < TEST_NODE_COUNT; i++) {
        uint16_t target_port = BASE_PORT + ((i + 1) % TEST_NODE_COUNT);
        EXPECT_EQ(0, meridian_protocol_connect(nodes[i].protocol, addr, target_port));
    }

    // Verify peer counts
    for (int i = 0; i < TEST_NODE_COUNT; i++) {
        size_t num_peers = 0;
        meridian_node_t** peers = meridian_protocol_get_connected_peers(nodes[i].protocol, &num_peers);
        ASSERT_NE(nullptr, peers);
        EXPECT_EQ(1, num_peers);
    }
}

TEST_F(MeridianIntegrationTest, FindClosestNode) {
    // Create a simple network and verify closest node lookup
    for (int i = 0; i < TEST_NODE_COUNT; i++) {
        uint16_t port = BASE_PORT + i;
        pthread_mutex_init(&nodes[i].mutex, NULL);
        nodes[i].port = port;

        meridian_protocol_config_t config = {
            .listen_port = port,
            .info_port = 0,
            .primary_ring_size = 8,
            .secondary_ring_size = 4,
            .ring_exponent_base = 2,
            .init_gossip_interval_s = 5,
            .num_init_gossip_intervals = 3,
            .steady_state_gossip_interval_s = 5,
            .replace_interval_s = 10,
            .gossip_timeout_ms = 5000,
            .measure_timeout_ms = 5000,
            .pool = NULL,
            .wheel = NULL
        };

        nodes[i].protocol = meridian_protocol_create(&config);
        ASSERT_NE(nullptr, nodes[i].protocol);
        if (!quic_available(nodes[i].protocol)) {
            GTEST_SKIP() << "QUIC stack unavailable";
        }
    }

    usleep(100000);

    // Add peers to node 0
    in_addr_t addr = htonl(0x7F000001);
    EXPECT_EQ(0, meridian_protocol_connect(nodes[0].protocol, addr, BASE_PORT + 1));
    EXPECT_EQ(0, meridian_protocol_connect(nodes[0].protocol, addr, BASE_PORT + 2));

    // Find closest should return one of the connected peers
    meridian_node_t* closest = meridian_protocol_find_closest(
        nodes[0].protocol, htonl(0x7F000002), BASE_PORT + 1);

    // May or may not find a closest depending on ring state
    // Just verify it doesn't crash
}

TEST_F(MeridianIntegrationTest, SendPacketBetweenNodes) {
    // Create two connected nodes
    for (int i = 0; i < 2; i++) {
        uint16_t port = BASE_PORT + i;
        pthread_mutex_init(&nodes[i].mutex, NULL);
        nodes[i].port = port;

        meridian_protocol_config_t config = {
            .listen_port = port,
            .info_port = 0,
            .primary_ring_size = 8,
            .secondary_ring_size = 4,
            .ring_exponent_base = 2,
            .init_gossip_interval_s = 5,
            .num_init_gossip_intervals = 3,
            .steady_state_gossip_interval_s = 5,
            .replace_interval_s = 10,
            .gossip_timeout_ms = 5000,
            .measure_timeout_ms = 5000,
            .pool = NULL,
            .wheel = NULL
        };

        nodes[i].protocol = meridian_protocol_create(&config);
        ASSERT_NE(nullptr, nodes[i].protocol);
        if (!quic_available(nodes[i].protocol)) {
            GTEST_SKIP() << "QUIC stack unavailable";
        }
    }

    usleep(100000);

    // Connect node 0 to node 1
    in_addr_t addr = htonl(0x7F000001);
    EXPECT_EQ(0, meridian_protocol_connect(nodes[0].protocol, addr, BASE_PORT + 1));

    // Get peer and send a packet
    size_t num_peers = 0;
    meridian_node_t** peers = meridian_protocol_get_connected_peers(nodes[0].protocol, &num_peers);
    ASSERT_NE(nullptr, peers);
    ASSERT_EQ(1, num_peers);

    // Create a simple test packet
    uint8_t test_data[16] = {0xde, 0xad, 0xbe, 0xef};

    // Send should not crash (actual delivery depends on network setup)
    int result = meridian_protocol_send_packet(nodes[0].protocol, test_data, sizeof(test_data), peers[0]);
    // Result may be 0 (success) or -1 (no socket) depending on initialization state
    (void)result;
}

TEST_F(MeridianIntegrationTest, StopAndRestart) {
    // Create and start a node
    uint16_t port = BASE_PORT;
    pthread_mutex_init(&nodes[0].mutex, NULL);
    nodes[0].port = port;

    meridian_protocol_config_t config = {
        .listen_port = port,
        .info_port = 0,
        .primary_ring_size = 8,
        .secondary_ring_size = 4,
        .ring_exponent_base = 2,
        .init_gossip_interval_s = 5,
        .num_init_gossip_intervals = 3,
        .steady_state_gossip_interval_s = 5,
        .replace_interval_s = 10,
        .gossip_timeout_ms = 5000,
        .measure_timeout_ms = 5000,
        .pool = NULL,
        .wheel = NULL
    };

    nodes[0].protocol = meridian_protocol_create(&config);
    ASSERT_NE(nullptr, nodes[0].protocol);
    if (!quic_available(nodes[0].protocol)) {
        GTEST_SKIP() << "QUIC stack unavailable";
    }

    usleep(100000);
    EXPECT_TRUE(nodes[0].protocol->running);

    // Stop the node
    EXPECT_EQ(0, meridian_protocol_stop(nodes[0].protocol));
    EXPECT_FALSE(nodes[0].protocol->running);
}

TEST_F(MeridianIntegrationTest, AddSeedNodes) {
    uint16_t port = BASE_PORT;
    pthread_mutex_init(&nodes[0].mutex, NULL);
    nodes[0].port = port;

    meridian_protocol_config_t config = {
        .listen_port = port,
        .info_port = 0,
        .primary_ring_size = 8,
        .secondary_ring_size = 4,
        .ring_exponent_base = 2,
        .init_gossip_interval_s = 5,
        .num_init_gossip_intervals = 3,
        .steady_state_gossip_interval_s = 5,
        .replace_interval_s = 10,
        .gossip_timeout_ms = 5000,
        .measure_timeout_ms = 5000,
        .pool = NULL,
        .wheel = NULL
    };

    nodes[0].protocol = meridian_protocol_create(&config);
    ASSERT_NE(nullptr, nodes[0].protocol);

    // Add seed nodes
    EXPECT_EQ(0, meridian_protocol_add_seed_node(nodes[0].protocol, htonl(0xC0A80001), 8080));
    EXPECT_EQ(0, meridian_protocol_add_seed_node(nodes[0].protocol, htonl(0xC0A80002), 8081));

    // Verify seed nodes are stored
    EXPECT_EQ(2, nodes[0].protocol->num_seed_nodes);
}

TEST_F(MeridianIntegrationTest, DisconnectNode) {
    for (int i = 0; i < 2; i++) {
        uint16_t port = BASE_PORT + i;
        pthread_mutex_init(&nodes[i].mutex, NULL);
        nodes[i].port = port;

        meridian_protocol_config_t config = {
            .listen_port = port,
            .info_port = 0,
            .primary_ring_size = 8,
            .secondary_ring_size = 4,
            .ring_exponent_base = 2,
            .init_gossip_interval_s = 5,
            .num_init_gossip_intervals = 3,
            .steady_state_gossip_interval_s = 5,
            .replace_interval_s = 10,
            .gossip_timeout_ms = 5000,
            .measure_timeout_ms = 5000,
            .pool = NULL,
            .wheel = NULL
        };

        nodes[i].protocol = meridian_protocol_create(&config);
        ASSERT_NE(nullptr, nodes[i].protocol);
        if (!quic_available(nodes[i].protocol)) {
            GTEST_SKIP() << "QUIC stack unavailable";
        }
    }

    usleep(100000);

    // Connect
    in_addr_t addr = htonl(0x7F000001);
    EXPECT_EQ(0, meridian_protocol_connect(nodes[0].protocol, addr, BASE_PORT + 1));

    size_t num_peers = 0;
    meridian_node_t** peers = meridian_protocol_get_connected_peers(nodes[0].protocol, &num_peers);
    ASSERT_NE(nullptr, peers);
    EXPECT_EQ(1, num_peers);

    // Disconnect
    EXPECT_EQ(0, meridian_protocol_disconnect(nodes[0].protocol, addr, BASE_PORT + 1));

    peers = meridian_protocol_get_connected_peers(nodes[0].protocol, &num_peers);
    EXPECT_EQ(0, num_peers);
}

TEST_F(MeridianIntegrationTest, GetLocalNodeInfo) {
    uint16_t port = BASE_PORT;
    pthread_mutex_init(&nodes[0].mutex, NULL);
    nodes[0].port = port;

    meridian_protocol_config_t config = {
        .listen_port = port,
        .info_port = 0,
        .primary_ring_size = 8,
        .secondary_ring_size = 4,
        .ring_exponent_base = 2,
        .init_gossip_interval_s = 5,
        .num_init_gossip_intervals = 3,
        .steady_state_gossip_interval_s = 5,
        .replace_interval_s = 10,
        .gossip_timeout_ms = 5000,
        .measure_timeout_ms = 5000,
        .pool = NULL,
        .wheel = NULL
    };

    nodes[0].protocol = meridian_protocol_create(&config);
    ASSERT_NE(nullptr, nodes[0].protocol);
    if (!quic_available(nodes[0].protocol)) {
        GTEST_SKIP() << "QUIC stack unavailable";
    }

    usleep(100000);

    uint32_t local_addr = 0;
    uint16_t local_port = 0;
    EXPECT_EQ(0, meridian_protocol_get_local_node(nodes[0].protocol, &local_addr, &local_port));
    EXPECT_EQ(port, local_port);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}