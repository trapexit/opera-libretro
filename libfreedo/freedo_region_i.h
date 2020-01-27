#ifndef FREEDO_REGION_I_H_INCLUDED
#define FREEDO_REGION_I_H_INCLUDED

#include <stdint.h>

typedef struct freedo_region_s freedo_region_t;
struct freedo_region_s
{
  uint32_t width;
  uint32_t height;
  uint32_t start_scanline;
  uint32_t end_scanline;
  uint32_t field_rate;
};

#endif
