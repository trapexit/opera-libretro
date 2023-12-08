/*
  ISC License

  Copyright (c) 2021, Antonio SJ Musumeci <trapexit@spawn.link>

  Permission to use, copy, modify, and/or distribute this software for any
  purpose with or without fee is hereby granted, provided that the above
  copyright notice and this permission notice appear in all copies.

  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/

#include "opera_lr_opts.h"

#include "opera_lr_callbacks.h"
#include "opera_lr_dsp.h"
#include "lr_input.h"

#include "libopera/hack_flags.h"
#include "libopera/opera_arm.h"
#include "libopera/opera_bios.h"
#include "libopera/opera_clock.h"
#include "libopera/opera_core.h"
#include "libopera/opera_madam.h"
#include "libopera/opera_mem.h"
#include "libopera/opera_region.h"
#include "libopera/opera_vdlp.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

opera_lr_opts_t g_OPTS;

static
int
getvar(struct retro_variable *var_)
{
  return retro_environment_cb(RETRO_ENVIRONMENT_GET_VARIABLE,var_);
}

static
const
char*
getval(const char *key_)
{
  int rv;
  char key[64];
  struct retro_variable var;

  strncpy(key,"4do_",(sizeof(key)-1));
  strncat(key,key_,(sizeof(key)-1));
  var.key = key;
  var.value = NULL;
  rv = getvar(&var);
  if(rv && (var.value != NULL))
    return var.value;

  strncpy(key,"opera_",(sizeof(key)-1));
  strncat(key,key_,(sizeof(key)-1));
  var.key = key;
  var.value = NULL;
  rv = getvar(&var);
  if(rv && (var.value != NULL))
    return var.value;

  return NULL;
}

bool
opera_lr_opts_is_enabled(const char *key_)
{
  const char *val;

  val = getval(key_);
  if(val == NULL)
    return false;

  return (strcmp(val,"enabled") == 0);
}

const
opera_bios_t*
opera_lr_opts_get_bios(void)
{
  const char *val;
  const opera_bios_t *bios;

  val = getval("bios");
  if(val == NULL)
    return NULL;

  for(bios = opera_bios_begin(); bios != opera_bios_end(); bios++)
    {
      if(strcmp(bios->name,val))
        continue;

      return bios;
    }

  return NULL;
}

const
opera_bios_t*
opera_lr_opts_get_font(void)
{
  const char *val;
  const opera_bios_t *font;

  val = getval("font");
  if(val == NULL)
    return NULL;

  for(font = opera_bios_font_begin(); font != opera_bios_font_end(); font++)
    {
      if(strcmp(font->name,val))
        continue;

      return font;
    }

  return NULL;
}

bool
opera_lr_opts_is_nvram_shared(void)
{
  const char *val;

  val = getval("nvram_storage");
  if(val == NULL)
    return true;

  return (strcmp(val,"shared") == 0);
}

uint8_t
opera_lr_opts_nvram_version(void)
{
  const char *val;

  val = getval("nvram_version");
  if(val == NULL)
    return 0;

  return atoi(val);
}

void
opera_lr_opts_process_bios(void)
{
  g_OPT_BIOS = opera_lr_opts_get_bios();
}

void
opera_lr_opts_process_font(void)
{
  g_OPT_FONT = opera_lr_opts_get_font();
}

void
opera_lr_opts_process_region(void)
{
  switch(g_OPTS.region)
    {
    default:
    case OPERA_REGION_NTSC:
      opera_region_set_NTSC();
      break;
    case OPERA_REGION_PAL1:
      opera_region_set_PAL1();
      break;
    case OPERA_REGION_PAL2:
      opera_region_set_PAL2();
      break;
    }
}

opera_region_e
opera_lr_opts_region(void)
{
  const char *val;

  val = getval("region");
  if(val == NULL)
    return OPERA_REGION_NTSC;

  if(!strcmp(val,"ntsc"))
    return OPERA_REGION_NTSC;
  else if(!strcmp(val,"pal1"))
    return OPERA_REGION_PAL1;
  else if(!strcmp(val,"pal2"))
    return OPERA_REGION_PAL2;

  return OPERA_REGION_NTSC;
}

float
opera_lr_opts_cpu_overclock(void)
{
  float mul;
  const char *val;

  val = getval("cpu_overclock");
  if(val == NULL)
    return;

  mul = atof(val);

  return mul;
}

vdlp_pixel_format_e
opera_lr_opts_vdlp_pixel_format(void)
{
  const char *val;

  val = getval("vdlp_pixel_format");
  if(val == NULL)
    return VDLP_PIXEL_FORMAT_XRGB8888;

  if(!strcmp(val,"XRGB8888"))
    return VDLP_PIXEL_FORMAT_XRGB8888;
  else if(!strcmp(val,"RGB565"))
    return VDLP_PIXEL_FORMAT_RGB565;
  else if(!strcmp(val,"0RGB1555"))
    return VDLP_PIXEL_FORMAT_0RGB1555;

  return VDLP_PIXEL_FORMAT_XRGB8888;
}

uint32_t
opera_lr_opts_get_vdlp_flags(void)
{
  uint32_t flags;

  flags = VDLP_FLAG_NONE;
  if(opera_lr_opts_is_enabled("high_resolution"))
    flags |= VDLP_FLAG_HIRES_CEL;
  if(opera_lr_opts_is_enabled("vdlp_bypass_clut"))
    flags |= VDLP_FLAG_CLUT_BYPASS;

  return flags;
}

void
opera_lr_opts_process_vdlp_flags(void)
{
  g_OPT_VDLP_FLAGS = opera_lr_opts_get_vdlp_flags();
}

void
opera_lr_opts_process_vdlp_pixel_format(void)
{
  static bool set = false;

  if(set == true)
    return;

  g_OPT_VDLP_PIXEL_FORMAT = opera_lr_opts_vdlp_pixel_format();
  switch(g_OPT_VDLP_PIXEL_FORMAT)
    {
    default:
    case VDLP_PIXEL_FORMAT_XRGB8888:
      g_OPT_VIDEO_PITCH_SHIFT = 2;
      break;
    case VDLP_PIXEL_FORMAT_0RGB1555:
    case VDLP_PIXEL_FORMAT_RGB565:
      g_OPT_VIDEO_PITCH_SHIFT = 1;
      break;
    }

  set = true;
}

unsigned
opera_lr_opts_active_devices(void)
{
  unsigned rv;
  const char *val;

  rv = 1;

  val = getval("active_devices");
  if(val)
    rv = atoi(val);

  if(rv > LR_INPUT_MAX_DEVICES)
    rv = 1;

  return rv;
}

void
opera_lr_opts_process_madam_matrix_engine(void)
{
  const char *val;

  val = getval("madam_matrix_engine");
  if(val == NULL)
    return;

  if(strcmp(val,"software") == 0)
    opera_madam_me_mode_software();
  else
    opera_madam_me_mode_hardware();
}

void
opera_lr_opts_process_swi_hle(void)
{
  opera_arm_swi_hle_set(g_OPTS.swi_hle);
}

static
uint32_t
set_reset_bits(const char *key_,
               uint32_t    input_,
               uint32_t    bitmask_)
{
  return (opera_lr_opts_is_enabled(key_) ?
          (input_ |  bitmask_) :
          (input_ & ~bitmask_));
}

uint32_t
opera_lr_opts_hacks(void)
{
  uint32_t rv;

  rv = 0;
  rv = set_reset_bits("hack_timing_1",rv,FIX_BIT_TIMING_1);
  rv = set_reset_bits("hack_timing_3",rv,FIX_BIT_TIMING_3);
  rv = set_reset_bits("hack_timing_5",rv,FIX_BIT_TIMING_5);
  rv = set_reset_bits("hack_timing_6",rv,FIX_BIT_TIMING_6);
  rv = set_reset_bits("hack_graphics_step_y",rv,FIX_BIT_GRAPHICS_STEP_Y);

  return rv;
}

void
opera_lr_opts_mem_cfg(void)
{
  unsigned    x;
  const char *val;

  val = getval("mem_capacity");
  if(val == NULL)
    return;

  sscanf(val,"%x",&x);

  return (opera_mem_cfg_t)x;
}

void
opera_lr_opts_get_values(opera_lr_opts_t &opts_)
{
  opts_.bios                     = opera_lr_opts_get_bios();
  opts_.font                     = opera_lr_opts_get_font();
  opts_.nvram_shared             = opera_lr_opts_is_nvram_shared();
  opts_.nvram_version            = opera_lr_opts_nvram_version();
  opts_.region                   = opera_lr_opts_region();
  opts_.cpu_overclock            = opera_lr_opts_cpu_overclock();
  opts_.vdlp_pixel_format        = opera_lr_opts_vdlp_pixel_format();
  opts_.high_resolution          = opera_lr_opts_is_enabled("high_resolution");
  opts_.vdlp_bypass_clut         = opera_lr_opts_is_enabled("vdlp_bypass_clut");
  opts_.video_width;
  opts_.video_height;
  opts_.video_pitch_shift;
  opts_.active_devices           = opera_lr_opts_active_devices();
  opts_.mem_cfg                  = opera_lr_opts_mem_cfg();
  opts_.hide_lightgun_crosshairs = opera_lr_opts_is_enabled("hide_lightgun_crosshairs");
  opts_.madam_matrix_engine;
  opts_.kprint                   = opera_lr_opts_is_enabled("kprint");
  opts_.dsp_threaded             = opera_lr_opts_is_enabled("dsp_threaded");
  opts_.swi_hle                  = opera_lr_opts_is_enabled("swi_hle");
  opts_.hack_flags               = opera_lr_opts_hacks();
}

void
opera_lr_opts_process_cpu_overlock(void)
{
  opera_clock_cpu_set_freq_mul(g_OPTS.cpu_overclock);
}

void
opera_lr_opts_process_dsp_threaded(void)
{
  opera_lr_dsp_init(g_OPTS.dsp_threaded);
}

void
opera_lr_opts_process_hack_flags()
{
  FIXMODE = g_OPTS.hack_flags;
}

void
opera_lr_opts_process_high_resolution(void)
{
  if(g_OPTS.high_resolution)
    {
      HIRESMODE           = 1;
      g_OPTS.video_width  = (opera_region_width()  << 1);
      g_OPTS.video_height = (opera_region_height() << 1);
    }
  else
    {
      HIRESMODE           = 0;
      g_OPTS.video_width  = opera_region_width();
      g_OPTS.video_height = opera_region_height();
    }
}

void
opera_lr_opts_process_kprint(void)
{
  if(g_OPTS.kprint)
    opera_madam_kprint_enable();
  else
    opera_madam_kprint_disable();
}

void
opera_lr_opts_process()
{
  opera_lr_opts_process_cpu_overlock();
  opera_lr_opts_process_dsp_threaded();
  opera_lr_opts_process_hack_flags();
  opera_lr_opts_process_high_resolution();
  opera_lr_opts_process_kprint();
  opera_lr_opts_process_peripherals();
  opera_lr_opts_process_region();
  opera_lr_opts_process_swi_hle();
  opera_lr_opts_process_vdlp();
}
