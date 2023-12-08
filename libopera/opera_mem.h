#pragma once

#include <boolean.h>

#include <stdint.h>

enum opera_mem_cfg_t
  {
    DRAM_VRAM_UNSET    = 0x00,
    DRAM_VRAM_STOCK    = 0x00,
    DRAM_2MB_VRAM_1MB  = 0x21,
    DRAM_2MB_VRAM_2MB  = 0x22,
    DRAM_4MB_VRAM_1MB  = 0x41,
    DRAM_4MB_VRAM_2MB  = 0x42,
    DRAM_8MB_VRAM_1MB  = 0x81,
    DRAM_8MB_VRAM_2MB  = 0x82,
    DRAM_14MB_VRAM_2MB = 0xE2,
    DRAM_15MB_VRAM_1MB = 0xF1
  };
typedef enum opera_mem_cfg_t opera_mem_cfg_t;

extern uint32_t        RAM_SIZE;
extern uint32_t        RAM_SIZE_MASK;
extern uint32_t        HIRES_RAM_SIZE;
extern uint32_t        HIRES_RAM_SIZE_MASK;
extern uint8_t        *DRAM;
extern uint32_t        DRAM_SIZE;
extern uint32_t        DRAM_SIZE_MASK;
extern uint8_t        *VRAM;
extern uint32_t        VRAM_SIZE;
extern uint32_t        VRAM_SIZE_MASK;
extern uint8_t        *NVRAM;
extern uint32_t const  NVRAM_SIZE;
extern uint32_t const  NVRAM_SIZE_MASK;
extern uint8_t        *ROM1;
extern uint32_t const  ROM1_SIZE;
extern uint32_t const  ROM1_SIZE_MASK;
extern uint8_t        *ROM2;
extern uint32_t const  ROM2_SIZE;
extern uint32_t const  ROM2_SIZE_MASK;
extern uint8_t        *ROM;

bool opera_mem_init(opera_mem_cfg_t const);
void opera_mem_destroy();

opera_mem_cfg_t opera_mem_cfg();

uint32_t opera_mem_madam_red_sysbits(uint32_t const);

void opera_mem_rom1_clear();
void opera_mem_rom2_clear();
