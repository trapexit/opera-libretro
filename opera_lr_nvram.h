#ifndef OPERA_LR_NVRAM_H_INCLUDED
#define OPERA_LR_NVRAM_H_INCLUDED

#include <stdint.h>

void opera_lr_nvram_load(const char    *gamepath,
                         const bool     shared_,
                         const uint8_t  version);
void opera_lr_nvram_save(const char    *gamepath,
                         const bool     shared_,                         
                         const uint8_t  version);

#endif
