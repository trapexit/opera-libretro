#ifndef FREEDO_MEM_H_INCLUDED
#define FREEDO_MEM_H_INCLUDED

#include <stdint.h>

uint32_t freedo_mem_r_ram32(const uint32_t addr);
void     freedo_mem_w_ram32(const uint32_t addr,
                            const uint32_t val);

uint32_t freedo_mem_r_dram32(const uint32_t addr);
void     freedo_mem_w_dram32(const uint32_t addr,
                             const uint32_t val);

uint32_t freedo_mem_r_vram32(const uint32_t addr);
void     freedo_mem_w_vram32(const uint32_t addr,
                             const uint32_t val);

#endif
