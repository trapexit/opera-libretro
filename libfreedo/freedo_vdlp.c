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
#include <string.h>

#define VRAM_OFFSET (1024 * 1024 * 2)

/* VDLP options */
/*
  - Output type (16bit, 24bit, 32bit) (move freedo_frame into vdlp)
  - interpolation
  - native vs 2x mode?
  - Disable CLUT?
 */

struct cdmaw
{
  uint32_t lines:9;             //0-8
  uint32_t numword:6;           //9-14
  uint32_t prevover:1;          //15
  uint32_t currover:1;          //16
  uint32_t prevtick:1;          //17
  uint32_t abs:1;               //18
  uint32_t vmode:1;             //19
  uint32_t pad0:1;              //20
  uint32_t enadma:1;            //21
  uint32_t pad1:1;              //22
  uint32_t modulo:3;            //23-25
  uint32_t pad2:6;              //26-31
};

union CDMW
{
  uint32_t     raw;
  struct cdmaw dmaw;
};

struct vdlp_datum_s
{
  uint8_t    CLUTB[32];
  uint8_t    CLUTG[32];
  uint8_t    CLUTR[32];
  uint32_t   BACKGROUND;
  uint32_t   HEADVDL;
  uint32_t   MODULO;
  uint32_t   CURRENTVDL;
  uint32_t   CURRENTBMP;
  uint32_t   PREVIOUSBMP;
  uint32_t   OUTCONTROLL;
  union CDMW CLUTDMA;
  int        line_delay;
};

typedef struct vdlp_datum_s vdlp_datum_t;

#define CLUTB       vdl.CLUTB
#define CLUTG       vdl.CLUTG
#define CLUTR       vdl.CLUTR
#define BACKGROUND  vdl.BACKGROUND
#define HEADVDL     vdl.HEADVDL
#define MODULO      vdl.MODULO
#define CURRENTVDL  vdl.CURRENTVDL
#define CURRENTBMP  vdl.CURRENTBMP
#define PREVIOUSBMP vdl.PREVIOUSBMP
#define OUTCONTROLL vdl.OUTCONTROLL
#define CLUTDMA     vdl.CLUTDMA
#define LINE_DELAY  vdl.line_delay

static vdlp_t g_VDLP = {0};

static vdlp_datum_t vdl;
static uint8_t *VRAM = NULL;
static const uint32_t PIXELS_PER_LINE_MODULO[8] =
  {320, 384, 512, 640, 1024, 320, 320, 320};

/*
  for(int i = 0; i < 32; i++)
    FIXED_CLUT[i] = (((i & 0x1F) << 3) | ((i >> 2) & 7));
*/
static const uint8_t FIXED_CLUT[32] =
  {
    0x00, 0x08, 0x10, 0x18,
    0x21, 0x29, 0x31, 0x39,
    0x42, 0x4A, 0x52, 0x5A,
    0x63, 0x6B, 0x73, 0x7B,
    0x84, 0x8C, 0x94, 0x9C,
    0xA5, 0xAD, 0xB5, 0xBD,
    0xC6, 0xCE, 0xD6, 0xDE,
    0xE7, 0xEF, 0xF7, 0xFF
  };


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
      g_VDLP.clut_r[cmd_.cvw.addr] = cmd_.cvw.red;
      g_VDLP.clut_b[cmd_.cvw.addr] = cmd_.cvw.blue;
      g_VDLP.clut_g[cmd_.cvw.addr] = cmd_.cvw.green;
      break;
    case 0b11:
      g_VDLP.clut_r[cmd_.cvw.addr] = cmd_.cvw.red;
      break;
    case 0b01:
      g_VDLP.clut_b[cmd_.cvw.addr] = cmd_.cvw.blue;
      break;
    case 0b10:
      g_VDLP.clut_g[cmd_.cvw.addr] = cmd_.cvw.green;
      break;
    }
}

static
INLINE
void
vdlp_clut_reset(void)
{
  memcpy(CLUTR,FIXED_CLUT,sizeof(FIXED_CLUT));
  memcpy(CLUTG,FIXED_CLUT,sizeof(FIXED_CLUT));
  memcpy(CLUTB,FIXED_CLUT,sizeof(FIXED_CLUT));
}

static
INLINE
void
vdlp_process_optional_cmds(const int ctrl_word_cnt_)
{
  int i;
  int ignore;
  vdl_ctrl_word_u cmd;

  ignore = 1;
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
          if(ignore)
            continue;
          g_VDLP.disp_ctrl.raw = cmd.raw;
          ignore = cmd.dcw.colors_only;
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
}



static
INLINE
void
vdlp_execute_next_vdl(const uint32_t vdl_)
{
  int i;
  int numcmd;
  uint32_t NEXTVDL;
  uint32_t ignore_flag;

  ignore_flag = 0;
  CLUTDMA.raw = vdl_;

  if(CLUTDMA.dmaw.currover)
    CURRENTBMP = ((FIXMODE & FIX_BIT_TIMING_5) ?
                  vram_read32(CURRENTVDL+8) :
                  vram_read32(CURRENTVDL+4));

  if(CLUTDMA.dmaw.prevover)
    PREVIOUSBMP = ((FIXMODE & FIX_BIT_TIMING_5) ?
                   vram_read32(CURRENTVDL+4) :
                   vram_read32(CURRENTVDL+8));

  NEXTVDL = ((CLUTDMA.dmaw.abs) ?
             (CURRENTVDL+vram_read32(CURRENTVDL+12)+16) :
             vram_read32(CURRENTVDL+12));

  CURRENTVDL += 16;

  numcmd = CLUTDMA.dmaw.numword;
  for(i = 0; i < numcmd; i++)
    {
      uint32_t cmd;

      cmd = vram_read32(CURRENTVDL);
      CURRENTVDL += 4;

      if(!(cmd & VDL_CONTROL))
        {
          const uint32_t idx = ((cmd & VDL_PEN_MASK) >> VDL_PEN_SHIFT);

          switch(cmd & VDL_RGBCTL_MASK)
            {
            case VDL_FULLRGB:
              CLUTR[idx] = ((cmd & VDL_R_MASK) >> VDL_R_SHIFT);
              CLUTG[idx] = ((cmd & VDL_G_MASK) >> VDL_G_SHIFT);
              CLUTB[idx] = ((cmd & VDL_B_MASK) >> VDL_B_SHIFT);
              break;
            case VDL_REDONLY:
              CLUTR[idx] = ((cmd & VDL_R_MASK) >> VDL_R_SHIFT);
              break;
            case VDL_GREENONLY:
              CLUTG[idx] = ((cmd & VDL_G_MASK) >> VDL_G_SHIFT);
              break;
            case VDL_BLUEONLY:
              CLUTB[idx] = ((cmd & VDL_B_MASK) >> VDL_B_SHIFT);
              break;
            }
        }
      else if(!ignore_flag)
        {
          if((cmd & 0xFF000000) == VDL_BACKGROUND)
            {
              BACKGROUND = (((cmd & 0x000000FF) << 16) |
                            ((cmd & 0x0000FF00) <<  0) |
                            ((cmd & 0x00FF0000) >> 16));
            }
          else if((cmd & 0xE0000000) == 0xC0000000)
            {
              OUTCONTROLL = cmd;
              ignore_flag = (OUTCONTROLL & 2);
            }
          else if(cmd == 0xFFFFFFFF)
            {
              vdlp_clut_reset();
            }
        }
    }

  CURRENTVDL = NEXTVDL;
  MODULO     = PIXELS_PER_LINE_MODULO[CLUTDMA.dmaw.modulo];
  LINE_DELAY = CLUTDMA.dmaw.lines;
}

static
INLINE
void
vdlp_execute(void)
{
  uint32_t tmp;

  tmp = vram_read32(CURRENTVDL);
  if(tmp)
    vdlp_execute_next_vdl(tmp);
}

static
INLINE
void
vdlp_process_line_320(int           line_,
                      vdlp_frame_t *frame_)
{
  vdlp_line_t *line;

  line = &frame_->lines[line_];
  if(CLUTDMA.dmaw.enadma)
    {
      int i;
      uint16_t *dst;
      uint32_t *src;

      dst = line->line;
      src = (uint32_t*)(VRAM + ((CURRENTBMP^2) & 0x0FFFFF));

      for(i = 0; i < 320; i++)
        *dst++ = *(uint16_t*)(src++);

      memcpy(line->xCLUTR,CLUTR,32);
      memcpy(line->xCLUTG,CLUTG,32);
      memcpy(line->xCLUTB,CLUTB,32);
    }

  line->xOUTCONTROLL = OUTCONTROLL;
  line->xCLUTDMA     = CLUTDMA.raw;
  line->xBACKGROUND  = BACKGROUND;
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
  if(CLUTDMA.dmaw.enadma)
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
      src1 = (uint32_t*)(VRAM + ((PREVIOUSBMP^2) & 0x0FFFFF) + (0*1024*1024));
      src2 = (uint32_t*)(VRAM + ((PREVIOUSBMP^2) & 0x0FFFFF) + (1*1024*1024));
      src3 = (uint32_t*)(VRAM + ((PREVIOUSBMP^2) & 0x0FFFFF) + (2*1024*1024));
      src4 = (uint32_t*)(VRAM + ((PREVIOUSBMP^2) & 0x0FFFFF) + (3*1024*1024));

      for(i = 0; i < 320; i++)
        {
          *dst1++ = *(uint16_t*)(src1++);
          *dst1++ = *(uint16_t*)(src2++);
          *dst2++ = *(uint16_t*)(src3++);
          *dst2++ = *(uint16_t*)(src4++);
        }

      memcpy(line0->xCLUTR,CLUTR,32);
      memcpy(line0->xCLUTG,CLUTG,32);
      memcpy(line0->xCLUTB,CLUTB,32);
      memcpy(line1->xCLUTR,CLUTR,32);
      memcpy(line1->xCLUTG,CLUTG,32);
      memcpy(line1->xCLUTB,CLUTB,32);
    }

  line0->xOUTCONTROLL = line1->xOUTCONTROLL = OUTCONTROLL;
  line0->xCLUTDMA     = line1->xCLUTDMA     = CLUTDMA.raw;
  line0->xBACKGROUND  = line1->xBACKGROUND  = BACKGROUND;
}

static
INLINE
void
vdlp_process_line(int           line_,
                  vdlp_frame_t *frame_)
{
  if(HIRESMODE)
    vdlp_process_line_640(line_,frame_);
  else
    vdlp_process_line_320(line_,frame_);
}

/* tick / increment bitmap-buffer address */
static
INLINE
uint32_t
tick_fba(const uint32_t fba_)
{
  return (fba_ + ((fba_ & 2) ? ((MODULO << 2) - 2) : 2));

}

void
freedo_vdlp_process_line(int           line_,
                         vdlp_frame_t *frame_)
{
  int y;

  line_ &= 0x07FF;
  if(line_ == 0)
    {
      LINE_DELAY = 0;
      CURRENTVDL = HEADVDL;
      vdlp_execute();
    }

  if(LINE_DELAY == 0)
    vdlp_execute();

  y = (line_ - 16);
  if((y >= 0) && (y < 240))
    vdlp_process_line(y,frame_);

  /*
    See ppgfldr/ggsfldr/gpgfldr/2gpgb.html for details on the frame
    buffer layout.

    320x240 by 16bit stored in "left/right" pairs. High order 16bits
    represent X,Y and low order bits represent X,Y+1. Starting from
    0,0 and going left to right and top to bottom to 319,239.
  */

  PREVIOUSBMP = ((CLUTDMA.dmaw.prevtick) ? tick_fba(PREVIOUSBMP) : CURRENTBMP);
  CURRENTBMP  = tick_fba(CURRENTBMP);

  LINE_DELAY--;
  OUTCONTROLL &= ~1; /* Vioff1ln: auto turn off vertical interpolation */
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

  VRAM    = vram_;
  HEADVDL = 0xB0000;

  for(i = 0; i < (sizeof(StartupVDL)/sizeof(uint32_t)); i++)
    vram_write32((HEADVDL + (i * sizeof(uint32_t))),StartupVDL[i]);

  vdlp_clut_reset();
}

void
freedo_vdlp_process(const uint32_t addr_)
{
  HEADVDL = addr_;
}

uint32_t
freedo_vdlp_state_size(void)
{
  return sizeof(vdlp_datum_t);
}

void
freedo_vdlp_state_save(void *buf_)
{
  memcpy(buf_,&vdl,sizeof(vdlp_datum_t));
}

void
freedo_vdlp_state_load(const void *buf_)
{
  memcpy(&vdl,buf_,sizeof(vdlp_datum_t));
}
