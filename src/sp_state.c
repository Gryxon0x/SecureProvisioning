#include "sp_state.h"

#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(sp_state, LOG_LEVEL_INF);

static sp_state_t g_state = SP_STATE_FACTORY_NEW;

static void sp_state_transition(sp_state_t new_state)
{
    if (g_state == new_state) {
        return;
    }

    LOG_INF("State transition: %s -> %s",
            sp_state_str(g_state),
            sp_state_str(new_state));

    g_state = new_state;
}

void sp_state_init(void)
{
    g_state = SP_STATE_FACTORY_NEW;
    LOG_INF("Initial state: %s", sp_state_str(g_state));
}

sp_state_t sp_state_get(void)
{
    return g_state;
}

const char *sp_state_str(sp_state_t state)
{
    switch (state) {
    case SP_STATE_FACTORY_NEW:
        return "FACTORY_NEW";
    case SP_STATE_PROVISIONED_IDLE:
        return "PROVISIONED_IDLE";
    case SP_STATE_AUTHENTICATED:
        return "AUTHENTICATED";
    default:
        return "UNKNOWN";
    }
}

bool sp_state_is_factory_new(void)
{
    return g_state == SP_STATE_FACTORY_NEW;
}

bool sp_state_is_provisioned_idle(void)
{
    return g_state == SP_STATE_PROVISIONED_IDLE;
}

bool sp_state_is_authenticated(void)
{
    return g_state == SP_STATE_AUTHENTICATED;
}

int sp_state_set_factory_new(void)
{
    sp_state_transition(SP_STATE_FACTORY_NEW);
    return 0;
}

int sp_state_set_provisioned_idle(void)
{
    sp_state_transition(SP_STATE_PROVISIONED_IDLE);
    return 0;
}

int sp_state_set_authenticated(void)
{
    sp_state_transition(SP_STATE_AUTHENTICATED);
    return 0;
}