/*
  www.freedo.org
  The first and only working 3DO multiplayer emulator.

  The FreeDO licensed under modified GNU LGPL, with following notes:

  *   The owners and original authors of the FreeDO have full right to
  *   develop closed source derivative work.

  *   Any non-commercial uses of the FreeDO sources or any knowledge
  *   obtained by studying or reverse engineering of the sources, or
  *   any other material published by FreeDO have to be accompanied
  *   with full credits.

  *   Any commercial uses of FreeDO sources or any knowledge obtained
  *   by studying or reverse engineering of the sources, or any other
  *   material published by FreeDO is strictly forbidden without
  *   owners approval.

  The above notes are taking precedence over GNU LGPL in conflicting
  situations.

  Project authors:
  *  Alexander Troosh
  *  Maxim Grishin
  *  Allen Wright
  *  John Sammons
  *  Felix Lazarev
*/

#include "freedo_arm.h"
#include "freedo_core.h"
#include "freedo_vdl.h"
#include "freedo_vdlp.h"
#include "freedo_vdlp_i.h"
#include "hack_flags.h"
#include "inline.h"
#include "static_assert.h"

#include <boolean.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define VRAM_OFFSET (1024 * 1024 * 2)

/* VDLP options */
/*
  - Output type (16bit, 24bit, 32bit) (move freedo_frame into vdlp)
  - interpolation
  - native vs 2x mode?
  - Disable CLUT?
  - Add pseudo random 3bit pattern for second clut bypass mode
  - table lookup vs shift left 3?
 */

static vdlp_t    g_VDLP        = {0};
static uint8_t  *g_VRAM        = NULL;
static void     *g_BUF         = NULL;
static void     *g_CURBUF      = NULL;
static uint32_t  g_BUF_WIDTH   = 320;
static uint32_t  g_BUF_HEIGHT  = 240;
static void (*g_RENDERER)(int) = NULL;

static const uint32_t PIXELS_PER_LINE_MODULO[8] =
  {320, 384, 512, 640, 1024, 320, 320, 320};

static
INLINE
uint32_t
vram_read32(const uint32_t addr_)
{
  return freedo_mem_read32(VRAM_OFFSET + (addr_ & 0x000FFFFF));
}

static
INLINE
void
vram_write32(const uint32_t addr_,
             const uint32_t val_)
{
  freedo_mem_write32((VRAM_OFFSET + (addr_ & 0x000FFFFF)),val_);
}

static
INLINE
uint32_t
vdl_read(const uint32_t off_)
{
  return vram_read32(g_VDLP.curr_vdl + (off_ << 2));
}

static
INLINE
void
vdl_set_clut(const vdl_ctrl_word_u cmd_)
{
  switch(cmd_.cvw.rgb_enable)
    {
    case 0b00:
      g_VDLP.clut_r[cmd_.cvw.addr] = cmd_.cvw.r;
      g_VDLP.clut_b[cmd_.cvw.addr] = cmd_.cvw.b;
      g_VDLP.clut_g[cmd_.cvw.addr] = cmd_.cvw.g;
      break;
    case 0b11:
      g_VDLP.clut_r[cmd_.cvw.addr] = cmd_.cvw.r;
      break;
    case 0b01:
      g_VDLP.clut_b[cmd_.cvw.addr] = cmd_.cvw.b;
      break;
    case 0b10:
      g_VDLP.clut_g[cmd_.cvw.addr] = cmd_.cvw.g;
      break;
    }
}

static
INLINE
void
vdlp_clut_reset(void)
{
  /* memcpy(CLUTR,FIXED_CLUT,sizeof(FIXED_CLUT)); */
  /* memcpy(CLUTG,FIXED_CLUT,sizeof(FIXED_CLUT)); */
  /* memcpy(CLUTB,FIXED_CLUT,sizeof(FIXED_CLUT)); */
}

static
INLINE
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
      switch((cmd.raw & 0xE0000000) >> 28)
        {
        case 0b000:
        case 0b001:
        case 0b010:
        case 0b011:
          vdl_set_clut(cmd);
          break;
        case 0b100:
        case 0b101:
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
        case 0b110:
          if(colors_only)
            continue;
          g_VDLP.disp_ctrl.raw = cmd.raw;
          colors_only = cmd.dcw.colors_only;
          break;
        case 0b111:
          g_VDLP.bg_color.raw = cmd.raw;
          break;
        }
    }
}

static
INLINE
void
vdlp_process_vdl_entry(const uint32_t entry_)
{
  int i;
  uint32_t next_entry;
  clut_dma_ctrl_word_s *cdcw = &g_VDLP.clut_ctrl.cdcw;

  g_VDLP.clut_ctrl.raw = entry_;

  if(cdcw->curr_fba_override)
    g_VDLP.curr_bmp = vdl_read(1);

  if(cdcw->prev_fba_override)
    g_VDLP.prev_bmp = vdl_read(2);

  next_entry = vdl_read(3);
  if(cdcw->next_vdl_addr_rel)
    next_entry += (g_VDLP.curr_vdl + (4 * sizeof(uint32_t)));

  g_VDLP.curr_vdl += (4 * sizeof(uint32_t));

  vdlp_process_optional_cmds(cdcw->ctrl_word_cnt);

  g_VDLP.curr_vdl = next_entry;
  g_VDLP.line_cnt = cdcw->persist_len;
}

static
INLINE
void
vdlp_execute(void)
{
  uint32_t tmp;

  tmp = vram_read32(g_VDLP.curr_vdl);
  if(tmp)
    vdlp_process_vdl_entry(tmp);
}

static
INLINE
void
vdlp_render_line_0RGB1555(void)
{
  int x;
  int width;
  int bypass_clut;
  uint16_t *src;
  uint16_t *dst;

  if(!g_VDLP.clut_ctrl.cdcw.enable_dma)
    return;

  bypass_clut = g_VDLP.disp_ctrl.dcw.clut_bypass;
  width = PIXELS_PER_LINE_MODULO[g_VDLP.clut_ctrl.cdcw.fba_incr_modulo];

  dst = g_CURBUF;
  src = (uint16_t*)(g_VRAM + ((g_VDLP.curr_bmp^2) & 0x0FFFFF));

  for(x = 0; x < width; x++)
    {
      if(*src == 0)
        {
          *dst = (((g_VDLP.bg_color.bvw.r >> 3) << 0xA) |
                  ((g_VDLP.bg_color.bvw.g >> 3) << 0x5) |
                  ((g_VDLP.bg_color.bvw.b >> 3) << 0x0));
        }
      else if(bypass_clut && (*src & 0x8000))
        {
          *dst = (*src & 0x7FFF);
        }
      else
        {
          *dst = (((g_VDLP.clut_r[(*src >> 0xA) & 0x1F] >> 3) << 0xA) |
                  ((g_VDLP.clut_g[(*src >> 0x5) & 0x1F] >> 3) << 0x5) |
                  ((g_VDLP.clut_b[(*src >> 0x0) & 0x1F] >> 3) << 0x0));
        }

      dst += 1;
      src += 2;
    }

  g_CURBUF = dst;
}

static
INLINE
void
vdlp_render_line_RGB565(void)
{
  int x;
  int width;
  int bypass_clut;
  uint16_t *src;
  uint16_t *dst;

  if(!g_VDLP.clut_ctrl.cdcw.enable_dma)
    return;

  bypass_clut = g_VDLP.disp_ctrl.dcw.clut_bypass;
  width = PIXELS_PER_LINE_MODULO[g_VDLP.clut_ctrl.cdcw.fba_incr_modulo];

  dst = g_CURBUF;
  src = (uint16_t*)(g_VRAM + ((g_VDLP.curr_bmp^2) & 0x0FFFFF));

  for(x = 0; x < width; x++)
    {
      if(*src == 0)
        {
          *dst = (((g_VDLP.bg_color.bvw.r >> 3) << 0xB) |
                  ((g_VDLP.bg_color.bvw.g >> 2) << 0x5) |
                  ((g_VDLP.bg_color.bvw.b >> 3) << 0x0));
        }
      else if(bypass_clut && (*src & 0x8000))
        {
          *dst = (*src & 0x7FFF);
        }
      else
        {
          *dst = (((g_VDLP.clut_r[(*src >> 0xA) & 0x1F] >> 3) << 0xB) |
                  ((g_VDLP.clut_g[(*src >> 0x5) & 0x1F] >> 2) << 0x5) |
                  ((g_VDLP.clut_b[(*src >> 0x0) & 0x1F] >> 3) << 0x0));
        }

      dst += 1;
      src += 2;
    }

  g_CURBUF = dst;
}

static
INLINE
void
vdlp_render_line_XRGB8888(void)
{
  int x;
  int width;
  int bypass_clut;
  uint16_t *src;
  uint32_t *dst;

  if(!g_VDLP.clut_ctrl.cdcw.enable_dma)
    return;

  bypass_clut = g_VDLP.disp_ctrl.dcw.clut_bypass;
  width = PIXELS_PER_LINE_MODULO[g_VDLP.clut_ctrl.cdcw.fba_incr_modulo];

  dst = g_CURBUF;
  src = (uint16_t*)(g_VRAM + ((g_VDLP.curr_bmp^2) & 0x0FFFFF));

  for(x = 0; x < width; x++)
    {
      if(*src == 0)
        {
          *dst = g_VDLP.bg_color.raw;
        }
      else if(bypass_clut && (*src & 0x8000))
        {
          *dst = (((*src & 0x7C00) << 0x9) |
                  ((*src & 0x03E0) << 0x6) |
                  ((*src & 0x001F) << 0x3));
        }
      else
        {
          *dst = ((g_VDLP.clut_r[(*src >> 0xA) & 0x1F] << 0x10) |
                  (g_VDLP.clut_g[(*src >> 0x5) & 0x1F] << 0x08) |
                  (g_VDLP.clut_b[(*src >> 0x0) & 0x1F] << 0x00));
        }

      dst += 1;
      src += 2;
    }

  g_CURBUF = dst;
}

static
INLINE
void
vdlp_process_line_640(int           line_,
                      vdlp_frame_t *frame_)
{
  vdlp_line_t *line0;
  vdlp_line_t *line1;

  line0 = &frame_->lines[(line_ << 1) + 0];
  line1 = &frame_->lines[(line_ << 1) + 1];
  if(g_VDLP.clut_ctrl.cdcw.enable_dma)
    {
      int i;
      uint16_t *dst1;
      uint16_t *dst2;
      uint32_t *src1;
      uint32_t *src2;
      uint32_t *src3;
      uint32_t *src4;

      dst1 = line0->line;
      dst2 = line1->line;
      src1 = (uint32_t*)(g_VRAM + ((g_VDLP.prev_bmp^2) & 0x0FFFFF) + (0*1024*1024));
      src2 = (uint32_t*)(g_VRAM + ((g_VDLP.prev_bmp^2) & 0x0FFFFF) + (1*1024*1024));
      src3 = (uint32_t*)(g_VRAM + ((g_VDLP.prev_bmp^2) & 0x0FFFFF) + (2*1024*1024));
      src4 = (uint32_t*)(g_VRAM + ((g_VDLP.prev_bmp^2) & 0x0FFFFF) + (3*1024*1024));

      for(i = 0; i < 320; i++)
        {
          *dst1++ = *(uint16_t*)(src1++);
          *dst1++ = *(uint16_t*)(src2++);
          *dst2++ = *(uint16_t*)(src3++);
          *dst2++ = *(uint16_t*)(src4++);
        }

      memcpy(line0->clut_r,g_VDLP.clut_r,32);
      memcpy(line0->clut_g,g_VDLP.clut_g,32);
      memcpy(line0->clut_b,g_VDLP.clut_b,32);
      memcpy(line1->clut_r,g_VDLP.clut_r,32);
      memcpy(line1->clut_g,g_VDLP.clut_g,32);
      memcpy(line1->clut_b,g_VDLP.clut_b,32);
    }

  line0->disp_ctrl  = line1->disp_ctrl  = g_VDLP.disp_ctrl.raw;
  line0->clut_ctrl  = line1->clut_ctrl  = g_VDLP.clut_ctrl.raw;
  line0->background = line1->background = g_VDLP.bg_color.raw;
}

static
INLINE
void
vdlp_render_line(void)
{
  /* if(HIRESMODE) */
  /*   vdlp_process_line_640(line_,frame_); */
  /* else */
  /*   vdlp_process_line_320(line_,frame_); */

  vdlp_render_line_XRGB8888();
  //vdlp_render_line_320_0RGB1555(line_);
  //vdlp_render_line_320_RGB565(line_);
}

/* tick / increment frame buffer address */
static
INLINE
uint32_t
tick_fba(const uint32_t fba_)
{
  uint32_t modulo;

  modulo = PIXELS_PER_LINE_MODULO[g_VDLP.clut_ctrl.cdcw.fba_incr_modulo];

  return (fba_ + ((fba_ & 2) ? ((modulo << 2) - 2) : 2));
}

void
freedo_vdlp_process_line(int line_)
{
  int y;

  line_ &= 0x07FF;
  if(line_ == 0)
    {
      g_CURBUF = g_BUF;
      g_VDLP.line_cnt = 0;
      g_VDLP.curr_vdl = g_VDLP.head_vdl;
      vdlp_execute();
    }

  if(g_VDLP.line_cnt == 0)
    vdlp_execute();

  y = (line_ - 16);
  if((y >= 0) && (y < 240))
    vdlp_render_line();

  /*
    See ppgfldr/ggsfldr/gpgfldr/2gpgb.html for details on the frame
    buffer layout.

    320x240 by 16bit stored in "left/right" pairs. High order 16bits
    represent X,Y and low order bits represent X,Y+1. Starting from
    0,0 and going left to right and top to bottom to 319,239.
  */
  g_VDLP.prev_bmp = ((g_VDLP.clut_ctrl.cdcw.prev_fba_tick) ?
                     tick_fba(g_VDLP.prev_bmp) : g_VDLP.curr_bmp);
  g_VDLP.curr_bmp = tick_fba(g_VDLP.curr_bmp);

  g_VDLP.disp_ctrl.dcw.vi_off_1_line = 0;
  g_VDLP.line_cnt--;
}


void
freedo_vdlp_init(uint8_t *vram_)
{
  uint32_t i;
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

  g_VRAM = vram_;
  g_VDLP.head_vdl = 0xB0000;

  for(i = 0; i < (sizeof(StartupVDL)/sizeof(uint32_t)); i++)
    vram_write32((0xB0000 + (i * sizeof(uint32_t))),StartupVDL[i]);

  vdlp_clut_reset();
}

void
freedo_vdlp_process(const uint32_t addr_)
{
  g_VDLP.head_vdl = addr_;
}

uint32_t
freedo_vdlp_state_size(void)
{
  return sizeof(vdlp_t);
}

void
freedo_vdlp_state_save(void *buf_)
{
  //memcpy(buf_,&vdl,sizeof(vdlp_datum_t));
}

void
freedo_vdlp_state_load(const void *buf_)
{
  //memcpy(&vdl,buf_,sizeof(vdlp_datum_t));
}

int
freedo_vdlp_configure(void                *buf_,
                      vdlp_pixel_format_e  pf_,
                      vdlp_pixel_res_e     res_,
                      uint32_t             flags_)
{
  g_BUF = buf_;

  switch(res_)
    {
    case VDLP_PIXEL_RES_320x240:
      g_BUF_WIDTH  = 320;
      g_BUF_HEIGHT = 240;
      break;
    case VDLP_PIXEL_RES_384x288:
      g_BUF_WIDTH  = 384;
      g_BUF_HEIGHT = 288;
      break;
    case VDLP_PIXEL_RES_640x480:
      g_BUF_WIDTH  = 640;
      g_BUF_HEIGHT = 480;
      break;
    case VDLP_PIXEL_RES_768x576:
      g_BUF_WIDTH  = 768;
      g_BUF_HEIGHT = 576;
      break;

    }

  switch(pf_)
    {
    case VDLP_PIXEL_FORMAT_0RGB1555:
      break;
    case VDLP_PIXEL_FORMAT_RGB565:
      break;
    case VDLP_PIXEL_FORMAT_XRGB8888:
      break;
    }

  return 0;
}
