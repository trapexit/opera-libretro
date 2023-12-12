#ifndef OPERA_LR_OPTS_H_INCLUDED
#define OPERA_LR_OPTS_H_INCLUDED

#include "libopera/opera_bios.h"
#include "libopera/opera_mem.h"
#include "libopera/opera_vdlp.h"
#include "libopera/opera_region.h"

typedef struct opera_lr_opts_t opera_lr_opts_t;
struct opera_lr_opts_t
{
  bool set_once;
  opera_bios_t const *bios;
  opera_bios_t const *font;
  bool nvram_shared;
  uint8_t nvram_version;
  opera_region_e region;
  float cpu_overclock;
  vdlp_pixel_format_e vdlp_pixel_format;
  bool vdlp_bypass_clut;
  uint32_t *video_buffer;
  bool high_resolution;
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

void opera_lr_opts_get();
void opera_lr_opts_set();
void opera_lr_opts_process();
void opera_lr_opts_destroy();

void opera_lr_opts_get_bios();
void opera_lr_opts_set_bios();

void opera_lr_opts_get_font();
void opera_lr_opts_set_font();

void opera_lr_opts_get_nvram_shared();
void opera_lr_opts_set_nvram_shared();

void opera_lr_opts_get_nvram_version();
void opera_lr_opts_set_nvram_version();

void opera_lr_opts_get_region();
void opera_lr_opts_set_region();

void opera_lr_opts_get_cpu_overclock();
void opera_lr_opts_set_cpu_overclock();

void opera_lr_opts_get_vdlp_pixel_format();
void opera_lr_opts_set_vdlp_pixel_format();

void opera_lr_opts_get_vdlp_bypass_clut();
void opera_lr_opts_set_vdlp_bypass_clut();

void opera_lr_opts_get_high_resolution();
void opera_lr_opts_set_high_resolution();

void opera_lr_opts_get_active_devices();
void opera_lr_opts_set_active_devices();

void opera_lr_opts_get_mem_cfg();
void opera_lr_opts_set_mem_cfg();

void opera_lr_opts_get_hide_lightgun_crosshairs();
void opera_lr_opts_set_hide_lightgun_crosshairs();

void opera_lr_opts_get_madam_matrix_engine();
void opera_lr_opts_set_madam_matrix_engine();

void opera_lr_opts_get_kprint();
void opera_lr_opts_set_kprint();

void opera_lr_opts_get_dsp_threaded();
void opera_lr_opts_set_dsp_threaded();

void opera_lr_opts_get_swi_hle();
void opera_lr_opts_set_swi_hle();

void opera_lr_opts_set_hack_flags();
void opera_lr_opts_set_hack_flags();

#endif
