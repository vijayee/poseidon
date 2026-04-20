//
// Created by victor on 4/20/26.
//

#include "quasar_message_id.h"
#include "../../Util/threadding.h"
#include <time.h>
#include <string.h>
#include <stdatomic.h>
#include <arpa/inet.h>

// Global atomic counter for same-timestamp uniqueness
static atomic_uint_fast64_t g_message_id_count = ATOMIC_VAR_INIT(0);

// One-time initialization control
#if _WIN32
static INIT_ONCE g_init_once = INIT_ONCE_STATIC_INIT;
#else
static pthread_once_t g_init_once = PTHREAD_ONCE_INIT;
#endif

static void quasar_message_id_do_init(void) {
    // Counter is already initialized to 0 via ATOMIC_VAR_INIT
}

void quasar_message_id_init(void) {
#if _WIN32
    InitOnceExecuteOnce(&g_init_once, quasar_message_id_do_init, NULL, NULL);
#else
    pthread_once(&g_init_once, quasar_message_id_do_init);
#endif
}

quasar_message_id_t quasar_message_id_get_next(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);

    uint64_t count = atomic_fetch_add(&g_message_id_count, 1);

    quasar_message_id_t next = {
        .time = (uint64_t)ts.tv_sec,
        .nanos = (uint64_t)ts.tv_nsec,
        .count = count
    };

    return next;
}

int quasar_message_id_compare(const quasar_message_id_t* a, const quasar_message_id_t* b) {
    if (a->time > b->time) return 1;
    if (a->time < b->time) return -1;
    if (a->nanos > b->nanos) return 1;
    if (a->nanos < b->nanos) return -1;
    if (a->count > b->count) return 1;
    if (a->count < b->count) return -1;
    return 0;
}

static void write_uint64(uint8_t* buf, uint64_t val) {
    uint32_t high = htonl((uint32_t)(val >> 32));
    uint32_t low = htonl((uint32_t)(val & 0xFFFFFFFF));
    memcpy(buf, &high, sizeof(uint32_t));
    memcpy(buf + 4, &low, sizeof(uint32_t));
}

static uint64_t read_uint64(const uint8_t* buf) {
    uint32_t high, low;
    memcpy(&high, buf, sizeof(uint32_t));
    memcpy(&low, buf + 4, sizeof(uint32_t));
    return ((uint64_t)ntohl(high) << 32) | (uint64_t)ntohl(low);
}

void quasar_message_id_serialize(const quasar_message_id_t* id, uint8_t* buf) {
    write_uint64(buf, id->time);
    write_uint64(buf + 8, id->nanos);
    write_uint64(buf + 16, id->count);
}

void quasar_message_id_deserialize(quasar_message_id_t* id, const uint8_t* buf) {
    id->time = read_uint64(buf);
    id->nanos = read_uint64(buf + 8);
    id->count = read_uint64(buf + 16);
}