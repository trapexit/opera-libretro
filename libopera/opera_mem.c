#include "opera_mem.h"

#include <stdint.h>
#include <stdlib.h>

#define ONE_MB (1024 * 1024)

static opera_mem_cfg_t gl_MEM_CFG = DRAM_VRAM_UNSET;

uint32_t        RAM_SIZE            = 0;
uint32_t        RAM_SIZE_MASK       = 0;
uint32_t        HIRES_RAM_SIZE      = 0;
uint32_t        HIRES_RAM_SIZE_MASK = 0;
uint8_t        *DRAM                = NULL;
uint32_t        DRAM_SIZE           = 0;
uint32_t        DRAM_SIZE_MASK      = 0;
uint8_t        *VRAM                = NULL;
uint32_t        VRAM_SIZE           = 0;
uint32_t        VRAM_SIZE_MASK      = 0;
uint8_t        *NVRAM               = NULL;
uint32_t const  NVRAM_SIZE          = (32 * 1024);
uint32_t const  NVRAM_SIZE_MASK     = ((32 * 1024) - 1);
uint8_t        *ROM                 = NULL;
uint8_t        *ROM1                = NULL;
uint32_t const  ROM1_SIZE           = ONE_MB;
uint32_t const  ROM1_SIZE_MASK      = (ONE_MB - 1);
uint8_t        *ROM2                = NULL;
uint32_t const  ROM2_SIZE           = ONE_MB;
uint32_t const  ROM2_SIZE_MASK      = (ONE_MB - 1);

opera_mem_cfg_t
opera_mem_cfg()
{
  return gl_MEM_CFG;
}

static
void
opera_mem_set(opera_mem_cfg_t const cfg_)
{
  opera_mem_cfg_t cfg = cfg_;

  switch(cfg)
    {
    case DRAM_VRAM_UNSET:
      cfg = DRAM_2MB_VRAM_1MB;
    case DRAM_2MB_VRAM_1MB:
      DRAM_SIZE = ONE_MB * 2;
      VRAM_SIZE = ONE_MB;
      break;
    case DRAM_2MB_VRAM_2MB:
      DRAM_SIZE = ONE_MB * 2;
      VRAM_SIZE = ONE_MB * 2;
      break;
    case DRAM_4MB_VRAM_1MB:
      DRAM_SIZE = ONE_MB * 4;
      VRAM_SIZE = ONE_MB;
      break;
    case DRAM_4MB_VRAM_2MB:
      DRAM_SIZE = ONE_MB * 4;
      VRAM_SIZE = ONE_MB * 2;
      break;
    case DRAM_8MB_VRAM_1MB:
      DRAM_SIZE = ONE_MB * 8;
      VRAM_SIZE = ONE_MB;
      break;
    case DRAM_8MB_VRAM_2MB:
      DRAM_SIZE = ONE_MB * 8;
      VRAM_SIZE = ONE_MB * 2;
      break;
    case DRAM_14MB_VRAM_2MB:
      DRAM_SIZE = ONE_MB * 14;
      VRAM_SIZE = ONE_MB *  2;
      break;
    case DRAM_15MB_VRAM_1MB:
      DRAM_SIZE = ONE_MB * 15;
      VRAM_SIZE = ONE_MB *  1;
      break;
    }

  DRAM_SIZE_MASK = (DRAM_SIZE - 1);
  VRAM_SIZE_MASK = (VRAM_SIZE - 1);

  RAM_SIZE            = (DRAM_SIZE + VRAM_SIZE);
  RAM_SIZE_MASK       = RAM_SIZE - 1;
  // 4X the VRAM for "hires" mode
  HIRES_RAM_SIZE      = (DRAM_SIZE + (VRAM_SIZE * 4));
  HIRES_RAM_SIZE_MASK = HIRES_RAM_SIZE - 1;

  gl_MEM_CFG = cfg;
}

#define DRAMSIZE_SHIFT     3	/* RED ONLY */
#define VRAMSIZE_SHIFT     0	/* RED ONLY */
#define VRAMSIZE_1MB 0x00000001 /* RED ONLY */
#define VRAMSIZE_2MB 0x00000002 /* RED ONLY */
#define DRAMSIZE_SETMASK   3	/* RED ONLY */
#define DRAMSIZE_1MB	   0x000000001 /* RED ONLY */
#define DRAMSIZE_4MB	   0x000000002 /* RED ONLY */
#define DRAMSIZE_16MB	   0x000000003 /* RED ONLY */
#define DRAMSIZE_SET1SHIFT 3	/* RED ONLY */
#define DRAMSIZE_SET0SHIFT (DRAMSIZE_SET1SHIFT+2) /* RED ONLY */

uint32_t
opera_mem_madam_red_sysbits(uint32_t const v_)
{
  uint32_t v = v_;

  v &= 0xFFFFFF80;

  switch(gl_MEM_CFG)
    {
    default:
    case DRAM_2MB_VRAM_1MB:
      v |= ((VRAMSIZE_1MB << VRAMSIZE_SHIFT)     |
            (DRAMSIZE_1MB << DRAMSIZE_SET0SHIFT) |
            (DRAMSIZE_1MB << DRAMSIZE_SET1SHIFT));
      break;
    case DRAM_2MB_VRAM_2MB:
      v |= ((VRAMSIZE_2MB << VRAMSIZE_SHIFT)     |
            (DRAMSIZE_1MB << DRAMSIZE_SET0SHIFT) |
            (DRAMSIZE_1MB << DRAMSIZE_SET1SHIFT));
      break;
    case DRAM_4MB_VRAM_1MB:
      v |= ((VRAMSIZE_1MB << VRAMSIZE_SHIFT)     |
            (DRAMSIZE_4MB << DRAMSIZE_SET0SHIFT));
      break;
    case DRAM_4MB_VRAM_2MB:
      v |= ((VRAMSIZE_2MB << VRAMSIZE_SHIFT)     |
            (DRAMSIZE_4MB << DRAMSIZE_SET0SHIFT));
      break;
    case DRAM_8MB_VRAM_1MB:
      v |= ((VRAMSIZE_1MB << VRAMSIZE_SHIFT)     |
            (DRAMSIZE_4MB << DRAMSIZE_SET0SHIFT) |
            (DRAMSIZE_4MB << DRAMSIZE_SET1SHIFT));
      break;
    case DRAM_8MB_VRAM_2MB:
      v |= ((VRAMSIZE_2MB << VRAMSIZE_SHIFT)     |
            (DRAMSIZE_4MB << DRAMSIZE_SET0SHIFT) |
            (DRAMSIZE_4MB << DRAMSIZE_SET1SHIFT));
      break;
    case DRAM_14MB_VRAM_2MB:
      v |= ((VRAMSIZE_2MB  << VRAMSIZE_SHIFT)     |
            (DRAMSIZE_16MB << DRAMSIZE_SET1SHIFT));
      break;
    case DRAM_15MB_VRAM_1MB:
      v |= ((VRAMSIZE_1MB  << VRAMSIZE_SHIFT)     |
            (DRAMSIZE_16MB << DRAMSIZE_SET1SHIFT));
      break;
    }

  return v;
}

bool
opera_mem_init(opera_mem_cfg_t const cfg_)
{
  uint8_t *new_ram;
  uint8_t *new_rom1;
  uint8_t *new_rom2;
  uint8_t *new_nvram;
  opera_mem_cfg_t cfg;

  cfg = cfg_;
  if(cfg == DRAM_VRAM_STOCK)
    cfg = DRAM_2MB_VRAM_1MB;
  if(cfg == gl_MEM_CFG)
    return true;

  opera_mem_set(cfg);

  new_ram   = realloc(DRAM,HIRES_RAM_SIZE);
  new_rom1  = realloc(ROM1,ROM1_SIZE);
  new_rom2  = realloc(ROM2,ROM2_SIZE);
  new_nvram = realloc(NVRAM,NVRAM_SIZE);

  if((new_ram   == NULL) ||
     (new_rom1  == NULL) ||
     (new_rom2  == NULL) ||
     (new_nvram == NULL))
    {
      if(new_ram && (new_ram != DRAM))
        free(new_ram);
      if(new_rom1 && (new_rom1 != ROM1))
        free(new_rom1);
      if(new_rom2 && (new_rom2 != ROM2))
        free(new_rom1);
      if(new_nvram && (new_nvram != NVRAM))
        free(new_nvram);
      return false;
    }

  // VRAM is always at the top of DRAM
  DRAM  = new_ram;
  VRAM  = &DRAM[DRAM_SIZE];
  ROM1  = new_rom1;
  ROM2  = new_rom2;
  ROM   = ROM1;
  NVRAM = new_nvram;

  return true;
}

void
opera_mem_destroy()
{
  if(NVRAM)
    free(NVRAM);
  NVRAM = NULL;

  if(ROM1)
    free(ROM1);
  ROM1 = NULL;

  if(ROM2)
    free(ROM2);
  ROM2 = NULL;

  if(DRAM)
    free(DRAM);
  DRAM = NULL;
  VRAM = NULL;

  gl_MEM_CFG = DRAM_VRAM_UNSET;
}
