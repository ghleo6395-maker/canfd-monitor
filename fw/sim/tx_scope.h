/* Device manual send scope. Both channels use their own entries and timing. */
#ifndef TX_SCOPE_H
#define TX_SCOPE_H
#include "services/core/app_types.h"
#include "services/core/jsonw.h"
typedef enum { SCOPE_IDLE, SCOPE_CONFIRMING, SCOPE_ARMING } scope_state_t;
typedef enum { SCOPE_GROUP, SCOPE_SINGLE } scope_kind_t;
typedef struct {
    scope_state_t state;
    uint32_t mask;
    scope_kind_t kind;
    uint32_t entry,request;
    int error;
    char message[192];
} scope_status_t;
void scope_init(void);
/* Results are TXSEQ_OK / TXSEQ_WAIT_CONFIRM / TXSEQ_ERR_*. */
int scope_start(uint32_t mask);
int scope_single(uint32_t mask,uint32_t entry);
void scope_stop(uint32_t mask);
/* Cancel an uncommitted request only; used when the UI changes send scope. */
void scope_cancel(void);
int scope_answer(uint32_t request,bool allow);
void scope_tick(void);
void scope_handle(const app_evt_t *event);
const scope_status_t *scope_state(void);
const char *scope_result_text(int result);
void scope_json(jw_t *json);
#endif
