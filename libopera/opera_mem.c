#include "opera_mem.h"
#include "endianness.h"
#include "opera_state.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define ONE_KB (1024)
#define ONE_MB (ONE_KB * 1024)

uint32_t RAM_SIZE            = (ONE_MB * 3);
uint32_t RAM_SIZE_MASK       = ((ONE_MB * 3) - 1);
uint32_t HIRES_RAM_SIZE      = (ONE_MB * 6);
uint32_t HIRES_RAM_SIZE_MASK = ((ONE_MB * 6)-1);
uint32_t MAX_RAM_SIZE        = (ONE_MB * 6);

uint8_t  *DRAM            = NULL;
uint32_t  DRAM_SIZE       = (ONE_MB * 2);
uint32_t  DRAM_SIZE_MASK  = ((ONE_MB * 2) - 1);
uint8_t  *VRAM            = NULL;;
uint32_t  VRAM_SIZE       = (ONE_MB * 1);
uint32_t  VRAM_SIZE_MASK  = ((ONE_MB * 1) - 1);
uint8_t  *NVRAM           = NULL;
uint32_t  NVRAM_SIZE      = (ONE_KB * 32);
uint32_t  NVRAM_SIZE_MASK = ((ONE_KB * 32) - 1);
uint8_t  *ROM             = NULL;
uint8_t  *ROM1            = NULL;
uint32_t  ROM1_SIZE       = (ONE_MB * 1);
uint32_t  ROM1_SIZE_MASK  = ((ONE_MB * 1) - 1);
uint8_t  *ROM2            = NULL;
uint32_t  ROM2_SIZE       = (ONE_MB * 1);
uint32_t  ROM2_SIZE_MASK  = ((ONE_MB * 1) - 1);


int
opera_mem_init()
{
  DRAM  = calloc(MAX_RAM_SIZE,1);
  ROM1  = calloc(ROM1_SIZE,1);
  ROM2  = calloc(ROM2_SIZE,1);
  NVRAM = calloc(NVRAM_SIZE,1);

  /* VRAM is always at the top of DRAM */
  VRAM = &DRAM[DRAM_SIZE];
  ROM  = ROM1;

  return 0;
}

void
opera_mem_destroy()
{
  if(DRAM)
    free(DRAM);
  DRAM = NULL;

  VRAM = NULL;

  if(ROM1)
    free(ROM1);
  ROM1 = NULL;

  if(ROM2)
    free(ROM2);
  ROM2 = NULL;

  if(NVRAM)
    free(NVRAM);
  NVRAM = NULL;
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

uint32_t
opera_mem_state_size()
{
  uint32_t size;

  size = 0;
  size += opera_state_save_size(DRAM_SIZE);
  size += opera_state_save_size(VRAM_SIZE);
  size += opera_state_save_size(ROM1_SIZE);
  size += opera_state_save_size(ROM2_SIZE);
  size += opera_state_save_size(NVRAM_SIZE);

  return size;
}

uint32_t
opera_mem_state_save(void *data_)
{
  uint8_t *start = (uint8_t*)data_;
  uint8_t *data  = (uint8_t*)data_;

  data += opera_state_save(data,"DRAM",DRAM,DRAM_SIZE);
  data += opera_state_save(data,"VRAM",VRAM,VRAM_SIZE);
  data += opera_state_save(data,"ROM1",ROM1,ROM1_SIZE);
  data += opera_state_save(data,"ROM2",ROM2,ROM2_SIZE);
  data += opera_state_save(data,"NVRM",NVRAM,NVRAM_SIZE);

  return (data - start);
}

uint32_t
opera_mem_state_load(void const *data_)
{
  uint8_t const *start = (uint8_t const*)data_;
  uint8_t const *data  = (uint8_t const*)data_;

  data += opera_state_load(DRAM,"DRAM",data,DRAM_SIZE);
  data += opera_state_load(VRAM,"VRAM",data,VRAM_SIZE);
  data += opera_state_load(ROM1,"ROM1",data,ROM1_SIZE);
  data += opera_state_load(ROM2,"ROM2",data,ROM2_SIZE);
  data += opera_state_load(NVRAM,"NVRM",data,NVRAM_SIZE);

  return (data - start);
}
