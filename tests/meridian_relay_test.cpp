#include <gtest/gtest.h>
#include <cbor.h>
#include "Network/Meridian/meridian_relay.h"
#include "Network/Meridian/meridian_relay_server.h"
#include "Network/Meridian/meridian_packet.h"
#include "Platform/poseidon_msquic.h"
#include "Crypto/key_pair.h"

extern "C" {
#include "Scheduler/scheduler.h"
}

class RelayTest : public ::testing::Test {
protected:
    void SetUp() override {}
    void TearDown() override {}
};

TEST_F(RelayTest, CreateDestroyClientWithoutMsquic) {
    // Without msquic, create returns NULL — verify the null check works
    meridian_rendv_t* server = meridian_rendv_create(0x01020304, 8080);
    meridian_relay_t* relay = meridian_relay_create(NULL, NULL, server, NULL, NULL, NULL);
    EXPECT_EQ(nullptr, relay);
    meridian_rendv_destroy(server);
}

TEST_F(RelayTest, CreateDestroyClientWithNullServer) {
    // Without server, create returns NULL
    meridian_relay_t* relay = meridian_relay_create(NULL, NULL, NULL, NULL, NULL, NULL);
    EXPECT_EQ(nullptr, relay);
}

TEST_F(RelayTest, ServerCreateDestroyWithoutMsquic) {
    // Without msquic, server create returns NULL — verify the null check works
    meridian_relay_server_config_t config = {
        .alpn = "meridian-relay",
        .listen_port = 8080,
        .idle_timeout_ms = 30000,
        .keepalive_interval_ms = 5000,
        .max_datagram_size = 1400
    };
    meridian_relay_server_t* server = meridian_relay_server_create(NULL, &config, nullptr);
    EXPECT_EQ(nullptr, server);
}

TEST_F(RelayTest, ClientNotConnectedInitially) {
    // Create relay with a server address but no msquic — it should fail
    // Instead, test the is_connected and get_endpoint_id with NULL
    EXPECT_FALSE(meridian_relay_is_connected(NULL));
    EXPECT_EQ(0u, meridian_relay_get_endpoint_id(NULL));
}

TEST_F(RelayTest, AddrResponseCreateDestroy) {
    meridian_addr_response_t* resp = meridian_addr_response_create(12345, 0xC0A80001, 8080, 42);
    ASSERT_NE(nullptr, resp);
    EXPECT_EQ(MERIDIAN_PACKET_TYPE_ADDR_RESPONSE, resp->type);
    EXPECT_EQ(12345u, resp->query_id);
    EXPECT_EQ(0xC0A80001u, resp->reflexive_addr);
    EXPECT_EQ(8080u, resp->reflexive_port);
    EXPECT_EQ(42u, resp->endpoint_id);
    meridian_addr_response_destroy(resp);
}

TEST_F(RelayTest, AddrResponseEncodeDecode) {
    meridian_addr_response_t* resp = meridian_addr_response_create(99, 0x0A000001, 443, 7);
    cbor_item_t* encoded = meridian_addr_response_encode(resp);
    ASSERT_NE(nullptr, encoded);
    meridian_addr_response_t* decoded = meridian_addr_response_decode(encoded);
    ASSERT_NE(nullptr, decoded);
    EXPECT_EQ(resp->reflexive_addr, decoded->reflexive_addr);
    EXPECT_EQ(resp->reflexive_port, decoded->reflexive_port);
    EXPECT_EQ(resp->endpoint_id, decoded->endpoint_id);
    meridian_addr_response_destroy(decoded);
    cbor_decref(&encoded);
    meridian_addr_response_destroy(resp);
}

TEST_F(RelayTest, PunchRequestCreateEncodeDecode) {
    meridian_punch_request_t* req = meridian_punch_request_create(1, 100, 0xC0A80001, 8080);
    ASSERT_NE(nullptr, req);
    cbor_item_t* encoded = meridian_punch_request_encode(req);
    ASSERT_NE(nullptr, encoded);
    meridian_punch_request_t* decoded = meridian_punch_request_decode(encoded);
    ASSERT_NE(nullptr, decoded);
    EXPECT_EQ(req->from_endpoint_id, decoded->from_endpoint_id);
    EXPECT_EQ(req->target_addr, decoded->target_addr);
    EXPECT_EQ(req->target_port, decoded->target_port);
    meridian_punch_request_destroy(decoded);
    cbor_decref(&encoded);
    meridian_punch_request_destroy(req);
}

TEST_F(RelayTest, PunchSyncCreateEncodeDecode) {
    meridian_punch_sync_t* sync = meridian_punch_sync_create(2, 200, 0x0A000002, 9090);
    ASSERT_NE(nullptr, sync);
    cbor_item_t* encoded = meridian_punch_sync_encode(sync);
    ASSERT_NE(nullptr, encoded);
    meridian_punch_sync_t* decoded = meridian_punch_sync_decode(encoded);
    ASSERT_NE(nullptr, decoded);
    EXPECT_EQ(sync->from_endpoint_id, decoded->from_endpoint_id);
    EXPECT_EQ(sync->from_addr, decoded->from_addr);
    EXPECT_EQ(sync->from_port, decoded->from_port);
    meridian_punch_sync_destroy(decoded);
    cbor_decref(&encoded);
    meridian_punch_sync_destroy(sync);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

// The server's lifecycle: start, stop, destroy.
//
// The stop path is the one that changed with upstream's relay server: it
// snapshots the connections under the lock, shuts them down outside it,
// and waits for the SHUTDOWN_COMPLETE callbacks to quiesce before the
// table goes away (liboffs/src/Network/Relay/relay_server.c).
TEST_F(RelayTest, ServerStartStopDestroy) {
    // MsQuic v2 does not let a server load a NONE credential, so the
    // server needs a certificate.
    poseidon_key_pair_t* key_pair = poseidon_key_pair_create("ED25519");
    ASSERT_NE(nullptr, key_pair);
    char key_path[256];
    char cert_path[256];
    snprintf(key_path, sizeof(key_path), "/tmp/poseidon_relay_test_key.pem");
    snprintf(cert_path, sizeof(cert_path), "/tmp/poseidon_relay_test_cert.pem");
    ASSERT_EQ(0, poseidon_key_pair_generate_tls_files(key_pair, "relay-test",
                                                       key_path, cert_path));

    const struct QUIC_API_TABLE* msquic = poseidon_msquic_open();
    ASSERT_NE(nullptr, msquic);

    scheduler_pool_t* sched = scheduler_pool_create(2);
    ASSERT_NE(nullptr, sched);
    scheduler_pool_start(sched);

    meridian_relay_server_config_t config = {
        .alpn = "meridian-relay-test",
        .listen_port = 0,          /* the OS picks a port */
        .idle_timeout_ms = 30000,
        .keepalive_interval_ms = 5000,
        .max_datagram_size = 1400,
        .tls_key_path = key_path,
        .tls_cert_path = cert_path
    };

    meridian_relay_server_t* server =
        meridian_relay_server_create(msquic, &config, sched);
    ASSERT_NE(nullptr, server);

    EXPECT_EQ(0, meridian_relay_server_start(server));

    meridian_relay_server_stats_t stats;
    EXPECT_EQ(0, meridian_relay_server_get_stats(server, &stats));
    EXPECT_EQ(0u, stats.num_clients);

    EXPECT_EQ(0, meridian_relay_server_stop(server));

    meridian_relay_server_destroy(server);
    scheduler_pool_stop(sched);
    scheduler_pool_destroy(sched);
    poseidon_key_pair_destroy(key_pair);
    unlink(key_path);
    unlink(cert_path);
    poseidon_msquic_close();
}
