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

/* VDLP.h: interface for the CVDLP class. */

#ifndef LIBFREEDO_VDLP_H_INCLUDED
#define LIBFREEDO_VDLP_H_INCLUDED

#include "extern_c.h"

#include <stdint.h>

#define VDLP_FLAG_NONE          0
#define VDLP_FLAG_CLUT_BYPASS   (1<<0)
#define VDLP_FLAG_HIRES_CEL     (1<<1)
#define VDLP_FLAG_INTERPOLATION (1<<2)
#define VDLP_FLAGS              (VDLP_FLAG_CLUT_BYPASS|VDLP_FLAG_HIRES_CEL|VDLP_FLAG_INTERPOLATION)

typedef struct vdlp_line_s vdlp_line_t;
struct vdlp_line_s
{
  uint16_t line[640];
  uint8_t  clut_r[32];
  uint8_t  clut_g[32];
  uint8_t  clut_b[32];
  uint32_t clut_ctrl;
  uint32_t disp_ctrl;
  uint32_t background;
};

typedef struct vdlp_frame_s vdlp_frame_t;
struct vdlp_frame_s
{
  vdlp_line_t  lines[480];
  unsigned int src_w;
  unsigned int src_h;
};

typedef enum vdlp_pixel_format_e vdlp_pixel_format_e;
enum vdlp_pixel_format_e
  {
    VDLP_PIXEL_FORMAT_0RGB1555,
    VDLP_PIXEL_FORMAT_XRGB8888,
    VDLP_PIXEL_FORMAT_RGB565
  };

typedef enum vdlp_pixel_res_e vdlp_pixel_res_e;
enum vdlp_pixel_res_e
  {
    VDLP_PIXEL_RES_320x240,
    VDLP_PIXEL_RES_384x288,
    VDLP_PIXEL_RES_640x480,
    VDLP_PIXEL_RES_768x576
  };

EXTERN_C_BEGIN

void     freedo_vdlp_init(uint8_t *vram_);

void     freedo_vdlp_process(const uint32_t addr_);
void     freedo_vdlp_process_line(int line_);

uint32_t freedo_vdlp_state_size(void);
void     freedo_vdlp_state_save(void *buf_);
void     freedo_vdlp_state_load(const void *buf_);

int      freedo_vdlp_configure(void *buf,
                               vdlp_pixel_format_e pf,
                               vdlp_pixel_res_e res,
                               uint32_t flags_);

EXTERN_C_END

#endif /* LIBFREEDO_VDLP_H_INCLUDED */
