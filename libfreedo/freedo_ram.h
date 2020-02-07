#ifndef FREEDO_RAM_H_INCLUDED
#define FREEDO_RAM_H_INCLUDED

#include <stdint.h>

uint32_t freedo_ram_r32(const uint32_t addr);
void     freedo_ram_w32(const uint32_t addr,
                        const uint32_t val);

uint32_t freedo_dram_r32(const uint32_t addr);
void     freedo_dram_w32(const uint32_t addr,
                         const uint32_t val);

uint32_t freedo_vram_r32(const uint32_t addr);
void     freedo_vram_w32(const uint32_t addr,
                         const uint32_t val);

#endif
