#include <gtest/gtest.h>
#include "Network/Meridian/meridian_conn.h"
#include "Network/Meridian/meridian.h"

class ConnTest : public ::testing::Test {
protected:
    void SetUp() override {}
    void TearDown() override {}
};

TEST_F(ConnTest, CreateDestroy) {
    meridian_node_t* peer = meridian_node_create(0xC0A80001, 8080);
    ASSERT_NE(nullptr, peer);
    meridian_conn_t* conn = meridian_conn_create(peer, NULL, MERIDIAN_NAT_TYPE_OPEN);
    ASSERT_NE(nullptr, conn);
    EXPECT_EQ(MERIDIAN_CONN_STATE_TRYING_DIRECT, meridian_conn_get_state(conn));
    meridian_conn_destroy(conn);
    meridian_node_destroy(peer);
}

TEST_F(ConnTest, SymmetricNatStartsRelayOnly) {
    meridian_node_t* peer = meridian_node_create(0xC0A80001, 8080);
    meridian_conn_t* conn = meridian_conn_create(peer, NULL, MERIDIAN_NAT_TYPE_SYMMETRIC);
    ASSERT_NE(nullptr, conn);
    EXPECT_EQ(MERIDIAN_CONN_STATE_RELAY_ONLY, meridian_conn_get_state(conn));
    EXPECT_FALSE(meridian_conn_is_direct(conn));
    EXPECT_TRUE(meridian_conn_is_relay(conn));
    meridian_conn_destroy(conn);
    meridian_node_destroy(peer);
}

TEST_F(ConnTest, OpenNatStartsTryingDirect) {
    meridian_node_t* peer = meridian_node_create(0xC0A80001, 8080);
    meridian_conn_t* conn = meridian_conn_create(peer, NULL, MERIDIAN_NAT_TYPE_OPEN);
    ASSERT_NE(nullptr, conn);
    EXPECT_EQ(MERIDIAN_CONN_STATE_TRYING_DIRECT, meridian_conn_get_state(conn));
    meridian_conn_destroy(conn);
    meridian_node_destroy(peer);
}

TEST_F(ConnTest, SetPeerNatType) {
    meridian_node_t* peer = meridian_node_create(0xC0A80001, 8080);
    meridian_conn_t* conn = meridian_conn_create(peer, NULL, MERIDIAN_NAT_TYPE_OPEN);
    ASSERT_NE(nullptr, conn);
    meridian_conn_set_peer_nat_type(conn, MERIDIAN_NAT_TYPE_FULL_CONE);
    EXPECT_EQ(MERIDIAN_NAT_TYPE_FULL_CONE, conn->peer_nat_type);
    meridian_conn_destroy(conn);
    meridian_node_destroy(peer);
}

TEST_F(ConnTest, SetPeerSymmetricNatDowngrades) {
    meridian_node_t* peer = meridian_node_create(0xC0A80001, 8080);
    meridian_conn_t* conn = meridian_conn_create(peer, NULL, MERIDIAN_NAT_TYPE_OPEN);
    ASSERT_NE(nullptr, conn);
    meridian_conn_set_peer_nat_type(conn, MERIDIAN_NAT_TYPE_SYMMETRIC);
    EXPECT_EQ(MERIDIAN_CONN_STATE_RELAY_ONLY, meridian_conn_get_state(conn));
    EXPECT_FALSE(meridian_conn_is_direct(conn));
    meridian_conn_destroy(conn);
    meridian_node_destroy(peer);
}

TEST_F(ConnTest, SetPeerReflexive) {
    meridian_node_t* peer = meridian_node_create(0xC0A80001, 8080);
    meridian_conn_t* conn = meridian_conn_create(peer, NULL, MERIDIAN_NAT_TYPE_OPEN);
    ASSERT_NE(nullptr, conn);
    meridian_conn_set_peer_reflexive(conn, 0x0A000001, 9090);
    EXPECT_EQ(0x0A000001u, conn->direct_path.reflexive_addr);
    EXPECT_EQ(9090u, conn->direct_path.reflexive_port);
    meridian_conn_destroy(conn);
    meridian_node_destroy(peer);
}

TEST_F(ConnTest, Disconnect) {
    meridian_node_t* peer = meridian_node_create(0xC0A80001, 8080);
    meridian_conn_t* conn = meridian_conn_create(peer, NULL, MERIDIAN_NAT_TYPE_OPEN);
    ASSERT_NE(nullptr, conn);
    meridian_conn_connect(conn);
    meridian_conn_disconnect(conn);
    EXPECT_EQ(MERIDIAN_CONN_STATE_RELAY, meridian_conn_get_state(conn));
    EXPECT_FALSE(meridian_conn_is_direct(conn));
    meridian_conn_destroy(conn);
    meridian_node_destroy(peer);
}

TEST_F(ConnTest, UpgradeToDirectRequiresReflexive) {
    meridian_node_t* peer = meridian_node_create(0xC0A80001, 8080);
    meridian_conn_t* conn = meridian_conn_create(peer, NULL, MERIDIAN_NAT_TYPE_PORT_RESTRICTED_CONE);
    ASSERT_NE(nullptr, conn);
    // Start in RELAY state (no peer address)
    conn->state = MERIDIAN_CONN_STATE_RELAY;
    // Upgrade fails without reflexive address
    EXPECT_EQ(-1, meridian_conn_upgrade_to_direct(conn));
    meridian_conn_destroy(conn);
    meridian_node_destroy(peer);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
