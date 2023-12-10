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

#include "file/file_path.h"
#include "retro_miscellaneous.h"
#include "streams/file_stream.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

opera_lr_opts_t g_OPTS = {0};

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

static
int
getval_as_int(const char *key_,
              const int   default_)
{
  const char *val;

  val = getval(key_);
  if(val == NULL)
    return default_;

  return atoi(val);
}

static
float
getval_as_float(const char  *key_,
                const float  default_)
{
  const char *val;

  val = getval(key_);
  if(val == NULL)
    return default_;

  return atof(val);
}

static
bool
getval_is_str(const char *key_,
              const char *str_,
              const bool  default_)
{
  const char *val;

  val = getval(key_);
  if(val == NULL)
    return default_;

  return (strcmp(val,str_) == 0);
}

static
bool
getval_is_enabled(const char *key_,
                  const bool  default_)
{
  return getval_is_str(key_,"enabled",default_);
}

void
opera_lr_opts_get_video_buffer()
{

}

void
opera_lr_opts_set_video_buffer()
{
  uint32_t size;

  if(g_OPTS.video_buffer != NULL)
    return;

  /*
   * The 4x multiplication is for hires mode
   * 4 bytes per pixel will handle any format
   * Wastes some memory but simplifies things
  */

  size = (opera_region_max_width() * opera_region_max_height() * 4);

  g_OPTS.video_buffer = (uint32_t*)calloc(size,sizeof(uint32_t));
}

void
opera_lr_opts_get_bios(void)
{
  const char *val;
  const opera_bios_t *bios;

  g_OPTS.bios = opera_bios_begin();

  val = getval("bios");
  if(val == NULL)
    return;

  for(bios = opera_bios_begin(); bios != opera_bios_end(); bios++)
    {
      if(strcmp(bios->name,val))
        continue;

      g_OPTS.bios = bios;
      return;
    }
}

static
int64_t
read_file_from_system_directory(const char *filename_,
                                uint8_t    *data_,
                                int64_t     size_)
{
  int64_t rv;
  RFILE *file;
  const char *system_path;
  char fullpath[PATH_MAX_LENGTH];

  system_path = NULL;
  rv = retro_environment_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY,&system_path);
  if((rv == 0) || (system_path == NULL))
    return -1;

  fill_pathname_join(fullpath,system_path,filename_,PATH_MAX_LENGTH);

  file = filestream_open(fullpath,RETRO_VFS_FILE_ACCESS_READ,RETRO_VFS_FILE_ACCESS_HINT_NONE);
  if(file == NULL)
    return -1;

  rv = filestream_read(file,data_,size_);

  filestream_close(file);

  return rv;
}

void
opera_lr_opts_set_bios(void)
{
  int64_t rv;

  opera_lr_opts_set_mem_cfg();

  if(g_OPTS.bios == NULL)
    {
      retro_log_printf_cb(RETRO_LOG_ERROR,"[Opera]: no BIOS ROM found\n");
      return;
    }

  rv = read_file_from_system_directory(g_OPTS.bios->filename,ROM1,ROM1_SIZE);
  if(rv < 0)
    {
      retro_log_printf_cb(RETRO_LOG_ERROR,
                          "[Opera]: unable to find or load BIOS ROM - %s\n",
                          g_OPTS.bios->filename);
      return;
    }

  opera_mem_rom1_byteswap32_if_le();

  // On real hardware the ROM is mapped to the beginning of memory and
  // once written to maps in RAM. Copying the ROM into RAM works for
  // our purposes.
  memcpy(DRAM,ROM1,ROM1_SIZE);
}


void
opera_lr_opts_get_font(void)
{
  const char *val;
  const opera_bios_t *font;

  g_OPTS.font = NULL;

  val = getval("font");
  if(val == NULL)
    return;

  for(font = opera_bios_font_begin(); font != opera_bios_font_end(); font++)
    {
      if(strcmp(font->name,val))
        continue;

      g_OPTS.font = font;
      return;
    }
}

void
opera_lr_opts_set_font(void)
{
  int64_t rv;

  opera_lr_opts_set_mem_cfg();

  if(g_OPTS.font == NULL)
    {
      opera_mem_rom2_clear();
      return;
    }

  rv = read_file_from_system_directory(g_OPTS.font->filename,ROM2,ROM2_SIZE);
  if(rv < 0)
    {
      retro_log_printf_cb(RETRO_LOG_ERROR,
                          "[Opera]: unable to find or load FONT ROM - %s\n",
                          g_OPTS.font->filename);
      return;
    }

  opera_mem_rom2_byteswap32_if_le();
}

void
opera_lr_opts_get_nvram_shared(void)
{
  g_OPTS.nvram_shared = getval_is_str("nvram_storage",
                                      "shared",
                                      true);
}

void
opera_lr_opts_set_nvram_shared()
{

}

void
opera_lr_opts_get_nvram_version(void)
{
  g_OPTS.nvram_version = getval_as_int("nvram_version",0);
}

void
opera_lr_opts_set_nvram_version()
{

}

void
opera_lr_opts_get_region(void)
{
  const char *val;

  g_OPTS.region = OPERA_REGION_NTSC;

  val = getval("region");
  if(val == NULL)
    return;

  if(!strcmp(val,"ntsc"))
    g_OPTS.region = OPERA_REGION_NTSC;
  else if(!strcmp(val,"pal1"))
    g_OPTS.region = OPERA_REGION_PAL1;
  else if(!strcmp(val,"pal2"))
    g_OPTS.region = OPERA_REGION_PAL2;
}

void
opera_lr_opts_set_region(void)
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

void
opera_lr_opts_get_cpu_overclock(void)
{
  g_OPTS.cpu_overclock = getval_as_float("cpu_overclock",1.0);
}

void
opera_lr_opts_set_cpu_overclock(void)
{
  opera_clock_cpu_set_freq_mul(g_OPTS.cpu_overclock);
}

void
opera_lr_opts_get_vdlp_pixel_format(void)
{
  const char *val;

  g_OPTS.vdlp_pixel_format = VDLP_PIXEL_FORMAT_0RGB1555;
  g_OPTS.video_pitch_shift = 1;

  val = getval("vdlp_pixel_format");
  if(val == NULL)
    return;

  if(!strcmp(val,"XRGB8888"))
    {
      g_OPTS.vdlp_pixel_format = VDLP_PIXEL_FORMAT_XRGB8888;
      g_OPTS.video_pitch_shift = 2;
    }
  else if(!strcmp(val,"RGB565"))
    {
      g_OPTS.vdlp_pixel_format = VDLP_PIXEL_FORMAT_RGB565;
      g_OPTS.video_pitch_shift = 1;
    }
  else if(!strcmp(val,"0RGB1555"))
    {
      g_OPTS.vdlp_pixel_format = VDLP_PIXEL_FORMAT_0RGB1555;
      g_OPTS.video_pitch_shift = 1;
    }
}

void
opera_lr_opts_set_vdlp_pixel_format()
{
  uint32_t flags;

  flags = VDLP_FLAG_NONE;
  if(g_OPTS.high_resolution)
    flags |= VDLP_FLAG_HIRES_CEL;
  if(g_OPTS.vdlp_bypass_clut)
    flags |= VDLP_FLAG_CLUT_BYPASS;

  opera_vdlp_configure(g_OPTS.video_buffer,
                       g_OPTS.vdlp_pixel_format,
                       flags);
}

void
opera_lr_opts_get_vdlp_bypass_clut()
{
  g_OPTS.vdlp_bypass_clut = getval_is_enabled("vdlp_bypass_clut",false);
}

void
opera_lr_opts_set_vdlp_bypass_clut()
{
  uint32_t flags;

  flags = VDLP_FLAG_NONE;
  if(g_OPTS.high_resolution)
    flags |= VDLP_FLAG_HIRES_CEL;
  if(g_OPTS.vdlp_bypass_clut)
    flags |= VDLP_FLAG_CLUT_BYPASS;

  opera_vdlp_configure(g_OPTS.video_buffer,
                       g_OPTS.vdlp_pixel_format,
                       flags);
}

void
opera_lr_opts_get_high_resolution(void)
{
  g_OPTS.high_resolution = getval_is_enabled("high_resolution",false);
}

void
opera_lr_opts_set_high_resolution(void)
{
  uint32_t flags;

  flags = VDLP_FLAG_NONE;
  if(g_OPTS.high_resolution)
    flags |= VDLP_FLAG_HIRES_CEL;
  if(g_OPTS.vdlp_bypass_clut)
    flags |= VDLP_FLAG_CLUT_BYPASS;

  opera_vdlp_configure(g_OPTS.video_buffer,
                       g_OPTS.vdlp_pixel_format,
                       flags);

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
opera_lr_opts_get_active_devices()
{
  unsigned rv;

  g_OPTS.active_devices = getval_as_int("active_devices",1);
  if(g_OPTS.active_devices > LR_INPUT_MAX_DEVICES)
    g_OPTS.active_devices = 1;
}

void
opera_lr_opts_set_active_devices()
{

}

void
opera_lr_opts_get_mem_cfg()
{
  unsigned x;
  const char *val;

  val = getval("mem_capacity");
  if(val == NULL)
    return;

  x = 0;
  sscanf(val,"%x",&x);

  g_OPTS.mem_cfg = (opera_mem_cfg_t)x;
}

void
opera_lr_opts_set_mem_cfg()
{
  opera_mem_init(g_OPTS.mem_cfg);
}

void
opera_lr_opts_get_hide_lightgun_crosshairs()
{
  g_OPTS.hide_lightgun_crosshairs = getval_is_enabled("hide_lightgun_crosshairs",false);
}

void
opera_lr_opts_set_hide_lightgun_crosshairs()
{

}

void
opera_lr_opts_get_madam_matrix_engine(void)
{
  const char *val;

  g_OPTS.madam_matrix_engine = "hardware";

  val = getval("madam_matrix_engine");
  if(val == NULL)
    return;

  if(!strcmp(val,"software"))
    g_OPTS.madam_matrix_engine = "software";
  else
    g_OPTS.madam_matrix_engine = "hardware";
}

void
opera_lr_opts_set_madam_matrix_engine()
{
  if(!strcmp(g_OPTS.madam_matrix_engine,"software"))
    opera_madam_me_mode_software();
  else
    opera_madam_me_mode_hardware();
}

void
opera_lr_opts_get_kprint(void)
{
  g_OPTS.kprint = getval_is_enabled("kprint",false);
}

void
opera_lr_opts_set_kprint(void)
{
  if(g_OPTS.kprint)
    opera_madam_kprint_enable();
  else
    opera_madam_kprint_disable();
}

void
opera_lr_opts_get_dsp_threaded(void)
{
  g_OPTS.dsp_threaded = getval_is_enabled("dsp_threaded",false);
}

void
opera_lr_opts_set_dsp_threaded(void)
{
  opera_lr_dsp_init(g_OPTS.dsp_threaded);
}

void
opera_lr_opts_get_swi_hle()
{
  g_OPTS.swi_hle = getval_is_enabled("swi_hle",false);
}

void
opera_lr_opts_set_swi_hle(void)
{
  opera_arm_swi_hle_set(g_OPTS.swi_hle);
}

static
uint32_t
set_reset_bits(const char *key_,
               uint32_t    input_,
               uint32_t    bitmask_)
{
  return (getval_is_enabled(key_,false) ?
          (input_ |  bitmask_) :
          (input_ & ~bitmask_));
}

void
opera_lr_opts_get_hacks(void)
{
  uint32_t rv;

  rv = 0;
  rv = set_reset_bits("hack_timing_1",rv,FIX_BIT_TIMING_1);
  rv = set_reset_bits("hack_timing_3",rv,FIX_BIT_TIMING_3);
  rv = set_reset_bits("hack_timing_5",rv,FIX_BIT_TIMING_5);
  rv = set_reset_bits("hack_timing_6",rv,FIX_BIT_TIMING_6);
  rv = set_reset_bits("hack_graphics_step_y",rv,FIX_BIT_GRAPHICS_STEP_Y);

  g_OPTS.hack_flags = rv;
}

void
opera_lr_opts_set_hacks()
{
  FIXMODE = g_OPTS.hack_flags;
}

void
opera_lr_opts_get()
{
  opera_lr_opts_get_video_buffer();
  opera_lr_opts_get_bios();
  opera_lr_opts_get_font();
  opera_lr_opts_get_nvram_shared();
  opera_lr_opts_get_nvram_version();
  opera_lr_opts_get_region();
  opera_lr_opts_get_cpu_overclock();
  opera_lr_opts_get_vdlp_pixel_format();
  opera_lr_opts_get_vdlp_bypass_clut();
  opera_lr_opts_get_high_resolution();
  opera_lr_opts_get_active_devices();
  opera_lr_opts_get_mem_cfg();
  opera_lr_opts_get_hide_lightgun_crosshairs();
  opera_lr_opts_get_madam_matrix_engine();
  opera_lr_opts_get_kprint();
  opera_lr_opts_get_dsp_threaded();
  opera_lr_opts_get_swi_hle();
  opera_lr_opts_get_hacks();
}

void
opera_lr_opts_set()
{
  opera_lr_opts_set_video_buffer();
  opera_lr_opts_set_bios();
  opera_lr_opts_set_font();
  opera_lr_opts_set_nvram_shared();
  opera_lr_opts_set_nvram_version();
  opera_lr_opts_set_region();
  opera_lr_opts_set_cpu_overclock();
  opera_lr_opts_set_vdlp_pixel_format();
  opera_lr_opts_set_vdlp_bypass_clut();
  opera_lr_opts_set_high_resolution();
  opera_lr_opts_set_active_devices();
  opera_lr_opts_set_mem_cfg();
  opera_lr_opts_set_hide_lightgun_crosshairs();
  opera_lr_opts_set_madam_matrix_engine();
  opera_lr_opts_set_kprint();
  opera_lr_opts_set_dsp_threaded();
  opera_lr_opts_set_swi_hle();
  opera_lr_opts_set_hacks();
}

void
opera_lr_opts_destroy()
{
  if(g_OPTS.video_buffer)
    free(g_OPTS.video_buffer);
  g_OPTS.video_buffer = NULL;

  opera_lr_dsp_destroy();

  opera_mem_destroy();
}
