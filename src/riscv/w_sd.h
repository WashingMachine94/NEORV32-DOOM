#ifndef __W_SD__
#define __W_SD__

#include <cstdint>

#define BLOCK_SIZE 512         // SD card block size in bytes
#define WAD_START_BLOCK 34864  // Starting block of the WAD file on the SD card

void wad_read_bytes(uint8_t* dest, uint32_t wad_start_block, uint32_t lump_offset, uint32_t lump_size);
uint8_t sd_init(void);

#endif