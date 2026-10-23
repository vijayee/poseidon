#include <gtest/gtest.h>

extern "C" {
#include "Network/Meridian/meridian_conn.h"
#include "Network/Meridian/meridian.h"
#include "Scheduler/scheduler.h"
}

class ConnTest : public ::testing::Test {
protected:
    // The connection's actor runs on a pool worker: the public calls send
    // a request to the actor and wait for its answer. One worker is
    // enough — the requests are answered one at a time.
    void SetUp() override {
        pool_ = scheduler_pool_create(2);
        ASSERT_NE(nullptr, pool_);
        scheduler_pool_start(pool_);
    }

    void TearDown() override {
        if (pool_ != nullptr) {
            scheduler_pool_stop(pool_);
            scheduler_pool_destroy(pool_);
            pool_ = nullptr;
        }
    }

    scheduler_pool_t* pool_ = nullptr;
};

TEST_F(ConnTest, CreateDestroy) {
    meridian_node_t* peer = meridian_node_create(0xC0A80001, 8080, NULL);
    ASSERT_NE(nullptr, peer);
    meridian_conn_t* conn = meridian_conn_create(peer, NULL, MERIDIAN_NAT_TYPE_OPEN, NULL, pool_);
    ASSERT_NE(nullptr, conn);
    EXPECT_EQ(MERIDIAN_CONN_STATE_TRYING_DIRECT, meridian_conn_get_state(conn));
    meridian_conn_destroy(conn);
    meridian_node_destroy(peer);
}

TEST_F(ConnTest, SymmetricNatStartsRelayOnly) {
    meridian_node_t* peer = meridian_node_create(0xC0A80001, 8080, NULL);
    meridian_conn_t* conn = meridian_conn_create(peer, NULL, MERIDIAN_NAT_TYPE_SYMMETRIC, NULL, pool_);
    ASSERT_NE(nullptr, conn);
    EXPECT_EQ(MERIDIAN_CONN_STATE_RELAY_ONLY, meridian_conn_get_state(conn));
    EXPECT_FALSE(meridian_conn_is_direct(conn));
    EXPECT_TRUE(meridian_conn_is_relay(conn));
    meridian_conn_destroy(conn);
    meridian_node_destroy(peer);
}

TEST_F(ConnTest, OpenNatStartsTryingDirect) {
    meridian_node_t* peer = meridian_node_create(0xC0A80001, 8080, NULL);
    meridian_conn_t* conn = meridian_conn_create(peer, NULL, MERIDIAN_NAT_TYPE_OPEN, NULL, pool_);
    ASSERT_NE(nullptr, conn);
    EXPECT_EQ(MERIDIAN_CONN_STATE_TRYING_DIRECT, meridian_conn_get_state(conn));
    meridian_conn_destroy(conn);
    meridian_node_destroy(peer);
}

TEST_F(ConnTest, SetPeerNatType) {
    meridian_node_t* peer = meridian_node_create(0xC0A80001, 8080, NULL);
    meridian_conn_t* conn = meridian_conn_create(peer, NULL, MERIDIAN_NAT_TYPE_OPEN, NULL, pool_);
    ASSERT_NE(nullptr, conn);
    meridian_conn_set_peer_nat_type(conn, MERIDIAN_NAT_TYPE_FULL_CONE);
    EXPECT_EQ(MERIDIAN_NAT_TYPE_FULL_CONE, conn->peer_nat_type);
    meridian_conn_destroy(conn);
    meridian_node_destroy(peer);
}

TEST_F(ConnTest, SetPeerSymmetricNatDowngrades) {
    meridian_node_t* peer = meridian_node_create(0xC0A80001, 8080, NULL);
    meridian_conn_t* conn = meridian_conn_create(peer, NULL, MERIDIAN_NAT_TYPE_OPEN, NULL, pool_);
    ASSERT_NE(nullptr, conn);
    meridian_conn_set_peer_nat_type(conn, MERIDIAN_NAT_TYPE_SYMMETRIC);
    EXPECT_EQ(MERIDIAN_CONN_STATE_RELAY_ONLY, meridian_conn_get_state(conn));
    EXPECT_FALSE(meridian_conn_is_direct(conn));
    meridian_conn_destroy(conn);
    meridian_node_destroy(peer);
}

TEST_F(ConnTest, SetPeerReflexive) {
    meridian_node_t* peer = meridian_node_create(0xC0A80001, 8080, NULL);
    meridian_conn_t* conn = meridian_conn_create(peer, NULL, MERIDIAN_NAT_TYPE_OPEN, NULL, pool_);
    ASSERT_NE(nullptr, conn);
    meridian_conn_set_peer_reflexive(conn, 0x0A000001, 9090);
    EXPECT_EQ(0x0A000001u, conn->direct_path.reflexive_addr);
    EXPECT_EQ(9090u, conn->direct_path.reflexive_port);
    meridian_conn_destroy(conn);
    meridian_node_destroy(peer);
}

TEST_F(ConnTest, Disconnect) {
    meridian_node_t* peer = meridian_node_create(0xC0A80001, 8080, NULL);
    meridian_conn_t* conn = meridian_conn_create(peer, NULL, MERIDIAN_NAT_TYPE_OPEN, NULL, pool_);
    ASSERT_NE(nullptr, conn);
    meridian_conn_connect(conn);
    meridian_conn_disconnect(conn);
    EXPECT_EQ(MERIDIAN_CONN_STATE_RELAY, meridian_conn_get_state(conn));
    EXPECT_FALSE(meridian_conn_is_direct(conn));
    meridian_conn_destroy(conn);
    meridian_node_destroy(peer);
}

TEST_F(ConnTest, UpgradeToDirectRequiresReflexive) {
    meridian_node_t* peer = meridian_node_create(0xC0A80001, 8080, NULL);
    meridian_conn_t* conn = meridian_conn_create(peer, NULL, MERIDIAN_NAT_TYPE_PORT_RESTRICTED_CONE, NULL, pool_);
    ASSERT_NE(nullptr, conn);
    // Start in RELAY state (no peer address)
    meridian_conn_test_force_state(conn, MERIDIAN_CONN_STATE_RELAY);
    // Upgrade fails without reflexive address
    EXPECT_EQ(-1, meridian_conn_upgrade_to_direct(conn));
    meridian_conn_destroy(conn);
    meridian_node_destroy(peer);
}

// Negative path: with no direct_connection and no relay, the handler's
// direct-path and relay-path branches both fall through, leaving the
// initial -1 in the caller's result slot. Exercises the actor
// wrapper→handler split and the int* result out-param mechanism.
TEST_F(ConnTest, SendNoPath) {
    meridian_node_t* peer = meridian_node_create(0xC0A80001, 8080, NULL);
    meridian_conn_t* conn = meridian_conn_create(peer, NULL, MERIDIAN_NAT_TYPE_OPEN, NULL, pool_);
    ASSERT_NE(nullptr, conn);

    // Neither direct_path.active nor relay_path.active is set without a
    // connect() call and a live relay — both handler branches are no-ops.
    const uint8_t data[] = {0x01, 0x02, 0x03, 0x04};
    EXPECT_EQ(-1, meridian_conn_send(conn, data, sizeof(data)));

    // Idempotency: a second send through the same actor still returns -1
    // without leaking or mutating the connection.
    EXPECT_EQ(-1, meridian_conn_send(conn, data, sizeof(data)));

    // Queries still work post-dispatch — confirms actor-thread access did
    // not corrupt conn's mutable state. Freshly created with OPEN NAT, no
    // paths are active yet, so neither direct nor relay is asserted.
    EXPECT_EQ(MERIDIAN_CONN_STATE_TRYING_DIRECT, meridian_conn_get_state(conn));
    EXPECT_FALSE(meridian_conn_is_direct(conn));
    EXPECT_FALSE(meridian_conn_is_relay(conn));

    meridian_conn_destroy(conn);
    meridian_node_destroy(peer);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
