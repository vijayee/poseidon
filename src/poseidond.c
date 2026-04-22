//
// Created by victor on 4/22/26.
//

#include "ClientAPIs/transport.h"
#include "Channel/channel_manager.h"
#include "Network/Meridian/msquic_singleton.h"
#include "Workers/pool.h"
#include "Time/wheel.h"
#include "Crypto/key_pair.h"
#include "Util/log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <getopt.h>

#define DEFAULT_UNIX_SOCKET "/var/run/poseidond.sock"
#define DEFAULT_TCP_PORT 9090
#define DEFAULT_WS_PORT 9091
#define DEFAULT_QUIC_PORT 9092
#define DEFAULT_DIAL_PORT 8000
#define DEFAULT_PORT_RANGE_START 8001
#define DEFAULT_PORT_RANGE_END 8100

static volatile bool g_running = true;
static poseidon_transport_t* g_unix_transport = NULL;

static void signal_handler(int signum) {
    (void)signum;
    g_running = false;
}

static void print_usage(const char* program) {
    printf("Usage: %s [OPTIONS]\n\n", program);
    printf("Poseidon daemon - decentralized pub/sub overlay network\n\n");
    printf("Options:\n");
    printf("  --enable-unix          Enable Unix domain socket transport (default)\n");
    printf("  --enable-tcp           Enable TCP transport\n");
    printf("  --enable-ws            Enable WebSocket transport\n");
    printf("  --enable-quic          Enable QUIC transport\n");
    printf("  --dial-port PORT       Dial channel port (default: %d)\n", DEFAULT_DIAL_PORT);
    printf("  --port-range-start PORT  Start of data channel port range (default: %d)\n", DEFAULT_PORT_RANGE_START);
    printf("  --port-range-end PORT    End of data channel port range (default: %d)\n", DEFAULT_PORT_RANGE_END);
    printf("  --unix-socket PATH     Unix socket path (default: %s)\n", DEFAULT_UNIX_SOCKET);
    printf("  --tcp-port PORT        TCP listen port (default: %d)\n", DEFAULT_TCP_PORT);
    printf("  --ws-port PORT          WebSocket listen port (default: %d)\n", DEFAULT_WS_PORT);
    printf("  --quic-port PORT       QUIC listen port (default: %d)\n", DEFAULT_QUIC_PORT);
    printf("  --tls-cert PATH        TLS certificate path\n");
    printf("  --tls-key PATH         TLS private key path\n");
    printf("  -h, --help             Show this help message\n");
}

int main(int argc, char* argv[]) {
    poseidon_transport_config_t config = poseidon_transport_config_defaults();
    uint16_t dial_port = DEFAULT_DIAL_PORT;
    uint16_t port_range_start = DEFAULT_PORT_RANGE_START;
    uint16_t port_range_end = DEFAULT_PORT_RANGE_END;
    bool show_help = false;

    static struct option long_options[] = {
        {"enable-unix",      no_argument,       0, 'U'},
        {"enable-tcp",       no_argument,       0, 'T'},
        {"enable-ws",        no_argument,       0, 'W'},
        {"enable-quic",      no_argument,       0, 'Q'},
        {"dial-port",        required_argument, 0, 'd'},
        {"port-range-start", required_argument, 0, 's'},
        {"port-range-end",   required_argument, 0, 'e'},
        {"unix-socket",      required_argument, 0, 'u'},
        {"tcp-port",         required_argument, 0, 't'},
        {"ws-port",          required_argument, 0, 'w'},
        {"quic-port",        required_argument, 0, 'q'},
        {"tls-cert",         required_argument, 0, 'c'},
        {"tls-key",          required_argument, 0, 'k'},
        {"help",             no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    int option_index = 0;
    while ((opt = getopt_long(argc, argv, "h", long_options, &option_index)) != -1) {
        switch (opt) {
        case 'U': config.enable_unix = true; break;
        case 'T': config.enable_tcp = true; break;
        case 'W': config.enable_ws = true; break;
        case 'Q': config.enable_quic = true; break;
        case 'd': dial_port = (uint16_t)atoi(optarg); break;
        case 's': port_range_start = (uint16_t)atoi(optarg); break;
        case 'e': port_range_end = (uint16_t)atoi(optarg); break;
        case 'u': config.unix_socket_path = optarg; break;
        case 't': config.tcp_port = (uint16_t)atoi(optarg); break;
        case 'w': config.ws_port = (uint16_t)atoi(optarg); break;
        case 'q': config.quic_port = (uint16_t)atoi(optarg); break;
        case 'c': config.tls_cert_path = optarg; break;
        case 'k': config.tls_key_path = optarg; break;
        case 'h': show_help = true; break;
        default:
            print_usage(argv[0]);
            return EXIT_FAILURE;
        }
    }

    if (show_help) {
        print_usage(argv[0]);
        return EXIT_SUCCESS;
    }

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    log_set_level(LOG_LEVEL_INFO);
    log_info("poseidond: starting...");

    // Initialize msquic singleton
    const struct QUIC_API_TABLE* msquic = poseidon_msquic_open();
    if (msquic == NULL) {
        log_error("poseidond: failed to open msquic");
        return EXIT_FAILURE;
    }

    // Create work pool and timing wheel
    work_pool_t* pool = work_pool_create(platform_core_count());
    work_pool_launch(pool);
    hierarchical_timing_wheel_t* wheel = hierarchical_timing_wheel_create(8, pool);
    hierarchical_timing_wheel_run(wheel);

    // Create dial channel key pair
    poseidon_key_pair_t* dial_key = poseidon_key_pair_create("ED25519");
    if (dial_key == NULL) {
        log_error("poseidond: failed to create dial key pair");
        poseidon_msquic_close();
        work_pool_shutdown(pool);
        work_pool_join_all(pool);
        work_pool_destroy(pool);
        hierarchical_timing_wheel_destroy(wheel);
        return EXIT_FAILURE;
    }

    // Create channel manager
    poseidon_channel_manager_t* mgr = poseidon_channel_manager_create(
        dial_key, dial_port, port_range_start, port_range_end, pool, wheel);
    if (mgr == NULL) {
        log_error("poseidond: failed to create channel manager");
        poseidon_key_pair_destroy(dial_key);
        poseidon_msquic_close();
        work_pool_shutdown(pool);
        work_pool_join_all(pool);
        work_pool_destroy(pool);
        hierarchical_timing_wheel_destroy(wheel);
        return EXIT_FAILURE;
    }

    // Start enabled transports
    poseidon_transport_t* g_tcp_transport = NULL;
    poseidon_transport_t* g_ws_transport = NULL;
    poseidon_transport_t* g_quic_transport = NULL;

    if (config.enable_unix) {
        g_unix_transport = poseidon_transport_unix_create(config.unix_socket_path, mgr);
        if (g_unix_transport != NULL) {
            if (g_unix_transport->start(g_unix_transport) == 0) {
                log_info("poseidond: Unix transport listening on %s", config.unix_socket_path);
            } else {
                log_error("poseidond: failed to start Unix transport");
                poseidon_transport_destroy(g_unix_transport);
                g_unix_transport = NULL;
            }
        } else {
            log_error("poseidond: failed to create Unix transport");
        }
    }

    if (config.enable_tcp) {
        if (config.tls_cert_path == NULL || config.tls_key_path == NULL) {
            log_error("poseidond: TCP transport requires --tls-cert and --tls-key");
        } else {
            g_tcp_transport = poseidon_transport_tcp_create(
                config.tcp_port, config.tls_cert_path, config.tls_key_path, mgr);
            if (g_tcp_transport != NULL) {
                if (g_tcp_transport->start(g_tcp_transport) == 0) {
                    log_info("poseidond: TCP transport listening on port %u", config.tcp_port);
                } else {
                    log_error("poseidond: failed to start TCP transport");
                    poseidon_transport_destroy(g_tcp_transport);
                    g_tcp_transport = NULL;
                }
            } else {
                log_error("poseidond: failed to create TCP transport");
            }
        }
    }

    if (config.enable_ws) {
        if (config.tls_cert_path == NULL || config.tls_key_path == NULL) {
            log_error("poseidond: WebSocket transport requires --tls-cert and --tls-key");
        } else {
            g_ws_transport = poseidon_transport_ws_create(
                config.ws_port, config.tls_cert_path, config.tls_key_path, mgr);
            if (g_ws_transport != NULL) {
                if (g_ws_transport->start(g_ws_transport) == 0) {
                    log_info("poseidond: WebSocket transport listening on port %u", config.ws_port);
                } else {
                    log_error("poseidond: failed to start WebSocket transport");
                    poseidon_transport_destroy(g_ws_transport);
                    g_ws_transport = NULL;
                }
            } else {
                log_error("poseidond: failed to create WebSocket transport");
            }
        }
    }

    if (config.enable_quic) {
        g_quic_transport = poseidon_transport_quic_create(config.quic_port, mgr);
        if (g_quic_transport != NULL) {
            if (g_quic_transport->start(g_quic_transport) == 0) {
                log_info("poseidond: QUIC transport listening on port %u", config.quic_port);
            } else {
                log_warn("poseidond: QUIC transport not yet implemented");
                poseidon_transport_destroy(g_quic_transport);
                g_quic_transport = NULL;
            }
        }
    }

    log_info("poseidond: running (press Ctrl+C to stop)");

    // Main loop — tick channels and gossip
    while (g_running) {
        platform_usleep(100000);
        poseidon_channel_manager_tick_all(mgr);
        poseidon_channel_manager_gossip_all(mgr);
    }

    log_info("poseidond: shutting down...");

    // Stop transports
    if (g_unix_transport != NULL) {
        g_unix_transport->stop(g_unix_transport);
        poseidon_transport_destroy(g_unix_transport);
    }
    if (g_tcp_transport != NULL) {
        g_tcp_transport->stop(g_tcp_transport);
        poseidon_transport_destroy(g_tcp_transport);
    }
    if (g_ws_transport != NULL) {
        g_ws_transport->stop(g_ws_transport);
        poseidon_transport_destroy(g_ws_transport);
    }
    if (g_quic_transport != NULL) {
        g_quic_transport->stop(g_quic_transport);
        poseidon_transport_destroy(g_quic_transport);
    }

    // Cleanup
    poseidon_channel_manager_destroy(mgr);
    poseidon_key_pair_destroy(dial_key);
    hierarchical_timing_wheel_wait_for_idle_signal(wheel);
    hierarchical_timing_wheel_stop(wheel);
    work_pool_shutdown(pool);
    work_pool_join_all(pool);
    work_pool_destroy(pool);
    hierarchical_timing_wheel_destroy(wheel);
    poseidon_msquic_close();

    log_info("poseidond: stopped");
    return EXIT_SUCCESS;
}