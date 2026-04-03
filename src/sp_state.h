#ifndef SP_STATE_H_
#define SP_STATE_H_

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    SP_STATE_FACTORY_NEW = 0,
    SP_STATE_PROVISIONED_IDLE,
    SP_STATE_AUTHENTICATED,
} sp_state_t;

void sp_state_init(void);

sp_state_t sp_state_get(void);
const char *sp_state_str(sp_state_t state);

bool sp_state_is_factory_new(void);
bool sp_state_is_provisioned_idle(void);
bool sp_state_is_authenticated(void);

int sp_state_set_factory_new(void);
int sp_state_set_provisioned_idle(void);
int sp_state_set_authenticated(void);

#endif /* SP_STATE_H_ */