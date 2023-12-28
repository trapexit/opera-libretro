#ifndef LIBOPERA_VDLP_H_INCLUDED
#define LIBOPERA_VDLP_H_INCLUDED

#include "extern_c.h"

#include "boolean.h"

#include <stdint.h>

#define VDLP_FLAG_NONE          0
#define VDLP_FLAG_BYPASS_CLUT   (1<<0)
#define VDLP_FLAG_HIRES_CEL     (1<<1)
#define VDLP_FLAG_INTERPOLATION (1<<2)
#define VDLP_FLAGS              (VDLP_FLAG_BYPASS_CLUT|VDLP_FLAG_HIRES_CEL|VDLP_FLAG_INTERPOLATION)

enum vdlp_pixel_format_e
  {
    VDLP_PIXEL_FORMAT_0RGB1555,
    VDLP_PIXEL_FORMAT_XRGB8888,
    VDLP_PIXEL_FORMAT_RGB565
  };

typedef enum vdlp_pixel_format_e vdlp_pixel_format_e;

EXTERN_C_BEGIN

void opera_vdlp_init();

void opera_vdlp_set_vdl_head(const uint32_t addr);
void opera_vdlp_process_line(int line);

uint32_t opera_vdlp_state_size(void);
uint32_t opera_vdlp_state_save(void *buf);
uint32_t opera_vdlp_state_load(void const *buf);

int opera_vdlp_set_video_buffer(void* buf);
int opera_vdlp_set_hires(bool v);
int opera_vdlp_set_bypass_clut(bool v);
int opera_vdlp_set_pixel_format(vdlp_pixel_format_e fmt);

EXTERN_C_END

#endif /* LIBOPERA_VDLP_H_INCLUDED */
