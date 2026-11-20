#ifndef CANFD_BOARD_PROFILE_H
#define CANFD_BOARD_PROFILE_H

#include "services/core/app_types.h"

/* Rev D stays the default. Removed Rev E switches are never sampled, while
 * their logical key IDs remain available to the touch UI. */
#ifdef CANFD_REVE_TARGET
#define CANFD_LOCK_KEY_A KEY_BACK
#define CANFD_LOCK_KEY_B KEY_REC
static inline bool board_has_physical_key(key_id_t key)
{
    return key == KEY_BACK || key == KEY_REC || key == KEY_POWER;
}
#else
#define CANFD_LOCK_KEY_A KEY_LEFT
#define CANFD_LOCK_KEY_B KEY_RIGHT
static inline bool board_has_physical_key(key_id_t key)
{
    return (unsigned)key < KEY_NUM;
}
#endif
#endif
