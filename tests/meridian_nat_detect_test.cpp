#include <gtest/gtest.h>
#include "Network/Meridian/meridian_rendv.h"

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

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
