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

#include "freedo_clio.h"
#include "freedo_clock.h"
#include "freedo_core.h"
#include "freedo_vdlp.h"

#include <string.h>

//#define NTSC_CLOCK      12270000
//#define PAL_CLOCK       14750000

#define DEFAULT_CPU_FREQUENCY    12500000
#define DEFAULT_CYCLES_PER_FIELD CYCLES_PER_FIELD(DEFAULT_CPU_FREQUENCY,NTSC_FIELD_RATE)
#define MIN_CPU_FREQUENCY        1000000
#define SND_CLOCK                44100
#define NTSC_FRAME_SIZE          526
#define NTSC_FRAME_RATE          2997
#define PAL_FRAME_RATE           2500
#define NTSC_FIELD_RATE          5994
#define PAL_FIELD_RATE           5000

#define CYCLES_PER_FIELD(X,Y) (((((uint64_t)X) * 100) / Y) + 1)


typedef struct freedo_clock_s freedo_clock_t;
struct freedo_clock_s
{
  uint32_t cpu_frequency;
  uint32_t dsp_acc;
  uint32_t vdl_acc;
  uint32_t timer_acc;
  uint32_t frames_per_second;
  uint32_t vdlline;
  uint32_t frame_size;
  uint32_t field_rate;
  uint64_t cycles_per_field;
};

static freedo_clock_t g_CLOCK =
  {
    DEFAULT_CPU_FREQUENCY,      /* cpu_frequency */
    0,                          /* dsp_acc */
    0,                          /* vdl_acc */
    0,                          /* timer_acc */
    30,                         /* fps */
    0,                          /* vdlline */
    NTSC_FRAME_SIZE,            /* frame_size */
    NTSC_FIELD_RATE,            /* field_rate */
    DEFAULT_CYCLES_PER_FIELD    /* cycles_per_field */
  };

void
freedo_clock_cpu_set_freq(const uint32_t freq_)
{
  uint32_t freq;

  freq = ((freq_ < MIN_CPU_FREQUENCY) ? MIN_CPU_FREQUENCY : freq_);

  g_CLOCK.cpu_frequency    = freq_;
  g_CLOCK.cycles_per_field = CYCLES_PER_FIELD(freq_,g_CLOCK.field_rate);
}

void
freedo_clock_cpu_set_freq_mul(const float mul_)
{
  float freq;

  freq = (DEFAULT_CPU_FREQUENCY * mul_);

  freedo_clock_cpu_set_freq((uint32_t)freq);
}

uint32_t
freedo_clock_cpu_get_freq(void)
{
  return g_CLOCK.cpu_frequency;
}

uint32_t
freedo_clock_cpu_get_default_freq(void)
{
  return DEFAULT_CPU_FREQUENCY;
}

uint64_t
freedo_clock_cpu_cycles_per_field(void)
{
  return g_CLOCK.cycles_per_field;
}

/* for backwards compatibility */
uint32_t
freedo_clock_state_size(void)
{
  return (sizeof(uint32_t) * 8);
}

void
freedo_clock_state_save(void *buf_)
{
  //memcpy(buf_,&g_CLOCK,sizeof(freedo_clock_t));
}

void
freedo_clock_state_load(const void *buf_)
{
  //memcpy(&g_CLOCK,buf_,sizeof(freedo_clock_t));
}

void
freedo_clock_init(void)
{
  g_CLOCK.dsp_acc    = 0;
  g_CLOCK.vdl_acc    = 0;
  g_CLOCK.timer_acc  = 0;
  g_CLOCK.fps        = 30;
  g_CLOCK.vdlline    = 0;
  g_CLOCK.frame_size = 526;
  g_CLOCK.field_rate = (6000000 / 1001);
  freedo_clock_cpu_set_freq(DEFAULT_CPU_FREQUENCY);
}

int
freedo_clock_vdl_current_line(void)
{
  return (g_CLOCK.vdlline % (g_CLOCK.frame_size >> 1));
}

int
freedo_clock_vdl_half_frame(void)
{
  return (g_CLOCK.vdlline / (g_CLOCK.frame_size >> 1));
}

int
freedo_clock_vdl_current_overline(void)
{
  return g_CLOCK.vdlline;
}

bool
freedo_clock_vdl_queued(void)
{
  const uint32_t limit = (g_CLOCK.cpu_frequency / (g_CLOCK.frame_size * g_CLOCK.fps));

  if(g_CLOCK.vdl_acc >= limit)
    {
      g_CLOCK.vdl_acc -= limit;
      g_CLOCK.vdlline = ((g_CLOCK.vdlline + 1) % g_CLOCK.frame_size);

      return true;
    }

  return false;
}

bool
freedo_clock_dsp_queued(void)
{
  const uint32_t limit = (g_CLOCK.cpu_frequency / SND_CLOCK);

  if(g_CLOCK.dsp_acc >= limit)
    {
      g_CLOCK.dsp_acc -= limit;

      return true;
    }

  return false;
}

/* Need to find out where the value 21000000 comes from. */
bool
freedo_clock_timer_queued(void)
{
  uint32_t limit;
  uint32_t timer_delay;

  timer_delay = freedo_clio_timer_get_delay();
  if(timer_delay == 0)
    return false;

  limit = (g_CLOCK.cpu_frequency / (21000000 / timer_delay));
  if(g_CLOCK.timer_acc >= limit)
    {
      g_CLOCK.timer_acc -= limit;

      return true;
    }

  return false;
}

void
freedo_clock_push_cycles(const uint32_t clks_)
{
  g_CLOCK.dsp_acc   += clks_;
  g_CLOCK.vdl_acc   += clks_;
  g_CLOCK.timer_acc += clks_;
}
