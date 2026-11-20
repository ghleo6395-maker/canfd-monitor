/* Native simulator adapter; target implementations live with the SD owner. */
#ifndef CANFD_REVD_TARGET
#include "services/device/file_port.h"
#include "sim/env_sim.h"

uint32_t file_port_token(int index) { return env_file_token(index); }
uint32_t file_port_record_token(uint32_t session) { return env_record_file_token(session); }
const char *file_port_rec_prefix_get(void) { return env_rec_name_get(); }
int file_port_rec_prefix_set(const char *prefix) { return env_rec_name_set(prefix); }
int file_port_rename(int index,uint32_t token,const char *name) { return env_log_rename(index,token,name); }
#endif
