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

#define DEFAULT_CPU_FREQUENCY       12500000
#define DEFAULT_CYCLES_PER_FIELD    CYCLES_PER_FIELD(DEFAULT_CPU_FREQUENCY,NTSC_FIELD_RATE)
#define DEFAULT_CYCLES_PER_SND      (DEFAULT_CPU_FREQUENCY / SND_CLOCK)
#define DEFAULT_CYCLES_PER_SCANLINE CYCLES_PER_SCANLINE(DEFAULT_CPU_FREQUENCY,NTSC_FIELD_SIZE,NTSC_FIELD_RATE)
#define MIN_CPU_FREQUENCY           1000000
#define SND_CLOCK                   44100
#define NTSC_FIELD_SIZE             263
#define PAL_FIELD_SIZE              288
#define NTSC_FIELD_RATE             5994
#define PAL_FIELD_RATE              5000

#define CYCLES_PER_FIELD(X,Y)      (((((uint64_t)(X)) * 100) / (Y)) + 1)
#define CYCLES_PER_SCANLINE(X,Y,Z) (((uint64_t)(X) * 100) / ((Y) * (Z)))

typedef struct freedo_clock_s freedo_clock_t;
struct freedo_clock_s
{
  uint32_t cpu_frequency;
  uint32_t dsp_acc;
  uint32_t vdl_acc;
  uint32_t timer_acc;
  uint32_t vdlline;
  uint32_t field_size;
  uint32_t field_rate;
  uint32_t cycles_per_field;
  uint32_t cycles_per_snd;
  uint32_t cycles_per_scanline;
};

static freedo_clock_t g_CLOCK =
  {
    DEFAULT_CPU_FREQUENCY,      /* .cpu_frequency */
    0,                          /* .dsp_acc */
    0,                          /* .vdl_acc */
    0,                          /* .timer_acc */
    0,                          /* .vdlline */
    NTSC_FIELD_SIZE,            /* .field_size */
    NTSC_FIELD_RATE,            /* .field_rate */
    DEFAULT_CYCLES_PER_FIELD,   /* .cycles_per_field */
    DEFAULT_CYCLES_PER_SND,     /* .cycles_per_snd */
    DEFAULT_CYCLES_PER_SCANLINE /* .cycles_per_scanline */
  };


static
void
recalculate_cycles_per(void)
{
  g_CLOCK.cycles_per_field    = CYCLES_PER_FIELD(g_CLOCK.cpu_frequency,g_CLOCK.field_rate);
  g_CLOCK.cycles_per_snd      = (g_CLOCK.cpu_frequency / SND_CLOCK);
  g_CLOCK.cycles_per_scanline = CYCLES_PER_SCANLINE(g_CLOCK.cpu_frequency,g_CLOCK.field_size,g_CLOCK.field_rate);
}

void
freedo_clock_cpu_set_freq(const uint32_t freq_)
{
  uint32_t freq;

  freq = ((freq_ < MIN_CPU_FREQUENCY) ? MIN_CPU_FREQUENCY : freq_);

  g_CLOCK.cpu_frequency = freq_;

  recalculate_cycles_per();
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
  /* memcpy(buf_,&g_CLOCK,sizeof(freedo_clock_t)); */
}

void
freedo_clock_state_load(const void *buf_)
{
  /* memcpy(&g_CLOCK,buf_,sizeof(freedo_clock_t)); */
}

void
freedo_clock_init(void)
{
  g_CLOCK.cpu_frequency       = DEFAULT_CPU_FREQUENCY;
  g_CLOCK.dsp_acc             = 0;
  g_CLOCK.vdl_acc             = 0;
  g_CLOCK.timer_acc           = 0;
  g_CLOCK.vdlline             = 0;
  g_CLOCK.field_size          = NTSC_FIELD_SIZE;
  g_CLOCK.field_rate          = NTSC_FIELD_RATE;
  g_CLOCK.cycles_per_field    = DEFAULT_CYCLES_PER_FIELD;
  g_CLOCK.cycles_per_snd      = DEFAULT_CYCLES_PER_SND;
  g_CLOCK.cycles_per_scanline = DEFAULT_CYCLES_PER_SCANLINE;
}

int
freedo_clock_vdl_current_line(void)
{
  return (g_CLOCK.vdlline % g_CLOCK.field_size);
}

int
freedo_clock_vdl_current_field(void)
{
  return (g_CLOCK.vdlline / g_CLOCK.field_size);
}

bool
freedo_clock_vdl_queued(void)
{
  const uint32_t limit = g_CLOCK.cycles_per_scanline;

  if(g_CLOCK.vdl_acc >= limit)
    {
      g_CLOCK.vdl_acc -= limit;
      g_CLOCK.vdlline  = ((g_CLOCK.vdlline + 1) % (g_CLOCK.field_size << 1));

      return true;
    }

  return false;
}

bool
freedo_clock_dsp_queued(void)
{
  const uint32_t limit = g_CLOCK.cycles_per_snd;

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

void
freedo_clock_region_set_ntsc(void)
{
  g_CLOCK.field_rate = NTSC_FIELD_RATE;
  g_CLOCK.field_size = NTSC_FIELD_SIZE;

  recalculate_cycles_per();
}

void
freedo_clock_region_set_pal(void)
{
  g_CLOCK.field_rate = PAL_FIELD_RATE;
  g_CLOCK.field_size = PAL_FIELD_SIZE;

  recalculate_cycles_per();
}
