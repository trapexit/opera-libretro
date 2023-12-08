#ifndef OPERA_LR_OPTS_H_INCLUDED
#define OPERA_LR_OPTS_H_INCLUDED

#include "libopera/opera_bios.h"
#include "libopera/opera_mem.h"
#include "libopera/opera_vdlp.h"
#include "libopera/opera_region.h"

typedef struct opera_lr_opts_t opera_lr_opts_t;
struct opera_lr_opts_t
{
  opera_bios_t const *bios;
  opera_bios_t const *font;
  bool nvram_shared;
  uint8_t nvram_version;
  opera_region_e region;
  float cpu_overclock;
  vdlp_pixel_format_e vdlp_pixel_format;
  bool high_resolution;
  bool vdlp_bypass_clut;
  unsigned video_width;
  unsigned video_height;
  unsigned video_pitch_shift;
  unsigned active_devices;
  opera_mem_cfg_t mem_cfg;
  bool hide_lightgun_crosshairs;
  char const *madam_matrix_engine;
  bool kprint;
  bool dsp_threaded;
  bool swi_hle;
  uint32_t hack_flags;
};

extern opera_lr_opts_t g_OPTS;

void opera_lr_opts_get_values(opera_lr_opts_t *opts);
void opera_lr_opts_process(void);

bool                 opera_lr_opts_is_enabled(const char *key);
bool                 opera_lr_opts_is_nvram_shared(void);
uint8_t              opera_lr_opts_nvram_version(void);
const opera_bios_t  *opera_lr_opts_bios(void);
const opera_bios_t  *opera_lr_opts_font(void);
vdlp_pixel_format_e  opera_lr_opts_vdlp_pixel_format(void);
uint32_t             opera_lr_opts_vdlp_flags(void);
opera_region_e       opera_lr_opts_region(void);
float                opera_lr_opts_cpu_overclock(void);
unsigned             opera_lr_opts_active_devices(void);
uint32_t             opera_lr_opts_hacks(void);  

#endif
