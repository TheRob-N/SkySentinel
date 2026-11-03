#if defined(SS_CYD) || defined(SS_SD)
#include "sd_format.h"

#include <diskio.h>
#include <ff.h>
#include <sd_diskio.h>

namespace ss {

bool sdFormatCard(uint8_t cs, SPIClass& spi, uint32_t hz, char* msg, size_t cap) {
  const uint8_t pdrv = sdcard_init(cs, &spi, (int)hz);
  if (pdrv == 0xFF) { snprintf(msg, cap, "no card"); return false; }
  // A bigger work buffer formats much faster; fall back to the minimum when memory is short.
  size_t len = 16384;
  BYTE* work = (BYTE*)malloc(len);
  if (!work) { len = FF_MAX_SS; work = (BYTE*)malloc(len); }
  if (!work) { sdcard_uninit(pdrv); snprintf(msg, cap, "out of memory"); return false; }
  char drv[3] = {(char)('0' + pdrv), ':', 0};
  // Wake the card (it's only talked to on first use); no answer = no card
  if (disk_initialize(pdrv) & STA_NOINIT) {
    free(work);
    sdcard_uninit(pdrv);
    snprintf(msg, cap, "no card answered");
    return false;
  }
  LBA_t sectors = 0;
  disk_ioctl(pdrv, GET_SECTOR_COUNT, &sectors);
  const uint64_t mb = (uint64_t)sectors * sdcard_sector_size(pdrv) / (1024ULL * 1024ULL);
  const MKFS_PARM opt = {(BYTE)FM_FAT32, 0, 0, 0, 0};
  FRESULT r = f_mkfs(drv, &opt, work, len);
  if (r != FR_OK) {  // too small for FAT32 (under ~64 MB): let FatFs pick
    const MKFS_PARM any = {(BYTE)FM_ANY, 0, 0, 0, 0};
    r = f_mkfs(drv, &any, work, len);
  }
  free(work);
  sdcard_uninit(pdrv);
  if (r != FR_OK) { snprintf(msg, cap, "format failed (FatFs %d)", (int)r); return false; }
  snprintf(msg, cap, "formatted FAT32, %lu MB", (unsigned long)mb);
  return true;
}

}  // namespace ss
#endif
