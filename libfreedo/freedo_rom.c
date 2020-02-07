#include <stdint.h>
#include <stdlib.h>

#define MB (1024 * 1024)

uint8_t *g_ROM;

void
freedo_rom_init(void)
{
  g_ROM = calloc(1,MB);
}

void
freedo_rom_free(void)
{
  if(g_ROM)
    free(g_ROM);
}
