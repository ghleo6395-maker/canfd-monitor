/* SPDX-License-Identifier: MIT
 * FatFs R0.16 disk I/O glue. No filesystem/raw-sector access is allowed while
 * the other owner holds the card. All callbacks execute in main-loop context.
 * The transport uses SDMMC IDMA and cooperatively waits for real completion;
 * FatFs is never told RES_OK while sectors are still in flight.
 */
#include "ff.h"
#include "diskio.h"
#include "storage_sd.h"
#ifdef REVD_FREERTOS
#include "rtos/storage_owner.h"
#define REQUIRE_OWNER(result) do { if(!revd_storage_is_owner())return (result); } while(0)
#else
#define REQUIRE_OWNER(result) ((void)0)
#endif

DSTATUS disk_initialize(BYTE pdrv)
{
    REQUIRE_OWNER(STA_NOINIT);
    return pdrv == 0 && storage_sd_disk_init() ? 0 : STA_NOINIT;
}

DSTATUS disk_status(BYTE pdrv)
{
    REQUIRE_OWNER(STA_NOINIT);
    return pdrv == 0 && storage_sd_disk_ready() ? 0 : STA_NOINIT;
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count)
{
    REQUIRE_OWNER(RES_NOTRDY);
    if (pdrv || !buff || !count) return RES_PARERR;
    if (!storage_sd_disk_ready()) return RES_NOTRDY;
    return storage_sd_disk_read((uint32_t)sector, buff, count) ? RES_OK : RES_ERROR;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count)
{
    REQUIRE_OWNER(RES_NOTRDY);
    if (pdrv || !buff || !count) return RES_PARERR;
    if (!storage_sd_disk_ready()) return RES_NOTRDY;
    return storage_sd_disk_write((uint32_t)sector, buff, count) ? RES_OK : RES_ERROR;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
    REQUIRE_OWNER(RES_NOTRDY);
    if (pdrv) return RES_PARERR;
    if (!storage_sd_disk_ready()) return RES_NOTRDY;
    switch (cmd) {
    case CTRL_SYNC:
        return storage_sd_disk_sync() ? RES_OK : RES_ERROR;
    case GET_SECTOR_COUNT:
        if (!buff) return RES_PARERR;
        *(LBA_t *)buff = storage_sd_disk_block_count();
        return RES_OK;
    case GET_SECTOR_SIZE:
        if (!buff) return RES_PARERR;
        *(WORD *)buff = 512;
        return RES_OK;
    case GET_BLOCK_SIZE:
        if (!buff) return RES_PARERR;
        /* Safe minimum; no erase/TRIM command is issued by this port. */
        *(DWORD *)buff = 1;
        return RES_OK;
    default:
        return RES_PARERR;
    }
}
