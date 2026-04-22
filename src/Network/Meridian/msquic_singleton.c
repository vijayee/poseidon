//
// Created by victor on 4/21/26.
//

#include "msquic_singleton.h"
#include "../../Util/threadding.h"
#include "../../Util/log.h"
#include <stdlib.h>

static const struct QUIC_API_TABLE* g_msquic = NULL;
static uint32_t g_msquic_refcount = 0;
static bool g_msquic_lock_initialized = false;
static PLATFORMLOCKTYPE(g_msquic_lock);

static void ensure_lock_initialized(void) {
    if (!g_msquic_lock_initialized) {
        platform_lock_init(&g_msquic_lock);
        g_msquic_lock_initialized = true;
    }
}

const struct QUIC_API_TABLE* poseidon_msquic_open(void) {
    ensure_lock_initialized();
    platform_lock(&g_msquic_lock);

    if (g_msquic == NULL) {
        QUIC_STATUS Status;
        const struct QUIC_API_TABLE* table = NULL;
        if (QUIC_FAILED(Status = MsQuicOpen2(&table))) {
            platform_unlock(&g_msquic_lock);
            log_error("MsQuicOpen2 failed: 0x%x", Status);
            return NULL;
        }
        g_msquic = table;
        g_msquic_refcount = 1;
        platform_unlock(&g_msquic_lock);
        return g_msquic;
    }

    g_msquic_refcount++;
    const struct QUIC_API_TABLE* result = g_msquic;
    platform_unlock(&g_msquic_lock);
    return result;
}

void poseidon_msquic_close(void) {
    ensure_lock_initialized();
    platform_lock(&g_msquic_lock);

    if (g_msquic == NULL || g_msquic_refcount == 0) {
        platform_unlock(&g_msquic_lock);
        return;
    }

    g_msquic_refcount--;
    if (g_msquic_refcount == 0) {
        MsQuicClose(g_msquic);
        g_msquic = NULL;
    }

    platform_unlock(&g_msquic_lock);
}