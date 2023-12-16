#ifndef LIBOPERA_MEM_H_INCLUDED
#define LIBOPERA_MEM_H_INCLUDED

#include <stdint.h>

extern uint32_t RAM_SIZE;
extern uint32_t RAM_SIZE_MASK;
extern uint32_t HIRES_RAM_SIZE;
extern uint32_t HIRES_RAM_SIZE_MASK;
extern uint32_t MAX_RAM_SIZE;

extern uint8_t *DRAM;
extern uint32_t DRAM_SIZE;
extern uint32_t DRAM_SIZE_MASK;

extern uint8_t *VRAM;
extern uint32_t VRAM_SIZE;
extern uint32_t VRAM_SIZE_MASK;

extern uint8_t *NVRAM;
extern uint32_t NVRAM_SIZE;
extern uint32_t NVRAM_SIZE_MASK;

extern uint8_t *ROM;

extern uint8_t *ROM1;
extern uint32_t ROM1_SIZE;
extern uint32_t ROM1_SIZE_MASK;

extern uint8_t *ROM2;
extern uint32_t ROM2_SIZE;
extern uint32_t ROM2_SIZE_MASK;

int  opera_mem_init();
void opera_mem_destroy();

void opera_mem_rom1_clear();
void opera_mem_rom1_byteswap32_if_le();
void opera_mem_rom2_clear();
void opera_mem_rom2_byteswap32_if_le();

void opera_mem_rom_select(int const);

uint32_t opera_mem_state_size();
uint32_t opera_mem_state_save(void *data);
uint32_t opera_mem_state_load(void const *data);

#endif
