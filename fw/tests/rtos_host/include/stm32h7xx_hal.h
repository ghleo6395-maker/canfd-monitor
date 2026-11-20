#ifdef REVD_RTOS_USB_TEST
#include "../../usb/include/stm32h7xx_hal.h"
#else
/* Host-only wrapper for production storage-owner tests. */
#include "../../test_storage_hal/stm32h7xx_hal.h"
uint32_t __get_IPSR(void);

#endif
