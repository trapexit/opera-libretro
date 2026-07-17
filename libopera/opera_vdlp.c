#include "flags.h"
#include "inline.h"

#include "boolean.h"

#include "opera_arm.h"
#include "opera_core.h"
#include "opera_madam.h"
#include "opera_mem.h"
#include "opera_region.h"
#include "opera_state.h"
#include "opera_vdl.h"
#include "opera_vdlp.h"
#include "opera_vdlp_i.h"

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/*
  VDLP options TODO
  - full cornerweight interpolation
  - add pseudo random 3bit pattern for second clut bypass mode
*/

typedef void(*vdlp_renderer_t)();

static vdlp_t               g_VDLP         = {0};
static void                *g_BUF          = NULL;
static void                *g_CURBUF       = NULL;
static vdlp_renderer_t      g_RENDERER     = NULL;
static uint32_t             g_FLAGS        = VDLP_FLAG_NONE;
static vdlp_pixel_format_e  g_PIXEL_FORMAT = VDLP_PIXEL_FORMAT_RGB565;

/*
  Derived custom-CLUT cache; it is not part of emulated or saved
  state.
*/
static uint32_t g_CLUT_RG[CLUT_LEN * CLUT_LEN];
static uint32_t g_CLUT_B[CLUT_LEN];
static uint8_t  g_PACKED_CLUT_R[CLUT_LEN];
static uint8_t  g_PACKED_CLUT_G[CLUT_LEN];
static uint8_t  g_PACKED_CLUT_B[CLUT_LEN];
static bool     g_PACKED_CLUT_VALID               = false;
static bool     g_PACKED_CLUT_DEFERRED            = false;
static uint32_t g_CLUT_GENERATION                 = 0;
static uint32_t g_PACKED_CLUT_GENERATION          = 0;
static uint32_t g_PACKED_CLUT_DEFERRED_GENERATION = 0;

static const uint32_t PIXELS_PER_LINE_MODULO[8] =
  {320, 384, 512, 640, 1024, 320, 320, 320};

/* The FBA modulo is a source stride and may exceed the visible region. */
static
INLINE
uint32_t
vdlp_visible_width(void)
{
  uint32_t modulo;
  uint32_t region_width;

  modulo = PIXELS_PER_LINE_MODULO[g_VDLP.clut_ctrl.cdcw.fba_incr_modulo];
  region_width = opera_region_width();

  return ((modulo < region_width) ? modulo : region_width);
}

static
INLINE
bool
madam_clut_dma_enabled(void)
{
  return flag_is_set(opera_madam_mctl(),MADAM_MCTL_CLUTXEN);
}

static
INLINE
bool
madam_video_dma_enabled(void)
{
  return flag_is_set(opera_madam_mctl(),MADAM_MCTL_VSCTXEN);
}

static
INLINE
bool
bitmap_dma_enabled(void)
{
  return (madam_video_dma_enabled() &&
          g_VDLP.clut_ctrl.cdcw.enable_dma);
}

/*
  TODO: why the "& VRAM_SIZE_MASK"?  The addresses shouldn't be
  outside the RAM space but are at times.
*/
static
INLINE
uint32_t
vram_read32(const uint32_t addr_)
{
  return *(uint32_t const * const)&VRAM[addr_ & VRAM_SIZE_MASK];
}

static
INLINE
void
vram_write32(const uint32_t addr_,
             const uint32_t val_)
{
  *((uint32_t * const)&VRAM[addr_ & VRAM_SIZE_MASK]) = val_;
}

static
INLINE
uint32_t
vdl_read(const uint32_t off_)
{
  return vram_read32(g_VDLP.curr_vdl + (off_ << 2));
}

static
void
vdl_set_clut(const vdl_ctrl_word_u cmd_)
{
  switch(cmd_.cvw.rgb_enable)
    {
    case 0x0:
      g_VDLP.clut_r[cmd_.cvw.addr] = cmd_.cvw.r;
      g_VDLP.clut_b[cmd_.cvw.addr] = cmd_.cvw.b;
      g_VDLP.clut_g[cmd_.cvw.addr] = cmd_.cvw.g;
      break;
    case 0x3:
      g_VDLP.clut_r[cmd_.cvw.addr] = cmd_.cvw.r;
      break;
    case 0x1:
      g_VDLP.clut_b[cmd_.cvw.addr] = cmd_.cvw.b;
      break;
    case 0x2:
      g_VDLP.clut_g[cmd_.cvw.addr] = cmd_.cvw.g;
      break;
    }
}

static
void
vdlp_process_optional_cmds(const int ctrl_word_cnt_)
{
  int i;
  int colors_only;
  vdl_ctrl_word_u cmd;

  colors_only = 0;
  for(i = 0; i < ctrl_word_cnt_; i++)
    {
      cmd.raw = vdl_read(i);
      switch((cmd.raw & 0xE0000000) >> 29)
        {
        case 0x0:
        case 0x1:
        case 0x2:
        case 0x3:
          vdl_set_clut(cmd);
          break;
        case 0x4:
        case 0x5:
          /*
            Page 17 of US Patent 5,502,462

            If bit 31 of an optional color/control word is one, and if
            bit 30 is zero (10X), then the word contains control
            information for an audio/video output circuit 105 (not
            detailed herein) of the system. The audio/video processor
            circuitry receives this word over the S-bus 123, and
            forwards in to the audio/video output circuitry for
            processing.
          */
          break;
        case 0x6:
          if(colors_only)
            continue;
          g_VDLP.disp_ctrl.raw = cmd.raw;
          colors_only = cmd.dcw.colors_only;
          break;
        case 0x7:
          g_VDLP.bg_color.raw = cmd.raw;
          break;
        }
    }
}

static
void
vdlp_process_vdl_entry(void)
{
  uint32_t entry;
  uint32_t next_entry;
  clut_dma_ctrl_word_s *cdcw = &g_VDLP.clut_ctrl.cdcw;

  entry = vram_read32(g_VDLP.curr_vdl);
  if(!entry)
    return;

  g_VDLP.clut_ctrl.raw = entry;

  if(cdcw->curr_fba_override)
    g_VDLP.curr_bmp = vdl_read(1);

  if(cdcw->prev_fba_override)
    g_VDLP.prev_bmp = vdl_read(2);

  next_entry = vdl_read(3);
  if(cdcw->next_vdl_addr_rel)
    next_entry += (g_VDLP.curr_vdl + (4 * sizeof(uint32_t)));

  g_VDLP.curr_vdl += (4 * sizeof(uint32_t));

  vdlp_process_optional_cmds(cdcw->ctrl_word_cnt);
  /* Only interpolated custom-CLUT rendering consumes the derived cache. */
  if(cdcw->vertical_mode && g_VDLP.disp_ctrl.dcw.vi_on)
    g_CLUT_GENERATION++;

  g_VDLP.curr_vdl = next_entry;
  g_VDLP.line_cnt = cdcw->persist_len;
}

static
void
vdlp_render_line_black_hires(const uint32_t width_,
                             const uint32_t bytes_per_pixel_)
{
  uint8_t *dst;
  uint32_t len;

  dst = g_CURBUF;
  len = (width_ * bytes_per_pixel_ * 2 * 2);

  memset(dst,0,len);

  g_CURBUF = (dst + len);
}

static
INLINE
uint32_t
duplicate_pixel_16(const uint16_t pixel_)
{
  return ((uint32_t)pixel_ | ((uint32_t)pixel_ << 16));
}

static
INLINE
uint64_t
duplicate_pixel_32(const uint32_t pixel_)
{
  return ((uint64_t)pixel_ | ((uint64_t)pixel_ << 32));
}

static
INLINE
uint32_t
vdlp_next_bitmap_line(const uint32_t fba_)
{
  uint32_t modulo;

  modulo = PIXELS_PER_LINE_MODULO[g_VDLP.clut_ctrl.cdcw.fba_incr_modulo];

  return (fba_ + ((fba_ & 2) ? ((modulo << 2) - 2) : 2));
}

static
INLINE
bool
vdlp_vertical_interpolation_enabled(const bool fixed_clut_)
{
  if(!g_VDLP.clut_ctrl.cdcw.vertical_mode ||
     g_VDLP.disp_ctrl.dcw.vi_off_1_line)
    return false;

  return (fixed_clut_ ? g_VDLP.disp_ctrl.dcw.window_vi_on :
          g_VDLP.disp_ctrl.dcw.vi_on);
}

static
INLINE
bool
vdlp_pixel_uses_fixed_clut(const uint16_t pixel_)
{
  return (g_VDLP.disp_ctrl.dcw.clut_bypass && (pixel_ & 0x8000));
}

static
INLINE
uint32_t
average_XRGB8888(const uint32_t a_,
                 const uint32_t b_)
{
  return ((a_ & b_) + (((a_ ^ b_) & 0x00FEFEFE) >> 1));
}

static
void
vdlp_render_line_vertical(const vdlp_pixel_format_e pixel_format_,
                          const bool                force_fixed_clut_);

static
void
vdlp_render_line_vertical_RGB565_custom(void);

static
INLINE
uint16_t
fixed_clut_to_RGB565(const uint16_t p_)
{
  return (((p_ & 0x7FE0) << 1) | (p_ & 0x001F));
}

static
INLINE
uint16_t
user_clut_to_RGB565(const uint16_t p_)
{
  return (((g_VDLP.clut_r[(p_ >> 0xA) & 0x1F] >> 3) << 0xB) |
          ((g_VDLP.clut_g[(p_ >> 0x5) & 0x1F] >> 2) << 0x5) |
          ((g_VDLP.clut_b[(p_ >> 0x0) & 0x1F] >> 3) << 0x0));
}

static
INLINE
uint16_t
background_to_RGB565(void)
{
  return (((g_VDLP.bg_color.bvw.r >> 3) << 0xB) |
          ((g_VDLP.bg_color.bvw.g >> 2) << 0x5) |
          ((g_VDLP.bg_color.bvw.b >> 3) << 0x0));
}

static
INLINE
uint16_t
vdlp_render_pixel_RGB565(const uint16_t p_)
{
  if(p_ == 0)
    return background_to_RGB565();

  return user_clut_to_RGB565(p_);
}

static
INLINE
uint16_t
vdlp_render_pixel_RGB565_bypass_clut(const uint16_t p_)
{
  if(p_ == 0)
    return background_to_RGB565();

  if(p_ & 0x8000)
    return fixed_clut_to_RGB565(p_);

  return user_clut_to_RGB565(p_);
}

static
void
vdlp_render_line_RGB565(void)
{
  int x;
  int width;
  uint16_t pixel;
  uint32_t pair;
  uint32_t *src;
  uint32_t *dst0;
  uint32_t *dst1;

  width = vdlp_visible_width();

  if(!bitmap_dma_enabled())
    {
      vdlp_render_line_black_hires(width,sizeof(uint16_t));
      return;
    }

  if(g_VDLP.clut_ctrl.cdcw.vertical_mode)
    {
      if(!g_VDLP.disp_ctrl.dcw.clut_bypass)
        vdlp_render_line_vertical_RGB565_custom();
      else
        vdlp_render_line_vertical(VDLP_PIXEL_FORMAT_RGB565,false);
      return;
    }

  dst0 = g_CURBUF;
  dst1 = &dst0[width];
  src = (uint32_t*)&VRAM[(g_VDLP.curr_bmp ^ 2) & VRAM_SIZE_MASK];
  if(!g_VDLP.disp_ctrl.dcw.clut_bypass)
    {
      for(x = 0; x < width; x++)
        {
          pixel = vdlp_render_pixel_RGB565(*(uint16_t*)&src[x]);
          pair = duplicate_pixel_16(pixel);
          *dst0++ = pair;
          *dst1++ = pair;
        }
    }
  else
    {
      for(x = 0; x < width; x++)
        {
          pixel = vdlp_render_pixel_RGB565_bypass_clut(*(uint16_t*)&src[x]);
          pair = duplicate_pixel_16(pixel);
          *dst0++ = pair;
          *dst1++ = pair;
        }
    }

  g_CURBUF = dst1;
}

static
void
vdlp_render_line_RGB565_bypass_clut(void)
{
  int x;
  int width;
  uint16_t pixel;
  uint32_t pair;
  uint32_t *src;
  uint32_t *dst0;
  uint32_t *dst1;

  width = vdlp_visible_width();

  if(!bitmap_dma_enabled())
    {
      vdlp_render_line_black_hires(width,sizeof(uint16_t));
      return;
    }

  if(g_VDLP.clut_ctrl.cdcw.vertical_mode)
    {
      vdlp_render_line_vertical(VDLP_PIXEL_FORMAT_RGB565,true);
      return;
    }

  dst0 = g_CURBUF;
  dst1 = &dst0[width];
  src = (uint32_t*)&VRAM[(g_VDLP.curr_bmp ^ 2) & VRAM_SIZE_MASK];
  for(x = 0; x < width; x++)
    {
      pixel = fixed_clut_to_RGB565(*(uint16_t*)&src[x]);
      pair = duplicate_pixel_16(pixel);
      *dst0++ = pair;
      *dst1++ = pair;
    }

  g_CURBUF = dst1;
}

static
void
vdlp_render_line_RGB565_hires(void)
{
  int x;
  int width;
  uint16_t *dst0;
  uint16_t *dst1;
  uint32_t *src0;
  uint32_t *src1;
  uint32_t *src2;
  uint32_t *src3;

  if(g_VDLP.clut_ctrl.cdcw.vertical_mode)
    {
      vdlp_render_line_RGB565();
      return;
    }

  width = vdlp_visible_width();

  if(!bitmap_dma_enabled())
    {
      vdlp_render_line_black_hires(width,sizeof(uint16_t));
      return;
    }

  dst0 = g_CURBUF;
  dst1 = &dst0[width << 1];
  src0 = (uint32_t*)&VRAM[(g_VDLP.curr_bmp ^ 2) & VRAM_SIZE_MASK];
  src1 = &src0[VRAM_SIZE / sizeof(uint32_t)];
  src2 = &src1[VRAM_SIZE / sizeof(uint32_t)];
  src3 = &src2[VRAM_SIZE / sizeof(uint32_t)];
  if(!g_VDLP.disp_ctrl.dcw.clut_bypass)
    {
      for(x = 0; x < width; x++)
        {
          *dst0++ = vdlp_render_pixel_RGB565(*(uint16_t*)&src0[x]);
          *dst0++ = vdlp_render_pixel_RGB565(*(uint16_t*)&src1[x]);
          *dst1++ = vdlp_render_pixel_RGB565(*(uint16_t*)&src2[x]);
          *dst1++ = vdlp_render_pixel_RGB565(*(uint16_t*)&src3[x]);
        }
    }
  else
    {
      for(x = 0; x < width; x++)
        {
          *dst0++ = vdlp_render_pixel_RGB565_bypass_clut(*(uint16_t*)&src0[x]);
          *dst0++ = vdlp_render_pixel_RGB565_bypass_clut(*(uint16_t*)&src1[x]);
          *dst1++ = vdlp_render_pixel_RGB565_bypass_clut(*(uint16_t*)&src2[x]);
          *dst1++ = vdlp_render_pixel_RGB565_bypass_clut(*(uint16_t*)&src3[x]);
        }
    }

  g_CURBUF = dst1;
}

static
void
vdlp_render_line_RGB565_hires_bypass_clut(void)
{
  int x;
  int width;
  uint16_t *dst0;
  uint16_t *dst1;
  uint32_t *src0;
  uint32_t *src1;
  uint32_t *src2;
  uint32_t *src3;

  if(g_VDLP.clut_ctrl.cdcw.vertical_mode)
    {
      vdlp_render_line_RGB565_bypass_clut();
      return;
    }

  width = vdlp_visible_width();

  if(!bitmap_dma_enabled())
    {
      vdlp_render_line_black_hires(width,sizeof(uint16_t));
      return;
    }

  dst0 = g_CURBUF;
  dst1 = &dst0[width << 1];
  src0 = (uint32_t*)&VRAM[(g_VDLP.curr_bmp ^ 2) & VRAM_SIZE_MASK];
  src1 = &src0[VRAM_SIZE / sizeof(uint32_t)];
  src2 = &src1[VRAM_SIZE / sizeof(uint32_t)];
  src3 = &src2[VRAM_SIZE / sizeof(uint32_t)];
  for(x = 0; x < width; x++)
    {
      *dst0++ = fixed_clut_to_RGB565(*(uint16_t*)&src0[x]);
      *dst0++ = fixed_clut_to_RGB565(*(uint16_t*)&src1[x]);
      *dst1++ = fixed_clut_to_RGB565(*(uint16_t*)&src2[x]);
      *dst1++ = fixed_clut_to_RGB565(*(uint16_t*)&src3[x]);
    }

  g_CURBUF = dst1;
}

static
INLINE
uint32_t
fixed_clut_to_XRGB8888(const uint16_t p_)
{
  return (((p_ & 0x7C00) << 0x9) |
          ((p_ & 0x03E0) << 0x6) |
          ((p_ & 0x001F) << 0x3));
}

static
INLINE
uint32_t
user_clut_to_XRGB8888(const uint16_t p_)
{
  return ((g_VDLP.clut_r[(p_ >> 0xA) & 0x1F] << 0x10) |
          (g_VDLP.clut_g[(p_ >> 0x5) & 0x1F] << 0x08) |
          (g_VDLP.clut_b[(p_ >> 0x0) & 0x1F] << 0x00));
}

static
INLINE
uint32_t
vdlp_render_pixel_XRGB8888(const uint16_t p_)
{
  if(p_ == 0)
    return g_VDLP.bg_color.raw;

  return user_clut_to_XRGB8888(p_);
}

static
INLINE
uint32_t
vdlp_render_pixel_XRGB8888_bypass_clut(const uint16_t p_)
{
  if(p_ == 0)
    return g_VDLP.bg_color.raw;

  if(p_ & 0x8000)
    return fixed_clut_to_XRGB8888(p_);

  return user_clut_to_XRGB8888(p_);
}

static
INLINE
uint32_t
vdlp_decode_pixel_XRGB8888(const uint16_t pixel_,
                           const bool     force_fixed_clut_)
{
  if(force_fixed_clut_)
    return fixed_clut_to_XRGB8888(pixel_);

  if(g_VDLP.disp_ctrl.dcw.clut_bypass)
    return vdlp_render_pixel_XRGB8888_bypass_clut(pixel_);

  return vdlp_render_pixel_XRGB8888(pixel_);
}

static
bool
vdlp_prepare_packed_custom_lut(void)
{
  uint32_t r;
  uint32_t g;

  if(g_PACKED_CLUT_VALID)
    {
      if(g_PACKED_CLUT_GENERATION == g_CLUT_GENERATION)
        return true;

      if(!memcmp(g_PACKED_CLUT_R,g_VDLP.clut_r,CLUT_LEN) &&
         !memcmp(g_PACKED_CLUT_G,g_VDLP.clut_g,CLUT_LEN) &&
         !memcmp(g_PACKED_CLUT_B,g_VDLP.clut_b,CLUT_LEN))
        {
          g_PACKED_CLUT_GENERATION = g_CLUT_GENERATION;
          g_PACKED_CLUT_DEFERRED = false;
          return true;
        }

      /* Avoid rebuilding every line when a VDL animates its custom CLUT. */
      if(!g_PACKED_CLUT_DEFERRED ||
         (g_PACKED_CLUT_DEFERRED_GENERATION != g_CLUT_GENERATION))
        {
          g_PACKED_CLUT_DEFERRED = true;
          g_PACKED_CLUT_DEFERRED_GENERATION = g_CLUT_GENERATION;
          return false;
        }
    }

  for(r = 0; r < CLUT_LEN; r++)
    for(g = 0; g < CLUT_LEN; g++)
      g_CLUT_RG[(r << 5) | g] =
        ((g_VDLP.clut_r[r] << 16) | (g_VDLP.clut_g[g] << 8));

  for(g = 0; g < CLUT_LEN; g++)
    g_CLUT_B[g] = g_VDLP.clut_b[g];

  memcpy(g_PACKED_CLUT_R,g_VDLP.clut_r,CLUT_LEN);
  memcpy(g_PACKED_CLUT_G,g_VDLP.clut_g,CLUT_LEN);
  memcpy(g_PACKED_CLUT_B,g_VDLP.clut_b,CLUT_LEN);
  g_PACKED_CLUT_VALID = true;
  g_PACKED_CLUT_DEFERRED = false;
  g_PACKED_CLUT_GENERATION = g_CLUT_GENERATION;

  return true;
}

/* Decode two custom-CLUT pixels with two packed lookups each. */
static
INLINE
uint32_t
vdlp_average_custom_XRGB8888(const uint16_t a_,
                             const uint16_t b_)
{
  uint32_t a;
  uint32_t b;

  if((a_ == 0) || (b_ == 0))
    return average_XRGB8888(vdlp_render_pixel_XRGB8888(a_),
                            vdlp_render_pixel_XRGB8888(b_));

  a = (g_CLUT_RG[(a_ >> 5) & 0x3FF] | g_CLUT_B[a_ & 0x1F]);
  b = (g_CLUT_RG[(b_ >> 5) & 0x3FF] | g_CLUT_B[b_ & 0x1F]);

  return average_XRGB8888(a,b);
}

/* Render the two custom-CLUT source rows without unused interpolation work. */
static
void
vdlp_render_line_vertical_XRGB8888_custom_rows(void)
{
  int x;
  int width;
  uint32_t pixel;
  uint32_t pixel_other;
  uint32_t const *src;
  uint32_t const *next;
  uint64_t *dst0;
  uint64_t *dst1;

  width = vdlp_visible_width();
  src = (uint32_t const *)&VRAM[(g_VDLP.curr_bmp ^ 2) & VRAM_SIZE_MASK];
  next = (uint32_t const *)&VRAM[(vdlp_next_bitmap_line(g_VDLP.curr_bmp) ^ 2) &
                                 VRAM_SIZE_MASK];
  dst0 = g_CURBUF;
  dst1 = &dst0[width];

  for(x = 0; x < width; x++)
    {
      pixel = vdlp_render_pixel_XRGB8888(*(uint16_t*)&src[x]);
      pixel_other = vdlp_render_pixel_XRGB8888(*(uint16_t*)&next[x]);
      *dst0++ = duplicate_pixel_32(pixel);
      *dst1++ = duplicate_pixel_32(pixel_other);
    }

  g_CURBUF = dst1;
}

static
INLINE
uint16_t
xrgb8888_to_RGB565(const uint32_t pixel_)
{
  return ((((pixel_ >> 19) & 0x1F) << 11) |
          (((pixel_ >> 10) & 0x3F) <<  5) |
          (((pixel_ >>  3) & 0x1F) <<  0));
}

/* Keep invariant custom-CLUT, interpolation, and 16-bit format branches out of
   the per-pixel loops. */
static
void
vdlp_render_line_vertical_RGB565_custom(void)
{
  int x;
  int width;
  uint16_t pixel;
  uint32_t color;
  uint32_t pair;
  uint32_t const *src;
  uint32_t const *other;
  uint32_t *dst0;
  uint32_t *dst1;

  width = vdlp_visible_width();
  src = (uint32_t const *)&VRAM[(g_VDLP.curr_bmp ^ 2) & VRAM_SIZE_MASK];
  dst0 = g_CURBUF;
  dst1 = &dst0[width];

  if(vdlp_vertical_interpolation_enabled(false))
    {
      if(!vdlp_prepare_packed_custom_lut())
        {
          vdlp_render_line_vertical(VDLP_PIXEL_FORMAT_RGB565,false);
          return;
        }
      other = (uint32_t const *)&VRAM[(g_VDLP.prev_bmp ^ 2) & VRAM_SIZE_MASK];
      for(x = 0; x < width; x++)
        {
          color = vdlp_average_custom_XRGB8888(*(uint16_t*)&src[x],
                                               *(uint16_t*)&other[x]);
          pixel = xrgb8888_to_RGB565(color);
          pair = duplicate_pixel_16(pixel);
          *dst0++ = pair;
          *dst1++ = pair;
        }
    }
  else
    {
      other = (uint32_t const *)&VRAM[
        (vdlp_next_bitmap_line(g_VDLP.curr_bmp) ^ 2) & VRAM_SIZE_MASK];
      for(x = 0; x < width; x++)
        {
          *dst0++ = duplicate_pixel_16(
            vdlp_render_pixel_RGB565(*(uint16_t*)&src[x]));
          *dst1++ = duplicate_pixel_16(
            vdlp_render_pixel_RGB565(*(uint16_t*)&other[x]));
        }
    }

  g_CURBUF = dst1;
}

/*
  In 480-line mode each field line consumes two bitmap rows. With vertical
  interpolation enabled, the display generator averages the current and
  previous rows; pseudo-24-bit images use this to reconstruct each original
  8-bit color component. Without interpolation, expose both rows in the
  progressive output so all 480 source lines remain visible.
*/
static
void
vdlp_render_line_vertical(const vdlp_pixel_format_e pixel_format_,
                          const bool                force_fixed_clut_)
{
  int x;
  int width;
  bool fixed_clut;
  bool fixed_clut_other;
  bool interpolate;
  uint16_t raw;
  uint16_t raw_other;
  uint16_t pixel16;
  uint16_t pixel16_other;
  uint32_t pixel;
  uint32_t pixel_other;
  uint8_t *out;
  uint32_t const *src;
  uint32_t const *prev;
  uint32_t const *next;
  uint32_t *dst16_0;
  uint32_t *dst16_1;
  uint64_t *dst32_0;
  uint64_t *dst32_1;

  width = vdlp_visible_width();
  out = g_CURBUF;
  src = (uint32_t const *)&VRAM[(g_VDLP.curr_bmp ^ 2) & VRAM_SIZE_MASK];
  prev = (uint32_t const *)&VRAM[(g_VDLP.prev_bmp ^ 2) & VRAM_SIZE_MASK];
  dst16_0 = (uint32_t*)out;
  dst16_1 = &dst16_0[width];
  dst32_0 = (uint64_t*)out;
  dst32_1 = &dst32_0[width];

  /* Hoist the invariant format, CLUT-path, and interpolation decisions. */
  if((pixel_format_ == VDLP_PIXEL_FORMAT_XRGB8888) &&
     !force_fixed_clut_ &&
     !g_VDLP.disp_ctrl.dcw.clut_bypass &&
     vdlp_vertical_interpolation_enabled(false) &&
     vdlp_prepare_packed_custom_lut())
    {
      for(x = 0; x < width; x++)
        {
          raw = *(uint16_t*)&src[x];
          raw_other = *(uint16_t*)&prev[x];
          pixel = vdlp_average_custom_XRGB8888(raw,raw_other);
          *dst32_0++ = duplicate_pixel_32(pixel);
          *dst32_1++ = duplicate_pixel_32(pixel);
        }

      g_CURBUF = (out + (width << 4));
      return;
    }

  next = (uint32_t const *)&VRAM[(vdlp_next_bitmap_line(g_VDLP.curr_bmp) ^ 2) &
                                 VRAM_SIZE_MASK];
  for(x = 0; x < width; x++)
    {
      raw = *(uint16_t*)&src[x];
      raw_other = *(uint16_t*)&prev[x];
      fixed_clut = (force_fixed_clut_ || vdlp_pixel_uses_fixed_clut(raw));
      fixed_clut_other =
        (force_fixed_clut_ || vdlp_pixel_uses_fixed_clut(raw_other));
      interpolate =
        (vdlp_vertical_interpolation_enabled(fixed_clut) ||
         vdlp_vertical_interpolation_enabled(fixed_clut_other));
      pixel = vdlp_decode_pixel_XRGB8888(raw,force_fixed_clut_);

      if(interpolate)
        {
          pixel_other =
            vdlp_decode_pixel_XRGB8888(raw_other,force_fixed_clut_);
          pixel = average_XRGB8888(pixel,pixel_other);
          pixel_other = pixel;
        }
      else
        {
          raw_other = *(uint16_t*)&next[x];
          pixel_other =
            vdlp_decode_pixel_XRGB8888(raw_other,force_fixed_clut_);
        }

      switch(pixel_format_)
        {
        case VDLP_PIXEL_FORMAT_RGB565:
          pixel16 = xrgb8888_to_RGB565(pixel);
          pixel16_other = xrgb8888_to_RGB565(pixel_other);
          *dst16_0++ = duplicate_pixel_16(pixel16);
          *dst16_1++ = duplicate_pixel_16(pixel16_other);
          break;
        case VDLP_PIXEL_FORMAT_XRGB8888:
          *dst32_0++ = duplicate_pixel_32(pixel);
          *dst32_1++ = duplicate_pixel_32(pixel_other);
          break;
        }
    }

  g_CURBUF = (out + (width << ((pixel_format_ == VDLP_PIXEL_FORMAT_XRGB8888) ?
                               4 : 3)));
}

static
void
vdlp_render_line_XRGB8888(void)
{
  int x;
  int width;
  uint32_t pixel;
  uint64_t pair;
  uint32_t *src;
  uint64_t *dst0;
  uint64_t *dst1;

  width = vdlp_visible_width();

  if(!bitmap_dma_enabled())
    {
      vdlp_render_line_black_hires(width,sizeof(uint32_t));
      return;
    }

  if(g_VDLP.clut_ctrl.cdcw.vertical_mode)
    {
      if(!g_VDLP.disp_ctrl.dcw.clut_bypass &&
         !vdlp_vertical_interpolation_enabled(false))
        vdlp_render_line_vertical_XRGB8888_custom_rows();
      else
        vdlp_render_line_vertical(VDLP_PIXEL_FORMAT_XRGB8888,false);
      return;
    }

  dst0 = g_CURBUF;
  dst1 = &dst0[width];
  src = (uint32_t*)&VRAM[(g_VDLP.curr_bmp ^ 2) & VRAM_SIZE_MASK];
  if(!g_VDLP.disp_ctrl.dcw.clut_bypass)
    {
      for(x = 0; x < width; x++)
        {
          pixel = vdlp_render_pixel_XRGB8888(*(uint16_t*)&src[x]);
          pair = duplicate_pixel_32(pixel);
          *dst0++ = pair;
          *dst1++ = pair;
        }
    }
  else
    {
      for(x = 0; x < width; x++)
        {
          pixel = vdlp_render_pixel_XRGB8888_bypass_clut(*(uint16_t*)&src[x]);
          pair = duplicate_pixel_32(pixel);
          *dst0++ = pair;
          *dst1++ = pair;
        }
    }

  g_CURBUF = dst1;
}

static
void
vdlp_render_line_XRGB8888_bypass_clut(void)
{
  int x;
  int width;
  uint32_t pixel;
  uint64_t pair;
  uint32_t *src;
  uint64_t *dst0;
  uint64_t *dst1;

  width = vdlp_visible_width();

  if(!bitmap_dma_enabled())
    {
      vdlp_render_line_black_hires(width,sizeof(uint32_t));
      return;
    }

  if(g_VDLP.clut_ctrl.cdcw.vertical_mode)
    {
      vdlp_render_line_vertical(VDLP_PIXEL_FORMAT_XRGB8888,true);
      return;
    }

  dst0 = g_CURBUF;
  dst1 = &dst0[width];
  src = (uint32_t*)&VRAM[(g_VDLP.curr_bmp ^ 2) & VRAM_SIZE_MASK];
  for(x = 0; x < width; x++)
    {
      pixel = fixed_clut_to_XRGB8888(*(uint16_t*)&src[x]);
      pair = duplicate_pixel_32(pixel);
      *dst0++ = pair;
      *dst1++ = pair;
    }

  g_CURBUF = dst1;
}

static
void
vdlp_render_line_XRGB8888_hires(void)
{
  int x;
  int width;
  uint32_t *dst0;
  uint32_t *dst1;
  uint32_t *src0;
  uint32_t *src1;
  uint32_t *src2;
  uint32_t *src3;

  if(g_VDLP.clut_ctrl.cdcw.vertical_mode)
    {
      vdlp_render_line_XRGB8888();
      return;
    }

  width = vdlp_visible_width();

  if(!bitmap_dma_enabled())
    {
      vdlp_render_line_black_hires(width,sizeof(uint32_t));
      return;
    }

  dst0 = g_CURBUF;
  dst1 = &dst0[width << 1];
  src0 = (uint32_t*)&VRAM[(g_VDLP.curr_bmp ^ 2) & VRAM_SIZE_MASK];
  src1 = &src0[VRAM_SIZE / sizeof(uint32_t)];
  src2 = &src1[VRAM_SIZE / sizeof(uint32_t)];
  src3 = &src2[VRAM_SIZE / sizeof(uint32_t)];
  if(!g_VDLP.disp_ctrl.dcw.clut_bypass)
    {
      for(x = 0; x < width; x++)
        {
          *dst0++ = vdlp_render_pixel_XRGB8888(*(uint16_t*)&src0[x]);
          *dst0++ = vdlp_render_pixel_XRGB8888(*(uint16_t*)&src1[x]);
          *dst1++ = vdlp_render_pixel_XRGB8888(*(uint16_t*)&src2[x]);
          *dst1++ = vdlp_render_pixel_XRGB8888(*(uint16_t*)&src3[x]);
        }
    }
  else
    {
      for(x = 0; x < width; x++)
        {
          *dst0++ = vdlp_render_pixel_XRGB8888_bypass_clut(*(uint16_t*)&src0[x]);
          *dst0++ = vdlp_render_pixel_XRGB8888_bypass_clut(*(uint16_t*)&src1[x]);
          *dst1++ = vdlp_render_pixel_XRGB8888_bypass_clut(*(uint16_t*)&src2[x]);
          *dst1++ = vdlp_render_pixel_XRGB8888_bypass_clut(*(uint16_t*)&src3[x]);
        }
    }

  g_CURBUF = dst1;
}

static
void
vdlp_render_line_XRGB8888_hires_bypass_clut(void)
{
  int x;
  int width;
  uint32_t *dst0;
  uint32_t *dst1;
  uint32_t *src0;
  uint32_t *src1;
  uint32_t *src2;
  uint32_t *src3;

  if(g_VDLP.clut_ctrl.cdcw.vertical_mode)
    {
      vdlp_render_line_XRGB8888_bypass_clut();
      return;
    }

  width = vdlp_visible_width();

  if(!bitmap_dma_enabled())
    {
      vdlp_render_line_black_hires(width,sizeof(uint32_t));
      return;
    }

  dst0 = g_CURBUF;
  dst1 = &dst0[width << 1];
  src0 = (uint32_t*)&VRAM[(g_VDLP.curr_bmp ^ 2) & VRAM_SIZE_MASK];
  src1 = &src0[VRAM_SIZE / sizeof(uint32_t)];
  src2 = &src1[VRAM_SIZE / sizeof(uint32_t)];
  src3 = &src2[VRAM_SIZE / sizeof(uint32_t)];
  for(x = 0; x < width; x++)
    {
      *dst0++ = fixed_clut_to_XRGB8888(*(uint16_t*)&src0[x]);
      *dst0++ = fixed_clut_to_XRGB8888(*(uint16_t*)&src1[x]);
      *dst1++ = fixed_clut_to_XRGB8888(*(uint16_t*)&src2[x]);
      *dst1++ = fixed_clut_to_XRGB8888(*(uint16_t*)&src3[x]);
    }

  g_CURBUF = dst1;
}


/* tick / increment frame buffer address */
static
INLINE
uint32_t
tick_fba(const uint32_t fba_)
{
  uint32_t modulo;

  if(g_VDLP.clut_ctrl.cdcw.vertical_mode)
    {
      /* Two bitmap-line increments collapse to one full modulo step. */
      modulo = PIXELS_PER_LINE_MODULO[g_VDLP.clut_ctrl.cdcw.fba_incr_modulo];
      return (fba_ + (modulo << 2));
    }

  return vdlp_next_bitmap_line(fba_);
}

static
INLINE
int
visible_scanline(const int line_)
{
  return ((line_ >= opera_region_start_scanline()) &&
          (line_  < opera_region_end_scanline()));
}

/*
  See ppgfldr/ggsfldr/gpgfldr/2gpgb.html for details on the frame
  buffer layout.

  320x240 by 16bit stored in "left/right" pairs. High order 16bits
  represent X,Y and low order bits represent X,Y+1. Starting from
  0,0 and going left to right and top to bottom to 319,239.
*/
void
opera_vdlp_process_line(int line_)
{
  int y;

  if(line_ < 5)
    return;

  if(line_ == 5)
    {
      g_CURBUF = g_BUF;
      g_VDLP.curr_vdl = g_VDLP.head_vdl;
      if(madam_clut_dma_enabled())
        vdlp_process_vdl_entry();
    }

  if(madam_clut_dma_enabled() && (g_VDLP.line_cnt <= 0))
    vdlp_process_vdl_entry();

  if(visible_scanline(line_))
    g_RENDERER();

  g_VDLP.prev_bmp = ((g_VDLP.clut_ctrl.cdcw.prev_fba_tick) ?
                     tick_fba(g_VDLP.prev_bmp) : g_VDLP.curr_bmp);
  g_VDLP.curr_bmp = tick_fba(g_VDLP.curr_bmp);

  g_VDLP.disp_ctrl.dcw.vi_off_1_line = 0;
  if(g_VDLP.line_cnt > 0)
    g_VDLP.line_cnt--;
}


void
opera_vdlp_init()
{
  uint32_t i;
  void *buf;
  vdlp_renderer_t renderer;
  uint32_t flags;
  vdlp_pixel_format_e pixel_format;
  static const uint32_t StartupVDL[]=
    { /* Startup VDL at address 0x2B0000 */
      0x00004410, 0x002C0000, 0x002C0000, 0x002B0098,
      0x00000000, 0x01080808, 0x02101010, 0x03191919,
      0x04212121, 0x05292929, 0x06313131, 0x073A3A3A,
      0x08424242, 0x094A4A4A, 0x0A525252, 0x0B5A5A5A,
      0x0C636363, 0x0D6B6B6B, 0x0E737373, 0x0F7B7B7B,
      0x10848484, 0x118C8C8C, 0x12949494, 0x139C9C9C,
      0x14A5A5A5, 0x15ADADAD, 0x16B5B5B5, 0x17BDBDBD,
      0x18C5C5C5, 0x19CECECE, 0x1AD6D6D6, 0x1BDEDEDE,
      0x1CE6E6E6, 0x1DEFEFEF, 0x1EF8F8F8, 0x1FFFFFFF,
      0xE0010101, 0xC001002C, 0x002180EF, 0x002C0000,
      0x002C0000, 0x002B00A8, 0x00000000, 0x002C0000,
      0x002C0000, 0x002B0000
    };

  buf          = g_BUF;
  renderer     = g_RENDERER;
  flags        = g_FLAGS;
  pixel_format = g_PIXEL_FORMAT;

  memset(&g_VDLP,0,sizeof(g_VDLP));
  g_PACKED_CLUT_VALID = false;
  g_PACKED_CLUT_DEFERRED = false;
  g_CLUT_GENERATION = 0;
  g_BUF          = buf;
  g_CURBUF       = buf;
  g_RENDERER     = renderer;
  g_FLAGS        = flags;
  g_PIXEL_FORMAT = pixel_format;

  g_VDLP.head_vdl = 0xB0000;
  if(g_RENDERER == NULL)
    g_RENDERER = vdlp_render_line_RGB565;

  for(i = 0; i < (sizeof(StartupVDL)/sizeof(uint32_t)); i++)
    vram_write32((0xB0000 + (i * sizeof(uint32_t))),StartupVDL[i]);
}

void
opera_vdlp_set_vdl_head(const uint32_t addr_)
{
  g_VDLP.head_vdl = addr_;
}

uint32_t
opera_vdlp_state_size_v1(void)
{
  return opera_state_save_size(sizeof(g_VDLP));
}

static
bool
vdlp_state_write_payload(opera_state_writer_t *writer_,
                         vdlp_t const         *state_)
{
  return (opera_state_write_bytes(writer_,state_->clut_r,CLUT_LEN) &&
          opera_state_write_bytes(writer_,state_->clut_g,CLUT_LEN) &&
          opera_state_write_bytes(writer_,state_->clut_b,CLUT_LEN) &&
          opera_state_write_u32(writer_,state_->background_color) &&
          opera_state_write_u32(writer_,state_->head_vdl) &&
          opera_state_write_u32(writer_,state_->curr_vdl) &&
          opera_state_write_u32(writer_,state_->prev_bmp) &&
          opera_state_write_u32(writer_,state_->curr_bmp) &&
          opera_state_write_u32(writer_,state_->bg_color.raw) &&
          opera_state_write_u32(writer_,state_->clut_ctrl.raw) &&
          opera_state_write_u32(writer_,state_->disp_ctrl.raw) &&
          opera_state_write_i32(writer_,state_->line_cnt));
}

static
uint32_t
vdlp_state_payload_size(void)
{
  opera_state_writer_t writer;

  opera_state_writer_init(&writer,NULL,UINT32_MAX);
  vdlp_state_write_payload(&writer,&g_VDLP);

  return opera_state_writer_used(&writer);
}

uint32_t
opera_vdlp_state_size(void)
{
  return opera_state_chunk_size(vdlp_state_payload_size());
}

uint32_t
opera_vdlp_state_save(void *buf_)
{
  uint32_t payload_size;
  opera_state_writer_t writer;

  payload_size = vdlp_state_payload_size();
  opera_state_writer_init(&writer,buf_,opera_state_chunk_size(payload_size));
  opera_state_write_chunk_header(&writer,"VDLP",payload_size);
  vdlp_state_write_payload(&writer,&g_VDLP);

  return opera_state_writer_ok(&writer) ? opera_state_writer_used(&writer) : 0;
}

uint32_t
opera_vdlp_state_load_v1(const void     *buf_,
                         uint32_t const  size_)
{
  uint32_t size;

  size = opera_state_load_sized(&g_VDLP,"VDLP",buf_,size_,sizeof(g_VDLP));
  if(size)
    {
      g_PACKED_CLUT_VALID = false;
      g_PACKED_CLUT_DEFERRED = false;
    }

  return size;
}

static
bool
vdlp_state_read_payload(opera_state_reader_t *reader_,
                        vdlp_t               *state_)
{
  return (opera_state_read_bytes(reader_,state_->clut_r,CLUT_LEN) &&
          opera_state_read_bytes(reader_,state_->clut_g,CLUT_LEN) &&
          opera_state_read_bytes(reader_,state_->clut_b,CLUT_LEN) &&
          opera_state_read_u32(reader_,&state_->background_color) &&
          opera_state_read_u32(reader_,&state_->head_vdl) &&
          opera_state_read_u32(reader_,&state_->curr_vdl) &&
          opera_state_read_u32(reader_,&state_->prev_bmp) &&
          opera_state_read_u32(reader_,&state_->curr_bmp) &&
          opera_state_read_u32(reader_,&state_->bg_color.raw) &&
          opera_state_read_u32(reader_,&state_->clut_ctrl.raw) &&
          opera_state_read_u32(reader_,&state_->disp_ctrl.raw) &&
          opera_state_read_i32(reader_,&state_->line_cnt));
}

uint32_t
opera_vdlp_state_load(const void     *buf_,
                      uint32_t const  size_)
{
  vdlp_t state;
  opera_state_reader_t reader;
  opera_state_reader_t payload;

  opera_state_reader_init(&reader,buf_,size_);
  if(!opera_state_read_chunk(&reader,"VDLP",&payload) ||
     !vdlp_state_read_payload(&payload,&state) ||
     !opera_state_reader_finished(&payload))
    return 0;

  g_VDLP = state;
  g_PACKED_CLUT_VALID = false;
  g_PACKED_CLUT_DEFERRED = false;

  return opera_state_reader_used(&reader);
}

/*
  Is having all these renderers nice? No. But given the performance
  sensitivy of this code and impact it can have on a lower end system
  such verbosity is necessary.
*/

#define _(X,Y) ((((uint64_t)VDLP_PIXEL_FORMAT_##X)<<32)|((Y)&VDLP_FLAGS))
void*
get_renderer(vdlp_pixel_format_e pf_,
             uint32_t            flags_)
{
  uint64_t x;

  x = ((((uint64_t)pf_) << 32) | flags_);

  switch(x)
    {
    case _(RGB565,VDLP_FLAG_NONE):
      return vdlp_render_line_RGB565;
    case _(RGB565,VDLP_FLAG_BYPASS_CLUT):
      return vdlp_render_line_RGB565_bypass_clut;
    case _(RGB565,VDLP_FLAG_HIRES_CEL):
      return vdlp_render_line_RGB565_hires;
    case _(RGB565,VDLP_FLAG_BYPASS_CLUT|VDLP_FLAG_HIRES_CEL):
      return vdlp_render_line_RGB565_hires_bypass_clut;

    case _(XRGB8888,VDLP_FLAG_NONE):
      return vdlp_render_line_XRGB8888;
    case _(XRGB8888,VDLP_FLAG_BYPASS_CLUT):
      return vdlp_render_line_XRGB8888_bypass_clut;
    case _(XRGB8888,VDLP_FLAG_HIRES_CEL):
      return vdlp_render_line_XRGB8888_hires;
    case _(XRGB8888,VDLP_FLAG_BYPASS_CLUT|VDLP_FLAG_HIRES_CEL):
      return vdlp_render_line_XRGB8888_hires_bypass_clut;
    }

  return NULL;
}
#undef _

int
opera_vdlp_set_video_buffer(void *buf_)
{
  g_BUF = buf_;

  return 0;
}

int
opera_vdlp_set_hires(bool const v_)
{
  uint32_t flags;
  vdlp_renderer_t new_renderer;

  flags = g_FLAGS;
  set_or_clr_flag(flags,VDLP_FLAG_HIRES_CEL,v_);

  new_renderer = get_renderer(g_PIXEL_FORMAT,flags);
  if(new_renderer == NULL)
    return -1;

  g_RENDERER = new_renderer;
  g_FLAGS    = flags;

  return 0;
}

int
opera_vdlp_set_bypass_clut(bool const v_)
{
  uint32_t flags;
  vdlp_renderer_t new_renderer;

  flags = g_FLAGS;
  set_or_clr_flag(flags,VDLP_FLAG_BYPASS_CLUT,v_);

  new_renderer = get_renderer(g_PIXEL_FORMAT,flags);
  if(new_renderer == NULL)
    return -1;

  g_RENDERER = new_renderer;
  g_FLAGS    = flags;

  return 0;
}

int
opera_vdlp_set_pixel_format(vdlp_pixel_format_e v_)
{
  vdlp_renderer_t new_renderer;

  new_renderer = get_renderer(v_,g_FLAGS);
  if(new_renderer == NULL)
    return -1;

  g_RENDERER     = new_renderer;
  g_PIXEL_FORMAT = v_;

  return 0;
}
