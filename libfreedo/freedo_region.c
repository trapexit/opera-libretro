#include "freedo_region_i.h"

#define NTSC_WIDTH 320
#define NTSC_HEIGHT 240
#define NTSC_START_SCANLINE 21
#define NTSC_END_SCANLINE 261
#define NTSC_FIELD_RATE 60

#define PAL1_WIDTH 320
#define PAL1_HEIGHT 240
#define PAL1_START_SCANLINE 21
#define PAL1_END_SCANLINE 261
#define PAL1_FIELD_RATE 50

#define PAL2_WIDTH 384
#define PAL2_HEIGHT 288
#define PAL2_START_SCANLINE 22
#define PAL2_END_SCANLINE 312
#define PAL2_FIELD_RATE 50

freedo_region_t g_REGION =
  {
    NTSC_WIDTH,
    NTSC_HEIGHT,
    NTSC_START_SCANLINE,
    NTSC_END_SCANLINE,
    NTSC_FIELD_RATE
  };

void
freedo_region_set_NTSC(void)
{
  g_REGION.width          = NTSC_WIDTH;
  g_REGION.height         = NTSC_HEIGHT;
  g_REGION.start_scanline = NTSC_START_SCANLINE;
  g_REGION.end_scanline   = NTSC_END_SCANLINE;
  g_REGION.field_rate     = NTSC_FIELD_RATE;
}

void
freedo_region_set_PAL1(void)
{
  g_REGION.width          = PAL1_WIDTH;
  g_REGION.height         = PAL1_HEIGHT;
  g_REGION.start_scanline = PAL1_START_SCANLINE;
  g_REGION.end_scanline   = PAL1_END_SCANLINE;
  g_REGION.field_rate     = PAL1_FIELD_RATE;
}

void
freedo_region_set_PAL2(void)
{
  g_REGION.width          = PAL2_WIDTH;
  g_REGION.height         = PAL2_HEIGHT;
  g_REGION.start_scanline = PAL2_START_SCANLINE;
  g_REGION.end_scanline   = PAL2_END_SCANLINE;
  g_REGION.field_rate     = PAL2_FIELD_RATE;
}
