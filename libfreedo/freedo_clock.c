#include "freedo_clio.h"
#include "freedo_clock.h"
#include "freedo_core.h"
#include "freedo_vdlp.h"

#define DEFAULT_CPU_FREQ     12500000UL
#define MIN_CPU_FREQ         1000000UL
#define SND_FREQ             44100UL
#define TIMER_FREQ           65536UL
#define NTSC_FIELD_SIZE      263UL
#define PAL_FIELD_SIZE       312UL
#define NTSC_FIELD_RATE_1616 3928227UL
#define PAL_FIELD_RATE_1616  3276800UL

typedef struct freedo_clock_s freedo_clock_t;
struct freedo_clock_s
{
  uint32_t cpu_freq;
  int32_t  dsp_acc;
  int32_t  vdl_acc;
  int32_t  timer_acc;
  uint32_t field_size;
  uint32_t field_rate;
  int32_t  cycles_per_snd;
  int32_t  cycles_per_scanline;
  int32_t  cycles_per_timer;
};

static freedo_clock_t g_CLOCK;


static
uint32_t
calc_cycles_per_snd(void)
{
  uint64_t rv;

  rv  = ((uint64_t)g_CLOCK.cpu_freq << 16);
  rv /= ((uint64_t)SND_FREQ);

  return rv;
}

/*
  For greater percision the field rate is stored as 16.16 so cpu_freq
  requires the 32 rather than 16 LSL
*/
static
uint32_t
calc_cycles_per_scanline(void)
{
  uint64_t rv;

  rv  = ((uint64_t)g_CLOCK.cpu_freq << 32);
  rv /= ((uint64_t)g_CLOCK.field_rate * (uint64_t)g_CLOCK.field_size);

  return rv;
}

static
uint32_t
calc_cycles_per_timer(void)
{
  uint64_t rv;

  rv  = ((uint64_t)g_CLOCK.cpu_freq << 16);
  rv /= ((uint64_t)TIMER_FREQ);

  return rv;
}

static
void
recalculate_cycles_per(void)
{
  g_CLOCK.cycles_per_snd      = calc_cycles_per_snd();
  g_CLOCK.cycles_per_scanline = calc_cycles_per_scanline();
  g_CLOCK.cycles_per_timer    = calc_cycles_per_timer();
}

void
freedo_clock_cpu_set_freq(const uint32_t freq_)
{
  uint32_t freq;

  freq = ((freq_ < MIN_CPU_FREQ) ? MIN_CPU_FREQ : freq_);

  g_CLOCK.cpu_freq = freq_;

  recalculate_cycles_per();
}

void
freedo_clock_cpu_set_freq_mul(const float mul_)
{
  float freq;

  freq = (DEFAULT_CPU_FREQ * mul_);

  freedo_clock_cpu_set_freq((uint32_t)freq);
}

uint32_t
freedo_clock_cpu_get_freq(void)
{
  return g_CLOCK.cpu_freq;
}

uint32_t
freedo_clock_cpu_get_default_freq(void)
{
  return DEFAULT_CPU_FREQ;
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
  g_CLOCK.cpu_freq   = DEFAULT_CPU_FREQ;
  g_CLOCK.dsp_acc    = 0;
  g_CLOCK.vdl_acc    = 0;
  g_CLOCK.timer_acc  = 0;
  g_CLOCK.field_size = NTSC_FIELD_SIZE;
  g_CLOCK.field_rate = NTSC_FIELD_RATE_1616;

  recalculate_cycles_per();
}

int
freedo_clock_vdl_queued(void)
{
  if(g_CLOCK.vdl_acc >= g_CLOCK.cycles_per_scanline)
    {
      g_CLOCK.vdl_acc -= g_CLOCK.cycles_per_scanline;
      return true;
    }

  return false;
}

int
freedo_clock_dsp_queued(void)
{
  if(g_CLOCK.dsp_acc >= g_CLOCK.cycles_per_snd)
    {
      g_CLOCK.dsp_acc -= g_CLOCK.cycles_per_snd;
      return true;
    }

  return false;
}

/*
  It's not clear what register 0x220 of the CLIO does in relation to
  the timer. Need to test on the real hardware.
 */
int
freedo_clock_timer_queued(void)
{
  if(g_CLOCK.timer_acc >= g_CLOCK.cycles_per_timer)
    {
      g_CLOCK.timer_acc -= g_CLOCK.cycles_per_timer;
      return true;
    }

  return false;
}

void
freedo_clock_push_cycles(const uint32_t clks_)
{
  uint32_t clks1616;

  clks1616 = (clks_ << 16);
  g_CLOCK.dsp_acc   += clks1616;
  g_CLOCK.vdl_acc   += clks1616;
  g_CLOCK.timer_acc += clks1616;
}

void
freedo_clock_region_set_ntsc(void)
{
  g_CLOCK.field_rate = NTSC_FIELD_RATE_1616;
  g_CLOCK.field_size = NTSC_FIELD_SIZE;

  recalculate_cycles_per();
}

void
freedo_clock_region_set_pal(void)
{
  g_CLOCK.field_rate = PAL_FIELD_RATE_1616;
  g_CLOCK.field_size = PAL_FIELD_SIZE;

  recalculate_cycles_per();
}
