#include "freedo_clio.h"
#include "freedo_madam.h"
#include "freedo_rom.h"

#include <stdint.h>
#include <stdlib.h>

#define MB (1024 * 1024)

uint8_t *g_DRAM;
uint8_t *g_VRAM;
uint32_t g_RAM_TOP = 0x00300000;

int
freedo_mem_init(void)
{
  g_DRAM = (uint8_t*)calloc(3,MB);
  g_VRAM = &g_DRAM[2 * MB];

  return (g_DRAM == NULL);
}

uint8_t*
freedo_dram_get(void)
{
  return g_DRAM;
}

uint32_t
freedo_dram_r32(const uint32_t addr_)
{
  return *(uint32_t*)&g_DRAM[addr_];
}

void
freedo_dram_w32(const uint32_t addr_,
                const uint32_t val_)
{
  *((uint32_t*)&g_DRAM[addr_]) = val_;
}

uint32_t
freedo_vram_r32(const uint32_t addr_)
{
  return *(uint32_t*)&g_VRAM[addr_];
}

void
freedo_vram_w32(const uint32_t addr_,
                const uint32_t val_)
{
  *((uint32_t*)&g_VRAM[addr_]) = val_;
}

uint32_t
freedo_mem_r32(const uint32_t addr_)
{
  uint32_t idx;
  uint32_t addr;

  addr = addr_;

  if(addr < g_RAM_TOP)
    return freedo_dram_r32(addr);

  idx = (addr ^ 0x03000000);
  if(!(idx & ~0x000FFFFF))
    return freedo_rom_r32(idx);

  idx = (addr ^ 0x06000000);
  if(!(idx & ~0x000FFFFF))
    return freedo_rom_r32(idx);

  /* idx = (addr ^ 0x03300000); */
  /* if(!(idx & ~0x000FFFFF)) */
  /*   return freedo_madam_peek(idx); */

  /* idx = (addr ^ 0x03400000); */
  /* if(!(idx & ~0x000FFFFF)) */
  /*   return freedo_clio_peek(idx); */

  return 0xBADACCE5;
}
