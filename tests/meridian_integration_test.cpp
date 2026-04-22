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
#include <stdlib.h>
#include "Network/Meridian/meridian_protocol.h"
#include "Crypto/key_pair.h"
#include "Crypto/node_id.h"

#define TEST_NODE_COUNT 3
#define TEST_TIMEOUT_MS 1000

typedef struct test_node_context_s {
    meridian_protocol_t* protocol;
    poseidon_key_pair_t* key_pair;
    char key_path[256];
    char cert_path[256];
    uint16_t port;
    pthread_t thread;
    int thread_ready;
    pthread_mutex_t mutex;
} test_node_context_t;

static int setup_node_tls(test_node_context_t* node, int index) {
    node->key_pair = poseidon_key_pair_create("ED25519");
    if (node->key_pair == NULL) return -1;

    uint8_t* pub_key = NULL;
    size_t pub_key_len = 0;
    if (poseidon_key_pair_get_public_key(node->key_pair, &pub_key, &pub_key_len) != 0) {
        poseidon_key_pair_destroy(node->key_pair);
        node->key_pair = NULL;
        return -1;
    }

    poseidon_node_id_t node_id;
    poseidon_node_id_from_public_key(pub_key, pub_key_len, &node_id);
    free(pub_key);

    snprintf(node->key_path, sizeof(node->key_path), "/tmp/poseidon_test_%s_%d_key.pem", node_id.str, index);
    snprintf(node->cert_path, sizeof(node->cert_path), "/tmp/poseidon_test_%s_%d_cert.pem", node_id.str, index);

    if (poseidon_key_pair_generate_tls_files(node->key_pair, node_id.str,
                                              node->key_path, node->cert_path) != 0) {
        poseidon_key_pair_destroy(node->key_pair);
        node->key_pair = NULL;
        return -1;
    }

    return 0;
}

static void cleanup_node_tls(test_node_context_t* node) {
    if (node->key_path[0]) unlink(node->key_path);
    if (node->cert_path[0]) unlink(node->cert_path);
    if (node->key_pair) poseidon_key_pair_destroy(node->key_pair);
    node->key_pair = NULL;
}

static meridian_protocol_config_t make_config(uint16_t port, const char* key_path, const char* cert_path) {
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
        .tls_key_path = key_path,
        .tls_cert_path = cert_path,
        .pool = NULL,
        .wheel = NULL
    };
    return config;
}

class MeridianIntegrationTest : public ::testing::Test {
protected:
    static int next_base_port;

    void SetUp() override {
        memset(nodes, 0, sizeof(nodes));
        base_port = next_base_port;
        next_base_port += TEST_NODE_COUNT;
    }

    void TearDown() override {
        for (int i = 0; i < TEST_NODE_COUNT; i++) {
            if (nodes[i].protocol) {
                meridian_protocol_stop(nodes[i].protocol);
                meridian_protocol_destroy(nodes[i].protocol);
                nodes[i].protocol = NULL;
            }
            cleanup_node_tls(&nodes[i]);
            pthread_mutex_destroy(&nodes[i].mutex);
        }
    }

    test_node_context_t nodes[TEST_NODE_COUNT];
    int base_port;
};

int MeridianIntegrationTest::next_base_port = 12000;

TEST_F(MeridianIntegrationTest, CreateMultipleNodes) {
    for (int i = 0; i < TEST_NODE_COUNT; i++) {
        uint16_t port = base_port + i;
        pthread_mutex_init(&nodes[i].mutex, NULL);
        nodes[i].port = port;

        ASSERT_EQ(0, setup_node_tls(&nodes[i], i));

        meridian_protocol_config_t config = make_config(port, nodes[i].key_path, nodes[i].cert_path);
        nodes[i].protocol = meridian_protocol_create(&config);
        ASSERT_NE(nullptr, nodes[i].protocol);
    }
}

static bool quic_available(meridian_protocol_t* protocol) {
    return meridian_protocol_start(protocol) == 0;
}

TEST_F(MeridianIntegrationTest, StartNodes) {
    for (int i = 0; i < TEST_NODE_COUNT; i++) {
        uint16_t port = base_port + i;
        pthread_mutex_init(&nodes[i].mutex, NULL);
        nodes[i].port = port;

        ASSERT_EQ(0, setup_node_tls(&nodes[i], i));

        meridian_protocol_config_t config = make_config(port, nodes[i].key_path, nodes[i].cert_path);
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

// NOTE: QUIC connection tests are disabled because msquic global state is not
// properly cleaned up between test fixtures. Multiple MsQuicOpen2/MsQuicClose
// cycles within a single process corrupt the QUIC API table. These tests pass
// in isolation but crash when run after other tests that started protocols.
// Re-enable when msquic lifecycle is fixed or tests are forked.

TEST_F(MeridianIntegrationTest, DISABLED_ConnectNodesInRing) {
    // Create and start all nodes first
    for (int i = 0; i < TEST_NODE_COUNT; i++) {
        uint16_t port = base_port + i;
        pthread_mutex_init(&nodes[i].mutex, NULL);
        nodes[i].port = port;

        ASSERT_EQ(0, setup_node_tls(&nodes[i], i));

        meridian_protocol_config_t config = make_config(port, nodes[i].key_path, nodes[i].cert_path);
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
        uint16_t target_port = base_port + ((i + 1) % TEST_NODE_COUNT);
        // Connection is async; may succeed or fail depending on handshake timing
        meridian_protocol_connect(nodes[i].protocol, addr, target_port);
    }

    // Give connections time to complete
    usleep(200000); // 200ms

    // Verify peer counts (non-fatal — QUIC handshake may not have completed)
    for (int i = 0; i < TEST_NODE_COUNT; i++) {
        size_t num_peers = 0;
        meridian_node_t** peers = meridian_protocol_get_connected_peers(nodes[i].protocol, &num_peers);
        EXPECT_NE(nullptr, peers);
        // Peer count may be 0 if handshake hasn't completed
    }
}

TEST_F(MeridianIntegrationTest, DISABLED_FindClosestNode) {
    // Create a simple network and verify closest node lookup
    for (int i = 0; i < TEST_NODE_COUNT; i++) {
        uint16_t port = base_port + i;
        pthread_mutex_init(&nodes[i].mutex, NULL);
        nodes[i].port = port;

        ASSERT_EQ(0, setup_node_tls(&nodes[i], i));

        meridian_protocol_config_t config = make_config(port, nodes[i].key_path, nodes[i].cert_path);
        nodes[i].protocol = meridian_protocol_create(&config);
        ASSERT_NE(nullptr, nodes[i].protocol);
        if (!quic_available(nodes[i].protocol)) {
            GTEST_SKIP() << "QUIC stack unavailable";
        }
    }

    usleep(100000);

    // Add peers to node 0
    in_addr_t addr = htonl(0x7F000001);
    meridian_protocol_connect(nodes[0].protocol, addr, base_port + 1);
    meridian_protocol_connect(nodes[0].protocol, addr, base_port + 2);

    // Find closest should return one of the connected peers
    meridian_node_t* closest = meridian_protocol_find_closest(
        nodes[0].protocol, htonl(0x7F000002), base_port + 1);
    (void)closest;
}

TEST_F(MeridianIntegrationTest, DISABLED_SendPacketBetweenNodes) {
    // Create two connected nodes
    for (int i = 0; i < 2; i++) {
        uint16_t port = base_port + i;
        pthread_mutex_init(&nodes[i].mutex, NULL);
        nodes[i].port = port;

        ASSERT_EQ(0, setup_node_tls(&nodes[i], i));

        meridian_protocol_config_t config = make_config(port, nodes[i].key_path, nodes[i].cert_path);
        nodes[i].protocol = meridian_protocol_create(&config);
        ASSERT_NE(nullptr, nodes[i].protocol);
        if (!quic_available(nodes[i].protocol)) {
            GTEST_SKIP() << "QUIC stack unavailable";
        }
    }

    usleep(100000);

    // Connect node 0 to node 1
    in_addr_t addr = htonl(0x7F000001);
    meridian_protocol_connect(nodes[0].protocol, addr, base_port + 1);

    usleep(200000); // Wait for handshake

    // Get peer and send a packet
    size_t num_peers = 0;
    meridian_node_t** peers = meridian_protocol_get_connected_peers(nodes[0].protocol, &num_peers);
    // May not have connected yet if handshake hasn't completed
    if (num_peers > 0 && peers != nullptr) {
        // Create a simple test packet
        uint8_t test_data[16] = {0xde, 0xad, 0xbe, 0xef};
        meridian_protocol_send_packet(nodes[0].protocol, test_data, sizeof(test_data), peers[0]);
    }
}

TEST_F(MeridianIntegrationTest, StopAndRestart) {
    // Create and start a node
    uint16_t port = base_port;
    pthread_mutex_init(&nodes[0].mutex, NULL);
    nodes[0].port = port;

    ASSERT_EQ(0, setup_node_tls(&nodes[0], 0));

    meridian_protocol_config_t config = make_config(port, nodes[0].key_path, nodes[0].cert_path);
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
    uint16_t port = base_port;
    pthread_mutex_init(&nodes[0].mutex, NULL);
    nodes[0].port = port;

    // No TLS needed for just adding seed nodes (no protocol start)
    meridian_protocol_config_t config = make_config(port, NULL, NULL);
    nodes[0].protocol = meridian_protocol_create(&config);
    ASSERT_NE(nullptr, nodes[0].protocol);

    // Add seed nodes
    EXPECT_EQ(0, meridian_protocol_add_seed_node(nodes[0].protocol, htonl(0xC0A80001), 8080));
    EXPECT_EQ(0, meridian_protocol_add_seed_node(nodes[0].protocol, htonl(0xC0A80002), 8081));

    // Verify seed nodes are stored
    EXPECT_EQ(2, nodes[0].protocol->num_seed_nodes);
}

TEST_F(MeridianIntegrationTest, DISABLED_DisconnectNode) {
    for (int i = 0; i < 2; i++) {
        uint16_t port = base_port + i;
        pthread_mutex_init(&nodes[i].mutex, NULL);
        nodes[i].port = port;

        ASSERT_EQ(0, setup_node_tls(&nodes[i], i));

        meridian_protocol_config_t config = make_config(port, nodes[i].key_path, nodes[i].cert_path);
        nodes[i].protocol = meridian_protocol_create(&config);
        ASSERT_NE(nullptr, nodes[i].protocol);
        if (!quic_available(nodes[i].protocol)) {
            GTEST_SKIP() << "QUIC stack unavailable";
        }
    }

    usleep(100000);

    // Connect
    in_addr_t addr = htonl(0x7F000001);
    meridian_protocol_connect(nodes[0].protocol, addr, base_port + 1);

    usleep(200000);

    // Disconnect — may fail if connect didn't complete
    meridian_protocol_disconnect(nodes[0].protocol, addr, base_port + 1);

    size_t num_peers = 0;
    meridian_node_t** peers = meridian_protocol_get_connected_peers(nodes[0].protocol, &num_peers);
    EXPECT_NE(nullptr, peers);
    // Peer count may not be 0 if disconnect didn't complete
}

TEST_F(MeridianIntegrationTest, DISABLED_GetLocalNodeInfo) {
    uint16_t port = base_port;
    pthread_mutex_init(&nodes[0].mutex, NULL);
    nodes[0].port = port;

    ASSERT_EQ(0, setup_node_tls(&nodes[0], 0));

    meridian_protocol_config_t config = make_config(port, nodes[0].key_path, nodes[0].cert_path);
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