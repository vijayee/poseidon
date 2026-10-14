#include "Platform/poseidon_msquic.h"

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "Platform/platform_thread.h"
#include "Util/log.h"
#include "Util/vec.h"

// Process-wide MSQUIC API table. MsQuicOpen2 may only be called once per
// process; we refcount callers around it.
static const struct QUIC_API_TABLE* g_api_table = NULL;
static uint32_t g_api_table_refcount = 0;
static platform_mutex_t* g_api_table_lock = NULL;
static pthread_once_t g_api_table_lock_once = PTHREAD_ONCE_INIT;

// Per-app-identity registrations. RegistrationOpen may be called at most
// once per distinct app name per process; we refcount callers per app name.
typedef struct {
    char* app_name;
    HQUIC reg;
    uint32_t refcount;
} registration_slot_t;

static vec_t(registration_slot_t) g_registrations;
static platform_mutex_t* g_registrations_lock = NULL;
static pthread_once_t g_registrations_lock_once = PTHREAD_ONCE_INIT;

static void init_api_table_lock(void) {
    g_api_table_lock = platform_mutex_create();
}

static void init_registrations_lock(void) {
    g_registrations_lock = platform_mutex_create();
    vec_init(&g_registrations);
}

const struct QUIC_API_TABLE* poseidon_msquic_open(void) {
    pthread_once(&g_api_table_lock_once, init_api_table_lock);
    platform_mutex_lock(g_api_table_lock);

    if (g_api_table_refcount == 0) {
        QUIC_STATUS status = MsQuicOpen2(&g_api_table);
        if (QUIC_FAILED(status)) {
            log_error("poseidon_msquic: MsQuicOpen2 failed: 0x%x", status);
            g_api_table = NULL;
            platform_mutex_unlock(g_api_table_lock);
            return NULL;
        }
    }
    g_api_table_refcount++;

    const struct QUIC_API_TABLE* result = g_api_table;
    platform_mutex_unlock(g_api_table_lock);
    return result;
}

void poseidon_msquic_close(void) {
    if (g_api_table_lock == NULL) {
        return;
    }
    platform_mutex_lock(g_api_table_lock);

    if (g_api_table_refcount == 0) {
        // Defensive: callers may have already closed out.
        platform_mutex_unlock(g_api_table_lock);
        return;
    }
    g_api_table_refcount--;

    if (g_api_table_refcount == 0 && g_api_table != NULL) {
        MsQuicClose(g_api_table);
        g_api_table = NULL;
    }

    platform_mutex_unlock(g_api_table_lock);
}

static registration_slot_t* find_registration_slot(const char* app_name) {
    for (int i = 0; i < g_registrations.length; i++) {
        if (strcmp(g_registrations.data[i].app_name, app_name) == 0) {
            return &g_registrations.data[i];
        }
    }
    return NULL;
}

bool poseidon_msquic_open_registration(const char* app_name, HQUIC* out_reg) {
    if (app_name == NULL || out_reg == NULL) {
        return false;
    }

    const struct QUIC_API_TABLE* api = poseidon_msquic_open();
    if (api == NULL) {
        return false;
    }

    pthread_once(&g_registrations_lock_once, init_registrations_lock);
    platform_mutex_lock(g_registrations_lock);

    registration_slot_t* slot = find_registration_slot(app_name);
    if (slot != NULL) {
        slot->refcount++;
        *out_reg = slot->reg;
        platform_mutex_unlock(g_registrations_lock);
        return true;
    }

    QUIC_REGISTRATION_CONFIG reg_config = {
        .AppName = app_name,
        .ExecutionProfile = QUIC_EXECUTION_PROFILE_LOW_LATENCY,
    };

    HQUIC reg = NULL;
    QUIC_STATUS status = api->RegistrationOpen(&reg_config, &reg);
    if (QUIC_FAILED(status)) {
        log_error("poseidon_msquic: RegistrationOpen(\"%s\") failed: 0x%x",
                  app_name, status);
        platform_mutex_unlock(g_registrations_lock);
        poseidon_msquic_close();
        return false;
    }

    registration_slot_t new_slot;
    new_slot.app_name = strdup(app_name);
    if (new_slot.app_name == NULL) {
        api->RegistrationClose(reg);
        platform_mutex_unlock(g_registrations_lock);
        poseidon_msquic_close();
        return false;
    }
    new_slot.reg = reg;
    new_slot.refcount = 1;

    if (vec_push(&g_registrations, new_slot) != 0) {
        free(new_slot.app_name);
        api->RegistrationClose(reg);
        platform_mutex_unlock(g_registrations_lock);
        poseidon_msquic_close();
        return false;
    }

    *out_reg = reg;
    platform_mutex_unlock(g_registrations_lock);
    return true;
}

void poseidon_msquic_close_registration(const char* app_name) {
    if (app_name == NULL) {
        return;
    }
    if (g_registrations_lock == NULL) {
        return;
    }

    const struct QUIC_API_TABLE* api = NULL;

    platform_mutex_lock(g_registrations_lock);

    registration_slot_t* slot = find_registration_slot(app_name);
    if (slot == NULL) {
        // Defensive: caller may have already closed out.
        platform_mutex_unlock(g_registrations_lock);
        return;
    }

    if (slot->refcount == 0) {
        platform_mutex_unlock(g_registrations_lock);
        return;
    }
    slot->refcount--;

    if (slot->refcount == 0) {
        api = g_api_table;
        HQUIC reg = slot->reg;
        char* name = slot->app_name;

        // Swap-remove the slot so the vec stays compact.
        int idx = (int)(slot - g_registrations.data);
        int last = g_registrations.length - 1;
        if (idx != last) {
            g_registrations.data[idx] = g_registrations.data[last];
        }
        g_registrations.length--;

        if (api != NULL && reg != NULL) {
            api->RegistrationClose(reg);
        }
        free(name);
    }

    platform_mutex_unlock(g_registrations_lock);

    if (api != NULL) {
        poseidon_msquic_close();
    }
}

const struct QUIC_API_TABLE* poseidon_msquic_api(void) {
    return g_api_table;
}
