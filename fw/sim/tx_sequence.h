/* Sequential ID-group scheduling shared by the device and simulator.
 * Reuses production TX validation/FIFO/ACK; only app_tick advances this owner. */
#ifndef TX_SEQUENCE_H
#define TX_SEQUENCE_H

#include "services/tx_sched/tx_sm.h"

#define TXSEQ_MAX_COUNT 10000u
#define TXSEQ_MAX_TIME_MS 3600000u
#define TXSEQ_DEFAULT_CYCLE_MS 100u
#define TXSEQ_DEFAULT_FRAME_GAP_MS 10u
#define TXSEQ_DEFAULT_ID_GAP_MS 10u

typedef struct {
    bool enabled;
    uint32_t repeat_count;
    uint32_t frame_gap_ms;
    uint32_t cycle_ms;
} txseq_entry_config_t;

typedef enum { TXSEQ_IDLE, TXSEQ_CONFIRMING, TXSEQ_ARMING, TXSEQ_RUNNING } txseq_state_t;
typedef enum {
    TXSEQ_OK = 0, TXSEQ_WAIT_CONFIRM = 1,
    TXSEQ_ERR_CHANNEL = -1, TXSEQ_ERR_ARGUMENT = -2, TXSEQ_ERR_CHANGED = -3,
    TXSEQ_ERR_ACTIVE = -4, TXSEQ_ERR_EMPTY = -5, TXSEQ_ERR_NOT_READY = -6,
    TXSEQ_ERR_CONFLICT = -7, TXSEQ_ERR_FRAME = -8, TXSEQ_ERR_MODE = -9,
    TXSEQ_ERR_REQUEST = -10, TXSEQ_ERR_LOAD = -11
} txseq_result_t;
typedef enum {
    TXSEQ_STOP_NONE, TXSEQ_STOP_USER, TXSEQ_STOP_CANCELLED,
    TXSEQ_STOP_POWER, TXSEQ_STOP_MODE, TXSEQ_STOP_CHANGED,
    TXSEQ_STOP_NOACK, TXSEQ_STOP_CONFLICT, TXSEQ_STOP_FRAME, TXSEQ_STOP_COMPLETE
} txseq_stop_reason_t;

typedef struct {
    txseq_entry_config_t entries[TX_ENTRIES];
    uint32_t id_gap_ms, revision, request;
    txseq_state_t state;
    int current_entry;
    uint32_t occurrence;       /* attempted/skipped slots in this ID group */
    uint32_t rounds;           /* complete passes through enabled entries */
    uint32_t sent, missed;     /* accepted into TX FIFO / skipped or FIFO full */
    uint64_t group_start_ms, next_due_ms;
    txseq_stop_reason_t stop_reason;
} txseq_channel_t;

void txseq_init(void);
const txseq_channel_t *txseq_get(int ch);
/* Atomic: all four rows are validated before any change is committed.
 * cycle_ms >= (repeat_count - 1) * frame_gap_ms + 1, cycle >= 1 ms.
 * Same-ID gap may be 0 only when count is 1. IDs/data stay in tx_get(). */
int txseq_configure(int ch, const txseq_entry_config_t entries[TX_ENTRIES],
                    uint32_t id_gap_ms, uint32_t expected_revision);
/* Only timing fields change; counts/selection/ID/data remain intact. */
int txseq_defaults(int ch, uint32_t expected_revision);
/* 0 = running, 1 = device confirmation queued, negative = rejected.
 * No test/probe frame is sent to obtain normal-mode or first-TX permission. */
int txseq_start(int ch, uint32_t expected_revision);
/* Finite execution stops at the exact final group boundary; zero is infinite. */
int txseq_start_rounds(int ch, uint32_t expected_revision, uint32_t round_limit);
void txseq_stop(int ch);
void txseq_stop_all(txseq_stop_reason_t reason);
bool txseq_any_active(void);
bool txseq_uses_entry(int ch, int entry);
uint16_t txseq_load_permille(int ch);
/* Device-dialog callback, bound to this exact request and entry revisions. */
int txseq_answer(int ch, uint32_t request, bool allow);
void txseq_tick(void);
/* SD-wait foreground pump: services RUNNING only, never arms/confirms a send. */
void txseq_realtime_tick(void);
const char *txseq_result_text(int result);
const char *txseq_stop_text(txseq_stop_reason_t reason);

#endif
