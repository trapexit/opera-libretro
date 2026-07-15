#include "lr_input.h"
#include "lr_input_crosshair.h"

#include <stdint.h>

struct lr_crosshair_s
{
  int32_t  x;
  int32_t  y;
  uint32_t c;
};

typedef struct lr_crosshair_s lr_crosshair_t;

static const uint32_t COLORS[LR_INPUT_MAX_DEVICES] =
  {
    0x000000FF,
    0x00FF0000,
    0x0000FF00,
    0x00FFFFFF,
    0x00FF00FF,
    0x00FFFF00,
    0x0000FFFF,
    0x00000001
  };

static lr_crosshair_t CROSSHAIRS[LR_INPUT_MAX_DEVICES] = {{0,0,0}};

static
uint16_t
lr_input_crosshair_color_RGB565(const uint32_t color_)
{
  uint16_t r;
  uint16_t g;
  uint16_t b;

  r = ((color_ >> 16) & 0xFF);
  g = ((color_ >>  8) & 0xFF);
  b = ((color_ >>  0) & 0xFF);

  return (((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

static
void
lr_input_crosshair_draw_pixel(void                      *buf_,
                              const size_t               pitch_,
                              const int32_t              x_,
                              const int32_t              y_,
                              const uint32_t             color_,
                              const vdlp_pixel_format_e  pixel_format_)
{
  uint8_t *row;

  row = ((uint8_t*)buf_ + ((size_t)y_ * pitch_));
  if(pixel_format_ == VDLP_PIXEL_FORMAT_XRGB8888)
    ((uint32_t*)row)[x_] = color_;
  else
    ((uint16_t*)row)[x_] = lr_input_crosshair_color_RGB565(color_);
}

static
void
lr_input_crosshair_draw(const lr_crosshair_t      *crosshair_,
                        void                      *buf_,
                        const int32_t              width_,
                        const int32_t              height_,
                        const size_t               pitch_,
                        const vdlp_pixel_format_e  pixel_format_)
{
  int32_t x;
  int32_t y;

  if((width_ <= 0) || (height_ <= 0))
    return;

  x = (((crosshair_->x + 32768) * width_) / 65536);
  y = (((crosshair_->y + 32768) * height_) / 65536);

  if(x < 0)
    x = 0;
  else if(x >= width_)
    x = (width_ - 1);

  if(y < 0)
    y = 0;
  else if(y >= height_)
    y = (height_ - 1);

  lr_input_crosshair_draw_pixel(buf_,pitch_,x,y,crosshair_->c,pixel_format_);
  if(x >= 1)
    lr_input_crosshair_draw_pixel(buf_,pitch_,x - 1,y,crosshair_->c,pixel_format_);
  if(x < (width_ - 1))
    lr_input_crosshair_draw_pixel(buf_,pitch_,x + 1,y,crosshair_->c,pixel_format_);
  if(y >= 1)
    lr_input_crosshair_draw_pixel(buf_,pitch_,x,y - 1,crosshair_->c,pixel_format_);
  if(y < (height_ - 1))
    lr_input_crosshair_draw_pixel(buf_,pitch_,x,y + 1,crosshair_->c,pixel_format_);
}

void
lr_input_crosshair_set(const uint32_t i_,
                       const int32_t  x_,
                       const int32_t  y_)
{
  if(i_ >= LR_INPUT_MAX_DEVICES)
    return;

  CROSSHAIRS[i_].x = x_;
  CROSSHAIRS[i_].y = y_;
  CROSSHAIRS[i_].c = COLORS[i_];
}

void
lr_input_crosshair_reset(const uint32_t i_)
{
  if(i_ >= LR_INPUT_MAX_DEVICES)
    return;

  CROSSHAIRS[i_].x = 0;
  CROSSHAIRS[i_].y = 0;
  CROSSHAIRS[i_].c = 0;
}

void
lr_input_crosshairs_draw(void                      *buf_,
                         const uint32_t             width_,
                         const uint32_t             height_,
                         const size_t               pitch_,
                         const vdlp_pixel_format_e  pixel_format_)
{
  int i;

  for(i = 0; i < LR_INPUT_MAX_DEVICES; i++)
    {
      if(CROSSHAIRS[i].c == 0)
        continue;
      lr_input_crosshair_draw(&CROSSHAIRS[i],
                              buf_,
                              width_,
                              height_,
                              pitch_,
                              pixel_format_);
    }
}
