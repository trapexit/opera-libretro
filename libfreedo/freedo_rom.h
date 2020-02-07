#ifndef FREEDO_ROM_H_INCLUDED
#define FREEDO_ROM_H_INCLUDED

#include "inline.h"

#include <stdint.h>

extern uint8_t *g_ROM;

void freedo_rom_init(void);

static
INLINE
uint32_t
freedo_rom_r32(const uint32_t addr_)
{
  return *(uint32_t*)&g_ROM[addr_];
}

#endif
