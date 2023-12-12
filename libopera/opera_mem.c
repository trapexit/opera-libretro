/*
  documentation:
  * https://3dodev.com/documentation/hardware/opera/memory_configurations
  * About Memory: https://3dodev.com/documentation/development/opera/pf25/ppgfldr/pgsfldr/spg/05spg001
  * Manageing Memory: https://3dodev.com/documentation/development/opera/pf25/ppgfldr/pgsfldr/spg/01spg003
  */

#include "opera_mem.h"

#include "endianness.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define ONE_KB 1024
#define ONE_MB (ONE_KB * 1024)

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
uint32_t const  NVRAM_SIZE          = (ONE_KB * 32);
uint32_t const  NVRAM_SIZE_MASK     = ((ONE_KB * 32) - 1);
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
    default:
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
  RAM_SIZE_MASK       = (RAM_SIZE - 1);
  // 4X the VRAM for "hires" mode
  HIRES_RAM_SIZE      = (DRAM_SIZE + (VRAM_SIZE * 4));
  HIRES_RAM_SIZE_MASK = (HIRES_RAM_SIZE - 1);

  gl_MEM_CFG = cfg;
}

#define MADAM_RAM_MASK     0x0000007F
#define DRAMSIZE_SHIFT     3	/* RED ONLY */
#define VRAMSIZE_SHIFT     0	/* RED ONLY */
#define VRAMSIZE_1MB       0x00000001 /* RED ONLY */
#define VRAMSIZE_2MB       0x00000002 /* RED ONLY */
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

  v &= ~MADAM_RAM_MASK;

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
  opera_mem_cfg_t cfg;

  if(gl_MEM_CFG != DRAM_VRAM_UNSET)
    return false;

  cfg = cfg_;
  if(cfg == DRAM_VRAM_UNSET)
    cfg = DRAM_2MB_VRAM_1MB;

  opera_mem_set(cfg);

  DRAM  = calloc(HIRES_RAM_SIZE,1);
  ROM1  = calloc(ROM1_SIZE,1);
  ROM2  = calloc(ROM2_SIZE,1);
  NVRAM = calloc(NVRAM_SIZE,1);

  if((DRAM  == NULL)  ||
     (ROM1  == NULL)  ||
     (ROM2  == NULL)  ||
     (NVRAM == NULL))
    {
      opera_mem_destroy();
      return false;
    }

  // VRAM is always at the top of DRAM
  VRAM  = &DRAM[DRAM_SIZE];
  ROM   = ROM1;

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

void
opera_mem_rom1_clear()
{
  memset(ROM1,0,ROM1_SIZE);
}

void
opera_mem_rom1_byteswap32_if_le()
{
  uint32_t *mem;
  uint64_t  size;

  mem  = (uint32_t*)ROM1;
  size = (ROM1_SIZE / sizeof(uint32_t));

  swap32_array_if_little_endian(mem,size);
}

void
opera_mem_rom2_clear()
{
  memset(ROM2,0,ROM2_SIZE);
}

void
opera_mem_rom2_byteswap32_if_le()
{
  uint32_t *mem;
  uint64_t  size;

  mem  = (uint32_t*)ROM2;
  size = (ROM2_SIZE / sizeof(uint32_t));

  swap32_array_if_little_endian(mem,size);
}

void
opera_mem_rom_select(int n_)
{
  ROM = ((n_ == 0) ? ROM1 : ROM2);
}
