#ifndef __W_SD__
#define __W_SD__

#include <cstdint>

void wad_read_bytes(uint8_t* dest, uint32_t wad_start_block, uint32_t lump_offset, uint32_t lump_size);
uint8_t sd_init(void);

#endif