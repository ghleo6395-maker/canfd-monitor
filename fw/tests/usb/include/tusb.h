/* Test-only TinyUSB transport facade, real headers independently ARM-compiled. */
#ifndef TEST_TUSB_H
#define TEST_TUSB_H
#include <stdint.h>
#include <stdbool.h>
#define SCSI_SENSE_NOT_READY 2u
#define SCSI_SENSE_MEDIUM_ERROR 3u
#define SCSI_SENSE_ILLEGAL_REQUEST 5u
#define TUD_CFGID_DWC2 100u
#define TUSB_ROLE_DEVICE 1u
#define TUSB_SPEED_FULL 2u
typedef union { struct { uint16_t bm_double_buffered; bool vbus_sensing; } dwc2; } tud_configure_param_t;
typedef struct { uint8_t role, speed; } tusb_rhport_init_t;
bool tud_mounted(void);
bool tud_suspended(void);
bool tud_cdc_connected(void);
uint32_t tud_cdc_write_available(void);
uint32_t tud_cdc_write(const void *bytes, uint32_t len);
uint32_t tud_cdc_write_flush(void);
uint32_t tud_cdc_available(void);
int32_t tud_cdc_read_char(void);
bool tud_configure(uint8_t port, uint32_t id, const void *config);
bool tusb_init(uint8_t port, const tusb_rhport_init_t *config);
void tud_task_ext(uint32_t timeout, bool in_isr);
void tud_int_handler(uint8_t port);
bool tud_msc_set_sense(uint8_t lun, uint8_t key, uint8_t asc, uint8_t ascq);
#ifdef REVD_FREERTOS
bool tud_cdc_n_connected(uint8_t itf);
uint32_t tud_cdc_n_available(uint8_t itf);
uint32_t tud_cdc_n_read(uint8_t itf, void *buffer, uint32_t count);
uint32_t tud_cdc_n_write_available(uint8_t itf);
uint32_t tud_cdc_n_write(uint8_t itf, const void *buffer, uint32_t count);
uint32_t tud_cdc_n_write_flush(uint8_t itf);
#endif
#endif
