#ifndef DEVICE_SEND_PLAN_VALIDATE_H
#define DEVICE_SEND_PLAN_VALIDATE_H
#include "sim/send_plan.h"
#include <stddef.h>
#include "services/tx_sched/tx_sm.h"
/* Fully validate one saved entry and decode it into a scheduler entry. */
int sp_entry_unpack(int ch,const send_plan_entry_t *source,tx_entry_t *out);
/* UTF-8/FAT-safe names; shared by host and board. safe_name returns
 * file_port name/limit codes, filename_validate returns SP_OK/SP_ERR_NAME. */
int sp_safe_name(const char *name,size_t maximum,int allow_empty);
int sp_filename_validate(const char *name);
#endif
