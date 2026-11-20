/* SPDX-License-Identifier: MIT */
#ifndef REVD_STORAGE_OWNER_H
#define REVD_STORAGE_OWNER_H
#include <stdbool.h>
#include <stdint.h>
#include "services/config/cfg.h"

/* bootstrap before scheduler; init/step only on the storage task. */
void revd_storage_owner_bootstrap(void);
void revd_storage_owner_init(void);
void revd_storage_owner_step(void);
bool revd_storage_owner_ready(void);
bool revd_storage_is_owner(void);
/* Driver publishes cache before completion events can wake the core. */
void revd_storage_owner_publish_snapshot(void);

/* Core publishes its own state; the storage owner never reads app globals. */
void revd_storage_publish_context(void);
void revd_storage_config_snapshot(cfg_t *out);
bool revd_storage_play_any_active(void);
bool revd_storage_play_file_in_use(int index);
bool revd_storage_player_idle(unsigned ch);
/* Root bridge delivers this notification to the core event dispatcher. */
void revd_storage_file_removed(int index);

#endif
