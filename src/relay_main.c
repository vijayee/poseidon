//
// Created by victor on 4/19/26.
//

#include "Network/Meridian/meridian_relay_server.h"
#include "Util/log.h"
#include "Util/allocator.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <getopt.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

// ============================================================================
// DEFAULT CONFIGURATION
// ============================================================================

#define DEFAULT_RELAY_PORT 9000
#define DEFAULT_MAX_CLIENTS 256
#define DEFAULT_IDLE_TIMEOUT_MS 30000
#define DEFAULT_KEEPALIVE_MS 10000
#define DEFAULT_MAX_DATAGRAM_SIZE 1400

// ============================================================================
// GLOBAL STATE
// ============================================================================

static meridian_relay_server_t* g_server = NULL;
static volatile bool g_running = true;

// ============================================================================
// SIGNAL HANDLING
// ============================================================================

static void signal_handler(int signum) {
    (void)signum;
    g_running = false;

    if (g_server != NULL) {
        meridian_relay_server_stop(g_server);
    }
}

// ============================================================================
// COMMAND LINE OPTIONS
// ============================================================================

typedef struct {
    uint16_t port;
    uint32_t max_clients;
    uint32_t idle_timeout_ms;
    uint32_t keepalive_ms;
    uint32_t max_datagram_size;
    bool help;
    bool version;
} relay_options_t;

static void print_usage(const char* program_name) {
    printf("Usage: %s [OPTIONS]\n\n", program_name);
    printf("Meridian Relay Server - QUIC-based relay for Meridian protocol\n\n");
    printf("Options:\n");
    printf("  -p, --port PORT             Relay server port (default: %u)\n", DEFAULT_RELAY_PORT);
    printf("  -c, --max-clients NUM      Maximum concurrent clients (default: %u)\n", DEFAULT_MAX_CLIENTS);
    printf("  -t, --idle-timeout MS      Connection idle timeout (default: %u ms)\n", DEFAULT_IDLE_TIMEOUT_MS);
    printf("  -k, --keepalive MS         Keep-alive interval (default: %u ms)\n", DEFAULT_KEEPALIVE_MS);
    printf("  -d, --max-datagram SIZE    Maximum datagram size (default: %u bytes)\n", DEFAULT_MAX_DATAGRAM_SIZE);
    printf("  -h, --help                 Show this help message\n");
    printf("  -v, --version              Show version information\n");
    printf("\n");
    printf("Example:\n");
    printf("  %s -p 9000 -c 256\n", program_name);
}

static void print_version(void) {
    printf("meridian_relay v0.1.0\n");
    printf("Meridian Relay Server\n");
}

static int parse_options(int argc, char* argv[], relay_options_t* opts) {
    if (opts == NULL) return -1;

    memset(opts, 0, sizeof(*opts));
    opts->port = DEFAULT_RELAY_PORT;
    opts->max_clients = DEFAULT_MAX_CLIENTS;
    opts->idle_timeout_ms = DEFAULT_IDLE_TIMEOUT_MS;
    opts->keepalive_ms = DEFAULT_KEEPALIVE_MS;
    opts->max_datagram_size = DEFAULT_MAX_DATAGRAM_SIZE;

    static struct option long_options[] = {
        { "port",           required_argument, 0, 'p' },
        { "max-clients",    required_argument, 0, 'c' },
        { "idle-timeout",   required_argument, 0, 't' },
        { "keepalive",      required_argument, 0, 'k' },
        { "max-datagram",   required_argument, 0, 'd' },
        { "help",           no_argument,       0, 'h' },
        { "version",        no_argument,       0, 'v' },
        { 0, 0, 0, 0 }
    };

    int opt;
    int option_index = 0;

    while ((opt = getopt_long(argc, argv, "p:c:t:k:d:hv", long_options, &option_index)) != -1) {
        switch (opt) {
        case 'p':
            opts->port = (uint16_t)atoi(optarg);
            break;
        case 'c':
            opts->max_clients = (uint32_t)atoi(optarg);
            break;
        case 't':
            opts->idle_timeout_ms = (uint32_t)atoi(optarg);
            break;
        case 'k':
            opts->keepalive_ms = (uint32_t)atoi(optarg);
            break;
        case 'd':
            opts->max_datagram_size = (uint32_t)atoi(optarg);
            break;
        case 'h':
            opts->help = true;
            break;
        case 'v':
            opts->version = true;
            break;
        default:
            return -1;
        }
    }

    return 0;
}

// ============================================================================
// SERVER MANAGEMENT
// ============================================================================

static int start_relay_server(const relay_options_t* opts) {
    if (opts == NULL) return -1;

    // Initialize msquic
    const struct QUIC_API_TABLE* msquic = NULL;
    if (QUIC_FAILED(MsQuicOpen2(&msquic))) {
        log_error("Failed to open msquic library");
        return -1;
    }

    // Configure server
    meridian_relay_server_config_t config = {
        .alpn = "meridian_relay",
        .listen_port = opts->port,
        .idle_timeout_ms = opts->idle_timeout_ms,
        .keepalive_interval_ms = opts->keepalive_ms,
        .max_datagram_size = opts->max_datagram_size
    };

    // Create server
    g_server = meridian_relay_server_create(msquic, &config);
    if (g_server == NULL) {
        log_error("Failed to create relay server");
        MsQuicClose(msquic);
        return -1;
    }

    // Start server
    if (meridian_relay_server_start(g_server) != 0) {
        log_error("Failed to start relay server on port %u", opts->port);
        meridian_relay_server_destroy(g_server);
        g_server = NULL;
        MsQuicClose(msquic);
        return -1;
    }

    log_info("Relay server listening on port %u", opts->port);
    return 0;
}

static void print_stats(void) {
    if (g_server == NULL) return;

    meridian_relay_server_stats_t stats;
    if (meridian_relay_server_get_stats(g_server, &stats) == 0) {
        printf("Clients: %zu\n", stats.num_clients);
        printf("Datagrams forwarded: %lu\n", (unsigned long)stats.datagrams_forwarded);
        printf("Datagrams dropped: %lu\n", (unsigned long)stats.datagrams_dropped);
        printf("Address requests: %lu\n", (unsigned long)stats.addr_requests_served);
    }
}

// ============================================================================
// MAIN
// ============================================================================

int main(int argc, char* argv[]) {
    relay_options_t opts;

    // Parse command line
    if (parse_options(argc, argv, &opts) != 0) {
        fprintf(stderr, "Error: Invalid command line options\n");
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    if (opts.help) {
        print_usage(argv[0]);
        return EXIT_SUCCESS;
    }

    if (opts.version) {
        print_version();
        return EXIT_SUCCESS;
    }

    // Setup signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    log_set_level(LOG_LEVEL_INFO);

    // Start server
    if (start_relay_server(&opts) != 0) {
        log_error("Failed to start relay server");
        return EXIT_FAILURE;
    }

    printf("Meridian Relay Server v0.1.0\n");
    printf("Listening on port %u\n", opts.port);
    printf("Press Ctrl+C to stop\n\n");

    // Main loop
    while (g_running) {
        sleep(1);

        // Print periodic stats
        static int tick = 0;
        tick++;
        if (tick >= 10) {
            tick = 0;
            printf("\n--- Server Stats ---\n");
            print_stats();
            printf("--------------------\n\n");
        }
    }

    // Cleanup
    printf("\nShutting down...\n");

    if (g_server != NULL) {
        meridian_relay_server_destroy(g_server);
        g_server = NULL;
    }

    return EXIT_SUCCESS;
}
