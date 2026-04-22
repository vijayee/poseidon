//
// Created by victor on 4/22/26.
//

#include <gtest/gtest.h>
#include "ClientAPIs/client_session.h"

class ClientSessionTest : public ::testing::Test {
protected:
    void SetUp() override {}
    void TearDown() override {}
};

TEST_F(ClientSessionTest, CreateDestroy) {
    client_session_t* session = client_session_create(123, nullptr);
    ASSERT_NE(nullptr, session);
    EXPECT_EQ(123u, session->session_id);
    EXPECT_EQ(-1, session->client_fd);
    EXPECT_EQ(nullptr, session->manager);
    EXPECT_EQ(0u, session->num_subscriptions);
    EXPECT_EQ(1u, session->next_request_id);

    client_session_destroy(session);
}

TEST_F(ClientSessionTest, SubscribeUnsubscribe) {
    client_session_t* session = client_session_create(1, nullptr);
    ASSERT_NE(nullptr, session);

    EXPECT_EQ(0, client_session_subscribe(session, "Feeds/public"));
    EXPECT_EQ(1u, session->num_subscriptions);
    EXPECT_TRUE(session->subscriptions[0].active);
    EXPECT_STREQ("Feeds/public", session->subscriptions[0].topic_path);

    // Duplicate subscribe is idempotent
    EXPECT_EQ(0, client_session_subscribe(session, "Feeds/public"));
    EXPECT_EQ(1u, session->num_subscriptions);

    EXPECT_EQ(0, client_session_subscribe(session, "Feeds/private"));
    EXPECT_EQ(2u, session->num_subscriptions);

    EXPECT_EQ(0, client_session_unsubscribe(session, "Feeds/public"));
    EXPECT_EQ(1u, session->num_subscriptions);
    EXPECT_STREQ("Feeds/private", session->subscriptions[0].topic_path);

    // Unsubscribe unknown topic fails
    EXPECT_EQ(-1, client_session_unsubscribe(session, "Unknown"));
    EXPECT_EQ(1u, session->num_subscriptions);

    client_session_destroy(session);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
