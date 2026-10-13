//
// Created by victor on 4/21/26.
//

#include "msquic_singleton.h"
#include "../../Platform/platform_thread.h"
#include "../../Util/log.h"
#include <stdlib.h>
#include <pthread.h>

static const struct QUIC_API_TABLE* g_msquic = NULL;
static uint32_t g_msquic_refcount = 0;
static platform_mutex_t* g_msquic_lock = NULL;
static pthread_once_t g_msquic_lock_once = PTHREAD_ONCE_INIT;

static void init_msquic_lock(void) {
    g_msquic_lock = platform_mutex_create();
}

const struct QUIC_API_TABLE* poseidon_msquic_open(void) {
    pthread_once(&g_msquic_lock_once, init_msquic_lock);
    platform_mutex_lock(g_msquic_lock);

    if (g_msquic == NULL) {
        QUIC_STATUS Status;
        const struct QUIC_API_TABLE* table = NULL;
        if (QUIC_FAILED(Status = MsQuicOpen2(&table))) {
            platform_mutex_unlock(g_msquic_lock);
            log_error("MsQuicOpen2 failed: 0x%x", Status);
            return NULL;
        }
        g_msquic = table;
        g_msquic_refcount = 1;
        platform_mutex_unlock(g_msquic_lock);
        return g_msquic;
    }

    g_msquic_refcount++;
    const struct QUIC_API_TABLE* result = g_msquic;
    platform_mutex_unlock(g_msquic_lock);
    return result;
}

void poseidon_msquic_close(void) {
    pthread_once(&g_msquic_lock_once, init_msquic_lock);
    platform_mutex_lock(g_msquic_lock);

    if (g_msquic == NULL || g_msquic_refcount == 0) {
        platform_mutex_unlock(g_msquic_lock);
        return;
    }

    g_msquic_refcount--;
    if (g_msquic_refcount == 0) {
        MsQuicClose(g_msquic);
        g_msquic = NULL;
    }

    platform_mutex_unlock(g_msquic_lock);
}