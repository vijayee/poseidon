#include <gtest/gtest.h>
#include "Network/Meridian/meridian_rendv.h"

extern "C" {
#include "Scheduler/scheduler.h"
}

class NatDetectTest : public ::testing::Test {
protected:
    void SetUp() override {}
    void TearDown() override {}
};

TEST_F(NatDetectTest, OpenNatType) {
    meridian_rendv_t* rendv = meridian_rendv_create(0xC0A80001, 8080);
    ASSERT_NE(nullptr, rendv);
    meridian_rendv_set_nat_type(rendv, MERIDIAN_NAT_TYPE_OPEN);
    EXPECT_EQ(MERIDIAN_NAT_TYPE_OPEN, meridian_rendv_get_nat_type(rendv));
    EXPECT_TRUE(meridian_rendv_can_direct_connect(rendv));
    meridian_rendv_destroy(rendv);
}

TEST_F(NatDetectTest, FullConeNatType) {
    meridian_rendv_t* rendv = meridian_rendv_create(0xC0A80001, 8080);
    meridian_rendv_set_nat_type(rendv, MERIDIAN_NAT_TYPE_FULL_CONE);
    EXPECT_TRUE(meridian_rendv_can_direct_connect(rendv));
    meridian_rendv_destroy(rendv);
}

TEST_F(NatDetectTest, RestrictedConeNatType) {
    meridian_rendv_t* rendv = meridian_rendv_create(0xC0A80001, 8080);
    meridian_rendv_set_nat_type(rendv, MERIDIAN_NAT_TYPE_RESTRICTED_CONE);
    EXPECT_TRUE(meridian_rendv_can_direct_connect(rendv));
    meridian_rendv_destroy(rendv);
}

TEST_F(NatDetectTest, PortRestrictedConeNatType) {
    meridian_rendv_t* rendv = meridian_rendv_create(0xC0A80001, 8080);
    meridian_rendv_set_nat_type(rendv, MERIDIAN_NAT_TYPE_PORT_RESTRICTED_CONE);
    EXPECT_TRUE(meridian_rendv_can_direct_connect(rendv));
    meridian_rendv_destroy(rendv);
}

TEST_F(NatDetectTest, SymmetricNatCannotDirectConnect) {
    meridian_rendv_t* rendv = meridian_rendv_create(0xC0A80001, 8080);
    meridian_rendv_set_nat_type(rendv, MERIDIAN_NAT_TYPE_SYMMETRIC);
    EXPECT_FALSE(meridian_rendv_can_direct_connect(rendv));
    meridian_rendv_destroy(rendv);
}

TEST_F(NatDetectTest, UnknownNatTypeCannotDirectConnect) {
    meridian_rendv_t* rendv = meridian_rendv_create(0xC0A80001, 8080);
    meridian_rendv_set_nat_type(rendv, MERIDIAN_NAT_TYPE_UNKNOWN);
    EXPECT_FALSE(meridian_rendv_can_direct_connect(rendv));
    meridian_rendv_destroy(rendv);
}

TEST_F(NatDetectTest, ReflexiveAddress) {
    meridian_rendv_t* rendv = meridian_rendv_create(0xC0A80001, 8080);
    EXPECT_EQ(0u, meridian_rendv_get_reflexive_addr(rendv));
    EXPECT_EQ(0u, meridian_rendv_get_reflexive_port(rendv));
    meridian_rendv_set_reflexive_addr(rendv, 0x01020304, 12345);
    EXPECT_EQ(0x01020304u, meridian_rendv_get_reflexive_addr(rendv));
    EXPECT_EQ(12345u, meridian_rendv_get_reflexive_port(rendv));
    meridian_rendv_destroy(rendv);
}

TEST_F(NatDetectTest, NatTypeStringConversion) {
    EXPECT_EQ(MERIDIAN_NAT_TYPE_OPEN, meridian_nat_type_from_string("open"));
    EXPECT_EQ(MERIDIAN_NAT_TYPE_FULL_CONE, meridian_nat_type_from_string("full_cone"));
    EXPECT_EQ(MERIDIAN_NAT_TYPE_SYMMETRIC, meridian_nat_type_from_string("symmetric"));
    EXPECT_EQ(MERIDIAN_NAT_TYPE_UNKNOWN, meridian_nat_type_from_string("unknown"));

    EXPECT_STREQ("open", meridian_nat_type_to_string(MERIDIAN_NAT_TYPE_OPEN));
    EXPECT_STREQ("symmetric", meridian_nat_type_to_string(MERIDIAN_NAT_TYPE_SYMMETRIC));
}

// The classification is upstream's, rule for rule
// (liboffs/src/Network/nat_detect.c, nat_detect_classify). The
// two-relay comparison is the point: one relay cannot tell a full cone
// from a symmetric NAT.
namespace {
constexpr uint32_t LOCAL = 0x0A000001;    // the node's own address
constexpr uint32_t RELAY_A = 0xC0A80001;  // what relay A reported
constexpr uint32_t RELAY_B = 0xC0A80002;  // what relay B reported
}  // namespace

TEST_F(NatDetectTest, ClassifyNoReportIsUnknown) {
    EXPECT_EQ(MERIDIAN_NAT_TYPE_UNKNOWN,
              meridian_rendv_classify_nat(LOCAL, 0, 0, false, 0, 0, false));
}

TEST_F(NatDetectTest, ClassifyOneRelayOpen) {
    // The one relay saw the node at its own address: no NAT.
    EXPECT_EQ(MERIDIAN_NAT_TYPE_OPEN,
              meridian_rendv_classify_nat(LOCAL, LOCAL, 1234, true, 0, 0, false));
    EXPECT_EQ(MERIDIAN_NAT_TYPE_OPEN,
              meridian_rendv_classify_nat(LOCAL, 0, 0, false, LOCAL, 1234, true));
}

TEST_F(NatDetectTest, ClassifyOneRelayIsPortRestrictedCone) {
    // One relay, and it saw the node at another address: a NAT is in
    // the way, and one relay cannot say more than that.
    EXPECT_EQ(MERIDIAN_NAT_TYPE_PORT_RESTRICTED_CONE,
              meridian_rendv_classify_nat(LOCAL, RELAY_A, 1234, true, 0, 0, false));
    EXPECT_EQ(MERIDIAN_NAT_TYPE_PORT_RESTRICTED_CONE,
              meridian_rendv_classify_nat(LOCAL, 0, 0, false, RELAY_B, 1234, true));
}

TEST_F(NatDetectTest, ClassifyTwoRelaysOpen) {
    EXPECT_EQ(MERIDIAN_NAT_TYPE_OPEN,
              meridian_rendv_classify_nat(LOCAL, LOCAL, 1234, true, RELAY_B, 1234, true));
}

TEST_F(NatDetectTest, ClassifyTwoRelaysAgreeIsFullCone) {
    // The same mapping for two different relays: the NAT maps every
    // destination the same way.
    EXPECT_EQ(MERIDIAN_NAT_TYPE_FULL_CONE,
              meridian_rendv_classify_nat(LOCAL, RELAY_A, 1234, true, RELAY_A, 1234, true));
}

TEST_F(NatDetectTest, ClassifyTwoRelaysDisagreeIsSymmetric) {
    // Different addresses: the mapping depends on the destination.
    EXPECT_EQ(MERIDIAN_NAT_TYPE_SYMMETRIC,
              meridian_rendv_classify_nat(LOCAL, RELAY_A, 1234, true, RELAY_B, 1234, true));
    // The same address with a different port: the port depends on the
    // destination — also a symmetric NAT.
    EXPECT_EQ(MERIDIAN_NAT_TYPE_SYMMETRIC,
              meridian_rendv_classify_nat(LOCAL, RELAY_A, 1234, true, RELAY_A, 5678, true));
}

// The node's own NAT type starts as a cone NAT, the way upstream starts
// it before the detection has answered
// (liboffs/src/Network/network.c:359): only a symmetric NAT forbids a
// direct attempt. A peer point keeps the unknown default — the peer's
// type is not known until the peer says so
// (liboffs/src/Network/peer_connection.c:33).
TEST_F(NatDetectTest, TheLocalPointDefaultsToAConeNat) {
    scheduler_pool_t* sched = scheduler_pool_create(1);
    ASSERT_NE(nullptr, sched);
    scheduler_pool_start(sched);

    meridian_rendv_handle_t* handle = meridian_rendv_handle_create(sched);
    ASSERT_NE(nullptr, handle);

    meridian_rendv_t* local = meridian_rendv_create(0xC0A80001, 8080);
    ASSERT_NE(nullptr, local);
    ASSERT_EQ(0, meridian_rendv_handle_set_local(handle, local));

    EXPECT_EQ(MERIDIAN_NAT_TYPE_PORT_RESTRICTED_CONE,
              meridian_rendv_get_nat_type(local));
    EXPECT_TRUE(meridian_rendv_can_direct_connect(local));

    meridian_rendv_t* peer = meridian_rendv_create(0x0A000001, 9090);
    ASSERT_NE(nullptr, peer);
    EXPECT_EQ(MERIDIAN_NAT_TYPE_UNKNOWN, meridian_rendv_get_nat_type(peer));
    EXPECT_FALSE(meridian_rendv_can_direct_connect(peer));

    meridian_rendv_destroy(peer);
    meridian_rendv_handle_destroy(handle);
    meridian_rendv_destroy(local);
    scheduler_pool_stop(sched);
    scheduler_pool_destroy(sched);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
