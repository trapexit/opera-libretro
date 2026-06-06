/*
  www.freedo.org
  The first working 3DO multiplayer emulator.

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

#include "opera_arm.h"
#include "opera_clio.h"
#include "opera_clock.h"
#include "opera_dsp.h"
#include "opera_madam.h"
#include "opera_mem.h"
#include "opera_state.h"
#include "opera_xbus.h"

#include "boolean.h"
#include "prng32.h"

#include <string.h>

#define CLIO_REGISTER_COUNT      65536
#define CLIO_INPUT_FIFO_COUNT    13
#define CLIO_OUTPUT_FIFO_COUNT   4
#define CLIO_FIFO_REG_STRIDE     0x10
#define CLIO_FIFO_ADDR_MASK      0x0F
#define CLIO_FIFO_CHANNEL_MASK   0x0F
#define CLIO_FIFO_CHANNEL_SHIFT  4
#define CLIO_FIFO_INPUT_MASK     0x500
#define CLIO_FIFO_INPUT_VALUE    0x400
#define CLIO_FIFO_LENGTH_BIAS    4

#define CLIO_REG_CLIOREV          0x0000
#define CLIO_REG_VINT0            0x0008
#define CLIO_REG_VINT1            0x000C
#define CLIO_REG_CSTATBITS        0x0028
#define CLIO_REG_WDOG             0x002C
#define CLIO_REG_HCNT             0x0030
#define CLIO_REG_VCNT             0x0034
#define CLIO_REG_RAND             0x003C
#define CLIO_REG_SETINT0BITS      0x0040
#define CLIO_REG_CLRINT0BITS      0x0044
#define CLIO_REG_SETINT0ENBITS    0x0048
#define CLIO_REG_CLRINT0ENBITS    0x004C
#define CLIO_REG_SETMODE          0x0050
#define CLIO_REG_CLRMODE          0x0054
#define CLIO_REG_SETINT1BITS      0x0060
#define CLIO_REG_CLRINT1BITS      0x0064
#define CLIO_REG_SETINT1ENBITS    0x0068
#define CLIO_REG_CLRINT1ENBITS    0x006C
#define CLIO_REG_ADBIO            0x0084

#define CLIO_REG_TIMER0           0x0100
#define CLIO_REG_TIMER15_BACK     0x017C
#define CLIO_TIMER_REG_STRIDE     0x04
#define CLIO_TIMER_PAIR_STRIDE    0x08
#define CLIO_REG_SETTM0           0x0200
#define CLIO_REG_CLRTM0           0x0204
#define CLIO_REG_SETTM1           0x0208
#define CLIO_REG_CLRTM1           0x020C
#define CLIO_REG_SLACK            0x0220
#define CLIO_REG_FIFOINIT         0x0300
#define CLIO_REG_DMAREQEN         0x0304
#define CLIO_REG_DMAREQDIS        0x0308
#define CLIO_REG_SETEXPCTL        0x0400
#define CLIO_REG_CLREXPCTL        0x0404
#define CLIO_REG_DIPIR2           0x0414
#define CLIO_REG_EXP_SEL_FIRST    0x0500
#define CLIO_REG_EXP_SEL_END      0x0540
#define CLIO_REG_EXP_POLL_FIRST   0x0540
#define CLIO_REG_EXP_POLL_END     0x0580
#define CLIO_REG_EXP_CMD_FIRST    0x0580
#define CLIO_REG_EXP_CMD_END      0x05C0
#define CLIO_REG_EXP_DATA_FIRST   0x05C0
#define CLIO_REG_EXP_DATA_END     0x0600

#define CLIO_REG_SEMAPHORE        0x17D0
#define CLIO_REG_DSPPRST1         0x17E8
#define CLIO_REG_NOISE            0x17F0
#define CLIO_REG_DSPPGW           0x17FC
#define CLIO_REG_DSPPN32_FIRST    0x1800
#define CLIO_REG_DSPPN32_LAST     0x1FFF
#define CLIO_REG_DSPPN16_FIRST    0x2000
#define CLIO_REG_DSPPN16_LAST     0x2FFF
#define CLIO_REG_DSPPEI32_FIRST   0x3000
#define CLIO_REG_DSPPEI32_LAST    0x33FF
#define CLIO_REG_DSPPEI16_FIRST   0x3400
#define CLIO_REG_DSPPEI16_LAST    0x37FF
#define CLIO_REG_DSPPEO32_FIRST   0x3800
#define CLIO_REG_DSPPEO32_LAST    0x3BFF
#define CLIO_REG_DSPPEO16_FIRST   0x3C00
#define CLIO_REG_DSPPEO16_LAST    0x3FFF

#define CLIO_GREEN       0x02020000
#define CLIO_SoftReset   0x10
#define CLIO_ClearDIPIR  0x20
#define CLIO_DIPIRReset  0x40

#define VCNT_MASK          0x000007FF
#define VCNT_FIELD         0x00000800
#define VCNT_FIELD_SHIFT   11

#define INT0_TIMINT1       0x00000400
#define INT0_DDRINT0       0x00001000
#define INT0_DRDINT0       0x00010000
#define INT0_DEXINT        0x20000000
#define INT0_SCNDPINT      0x80000000

#define I_DMAtoDSPP_MASK   0x00001FFF
#define I_DSPPtoDMA_MASK   0x000F0000
#define I_DSPPtoDMA_SHIFT  16

#define EN_DMAtofrEXP      0x00100000

#define XB_CPUHASXBUS      (1 << 7)
#define XB_DMAISOUT        (1 << 9)
#define XB_DMAON           (1 << 11)
#define XB_DipirNOSR       0x4000

#define ADBIO_COUNT         4
#define ADBIO_ENABLE_SHIFT  4

#define TIMER_DECREMENT  0x1
#define TIMER_RELOAD     0x2
#define TIMER_CASCADE    0x4

#define VINT_MASK    VCNT_MASK
#define HCNT_MASK    VCNT_MASK

#define WDOG_RESTART       0x0B
#define WDOG_RESTART_MASK  0x0F

#define CLIO_FIQ_REG_ALIAS_MASK  0x2C
#define CLIO_SETMODE_MASK        0x3FFF0000
#define CLIO_WORD_MASK           0xFFFF
#define CLIO_WORD_SHIFT          16
#define CLIO_TIMER_MASK          0x0F
#define CLIO_TIMER_CONTROL_MASK  0x07
#define CLIO_TIMER_CONTROL_SHIFT 2
#define CLIO_TIMER_SET_COUNT     8
#define CLIO_TIMER_SLACK_MASK    0x3FF
#define CLIO_DEFAULT_TIMER_SLACK 64
#define CLIO_DMA_TIMER_VAL_LIMIT 5800
#define CLIO_DMA_TIMER_VAL_STEP  0x33
#define CLIO_DMA_TIMER_FREQ_DIV  2000000

#define CLIO_DSPPN32_MIRROR_MASK  0x400
#define CLIO_DSPPN16_MIRROR_MASK  0x800
#define CLIO_DSPP_IMEM_MASK       0xFF
#define CLIO_DSPP_IMEM_READ_BASE  0x300

#define FIFO_Count1  0x00000002
#define FIFO_Count0  0x00000001

#define MADAM_REG_DMATOFR_EXP0_CURADR  0x540
#define MADAM_REG_DMATOFR_EXP0_CURLEN  0x544
#define MADAM_DMA_IDLE_LEN             0xFFFFFFFC

#define MADAM_DMA_CURADR_OFFSET  0x00
#define MADAM_DMA_CURLEN_OFFSET  0x04
#define MADAM_DMA_RLDADR_OFFSET  0x08
#define MADAM_DMA_RLDLEN_OFFSET  0x0C

struct fifo_s
{
  uint32_t addr;
  int32_t  len;
};

typedef struct fifo_s fifo_t;

struct clio_fifo_s
{
  int32_t idx;
  fifo_t  start;
  fifo_t  next;
};

typedef struct clio_fifo_s clio_fifo_t;

struct clio_s
{
  uint32_t    regs[CLIO_REGISTER_COUNT];
  int32_t     dsp_word1;
  int32_t     dsp_word2;
  int32_t     dsp_address;
  clio_fifo_t fifo_i[CLIO_INPUT_FIFO_COUNT];
  clio_fifo_t fifo_o[CLIO_OUTPUT_FIFO_COUNT];
};

typedef struct clio_s clio_t;

int flagtime;
int TIMER_VAL = 0;

static uint32_t *MADAM_REGS;
static clio_t    CLIO = {0};
static uint32_t  TIMER_CARRY = 0;

static
bool
clio_timer_reg_addr(uint32_t addr_)
{
  return ((addr_ >= CLIO_REG_TIMER0) &&
          (addr_ <= CLIO_REG_TIMER15_BACK) &&
          !(addr_ & (CLIO_TIMER_REG_STRIDE - 1)));
}

static
uint32_t
timer_control_shift(const uint32_t timer_)
{
  return ((timer_ & CLIO_TIMER_CONTROL_MASK) << CLIO_TIMER_CONTROL_SHIFT);
}

static
void
clio_timer_regs_mask(void)
{
  uint32_t addr;

  for(addr = CLIO_REG_TIMER0; addr <= CLIO_REG_TIMER15_BACK;
      addr += CLIO_TIMER_REG_STRIDE)
    CLIO.regs[addr] &= CLIO_WORD_MASK;
}

static
void
opera_clio_set_rom()
{
  opera_mem_rom_select((CLIO.regs[CLIO_REG_ADBIO] & ADBIO_OTHERROM) ?
                       ROM2 : ROM1);
}

uint32_t
opera_clio_state_size_v1(void)
{
  return opera_state_save_size(sizeof(clio_t));
}

static
bool
clio_state_write_fifo(opera_state_writer_t *writer_,
                      clio_fifo_t const    *fifo_)
{
  return (opera_state_write_i32(writer_,fifo_->idx) &&
          opera_state_write_u32(writer_,fifo_->start.addr) &&
          opera_state_write_i32(writer_,fifo_->start.len) &&
          opera_state_write_u32(writer_,fifo_->next.addr) &&
          opera_state_write_i32(writer_,fifo_->next.len));
}

static
bool
clio_state_write_payload(opera_state_writer_t *writer_,
                         clio_t const         *state_)
{
  uint32_t i;

  if(!opera_state_write_u32_array(writer_,state_->regs,CLIO_REGISTER_COUNT) ||
     !opera_state_write_i32(writer_,state_->dsp_word1) ||
     !opera_state_write_i32(writer_,state_->dsp_word2) ||
     !opera_state_write_i32(writer_,state_->dsp_address))
    return false;

  for(i = 0; i < CLIO_INPUT_FIFO_COUNT; i++)
    if(!clio_state_write_fifo(writer_,&state_->fifo_i[i]))
      return false;
  for(i = 0; i < CLIO_OUTPUT_FIFO_COUNT; i++)
    if(!clio_state_write_fifo(writer_,&state_->fifo_o[i]))
      return false;

  return (opera_state_write_i32(writer_,flagtime) &&
          opera_state_write_i32(writer_,TIMER_VAL) &&
          opera_state_write_u32(writer_,TIMER_CARRY));
}

static
uint32_t
clio_state_payload_size(void)
{
  opera_state_writer_t writer;

  opera_state_writer_init(&writer,NULL,UINT32_MAX);
  clio_state_write_payload(&writer,&CLIO);

  return opera_state_writer_used(&writer);
}

uint32_t
opera_clio_state_size(void)
{
  return opera_state_chunk_size(clio_state_payload_size());
}

uint32_t
opera_clio_state_save(void *buf_)
{
  uint32_t payload_size;
  opera_state_writer_t writer;

  payload_size = clio_state_payload_size();
  opera_state_writer_init(&writer,buf_,opera_state_chunk_size(payload_size));
  opera_state_write_chunk_header(&writer,"CLIO",payload_size);
  clio_state_write_payload(&writer,&CLIO);

  return opera_state_writer_ok(&writer) ? opera_state_writer_used(&writer) : 0;
}

uint32_t
opera_clio_state_load_v1(const void     *buf_,
                         uint32_t const  size_)
{
  uint32_t rv;

  rv = opera_state_load_sized(&CLIO,"CLIO",buf_,size_,sizeof(CLIO));
  if(rv != 0)
    {
      TIMER_VAL = 0;
      flagtime = 0;
      TIMER_CARRY = 0;
      opera_clio_set_rom();
    }

  return rv;
}

static
bool
clio_state_read_fifo(opera_state_reader_t *reader_,
                     clio_fifo_t          *fifo_)
{
  return (opera_state_read_i32(reader_,&fifo_->idx) &&
          opera_state_read_u32(reader_,&fifo_->start.addr) &&
          opera_state_read_i32(reader_,&fifo_->start.len) &&
          opera_state_read_u32(reader_,&fifo_->next.addr) &&
          opera_state_read_i32(reader_,&fifo_->next.len));
}

static
bool
clio_state_read_payload(opera_state_reader_t *reader_,
                        clio_t               *state_,
                        int32_t              *flagtime_state_,
                        int32_t              *timer_val_state_,
                        uint32_t             *timer_carry_state_)
{
  uint32_t i;

  if(!opera_state_read_u32_array(reader_,state_->regs,CLIO_REGISTER_COUNT) ||
     !opera_state_read_i32(reader_,&state_->dsp_word1) ||
     !opera_state_read_i32(reader_,&state_->dsp_word2) ||
     !opera_state_read_i32(reader_,&state_->dsp_address))
    return false;

  for(i = 0; i < CLIO_INPUT_FIFO_COUNT; i++)
    if(!clio_state_read_fifo(reader_,&state_->fifo_i[i]))
      return false;
  for(i = 0; i < CLIO_OUTPUT_FIFO_COUNT; i++)
    if(!clio_state_read_fifo(reader_,&state_->fifo_o[i]))
      return false;

  if(!opera_state_read_i32(reader_,flagtime_state_) ||
     !opera_state_read_i32(reader_,timer_val_state_) ||
     !opera_state_read_u32(reader_,timer_carry_state_))
    return false;

  return true;
}

uint32_t
opera_clio_state_load(const void     *buf_,
                      uint32_t const  size_)
{
  clio_t state;
  int32_t flagtime_state;
  int32_t timer_val_state;
  uint32_t timer_carry_state;
  opera_state_reader_t reader;
  opera_state_reader_t payload;

  opera_state_reader_init(&reader,buf_,size_);
  if(!opera_state_read_chunk(&reader,"CLIO",&payload) ||
     !clio_state_read_payload(&payload,
                              &state,
                              &flagtime_state,
                              &timer_val_state,
                              &timer_carry_state) ||
     !opera_state_reader_finished(&payload))
    return 0;

  CLIO = state;
  flagtime = flagtime_state;
  TIMER_VAL = timer_val_state;
  TIMER_CARRY = timer_carry_state;
  opera_clio_set_rom();

  return opera_state_reader_used(&reader);
}

#define CURADR MADAM_REGS[base + MADAM_DMA_CURADR_OFFSET]
#define CURLEN MADAM_REGS[base + MADAM_DMA_CURLEN_OFFSET]
#define RLDADR MADAM_REGS[base + MADAM_DMA_RLDADR_OFFSET]
#define RLDLEN MADAM_REGS[base + MADAM_DMA_RLDLEN_OFFSET]

void
opera_clio_timer_set(uint32_t v200_,
                     uint32_t v208_)
{
  (void) v200_;
  (void) v208_;
}

void
opera_clio_timer_clear(uint32_t v204_,
                       uint32_t v20C_)
{
  (void) v204_;
  (void) v20C_;
}

uint32_t
opera_clio_line_vint0(void)
{
  return (CLIO.regs[CLIO_REG_VINT0] & VINT_MASK);
}

uint32_t
opera_clio_line_vint1(void)
{
  return (CLIO.regs[CLIO_REG_VINT1] & VINT_MASK);
}

int
opera_clio_fiq_needed(void)
{
  return ((CLIO.regs[CLIO_REG_SETINT0BITS] &
           CLIO.regs[CLIO_REG_SETINT0ENBITS]) ||
          (CLIO.regs[CLIO_REG_SETINT1BITS] &
           CLIO.regs[CLIO_REG_SETINT1ENBITS]));
}

void
opera_clio_fiq_generate(uint32_t reason1_,
                        uint32_t reason2_)
{
  CLIO.regs[CLIO_REG_SETINT0BITS] |= reason1_;
  CLIO.regs[CLIO_REG_SETINT1BITS] |= reason2_;
  /* irq31 if exist irq32 and high */
  if(CLIO.regs[CLIO_REG_SETINT1BITS])
    CLIO.regs[CLIO_REG_SETINT0BITS] |= INT0_SCNDPINT;
}

/*
  Documentation:
  * https://github.com/trapexit/portfolio_os/blob/master/src/kernel/includes/clio.h

  | Register           | Description
  |--------------------|------------
  | CLIO_REG_FIFOINIT  | FIFO init group masks
  |                    | I_DMAtoDSPP_MASK, I_DSPPtoDMA_MASK
  | CLIO_REG_DMAREQEN  | enable DMA
  |                    | EN_DMAtofrEXP and related DMA enable bits
  | CLIO_REG_DMAREQDIS | disable / clear DMA
  |                    | bits same as above
*/
static
void
clio_handle_dma(uint32_t val_)
{
  CLIO.regs[CLIO_REG_DMAREQEN] |= val_;

  if(val_ & EN_DMAtofrEXP)
    {
      int len;
      unsigned trg;
      uint8_t b0,b1,b2,b3;

      trg = opera_madam_peek(MADAM_REG_DMATOFR_EXP0_CURADR);
      len = opera_madam_peek(MADAM_REG_DMATOFR_EXP0_CURLEN);
      CLIO.regs[CLIO_REG_DMAREQEN] &= ~EN_DMAtofrEXP;
      CLIO.regs[CLIO_REG_SETEXPCTL] &= ~XB_CPUHASXBUS;

      if(CLIO.regs[CLIO_REG_CLREXPCTL] & XB_DMAISOUT)
        {
          while(len >= 0)
            {
              b3 = opera_xbus_fifo_get_data();
              b2 = opera_xbus_fifo_get_data();
              b1 = opera_xbus_fifo_get_data();
              b0 = opera_xbus_fifo_get_data();

              opera_mem_write8(trg+0,b3);
              opera_mem_write8(trg+1,b2);
              opera_mem_write8(trg+2,b1);
              opera_mem_write8(trg+3,b0);

              trg += 4;
              len -= 4;
            }

          CLIO.regs[CLIO_REG_SETEXPCTL] |= XB_CPUHASXBUS;
        }
      else
        {
          while(len >= 0)
            {
              b3 = opera_xbus_fifo_get_data();
              b2 = opera_xbus_fifo_get_data();
              b1 = opera_xbus_fifo_get_data();
              b0 = opera_xbus_fifo_get_data();

              opera_mem_write8(trg+0,b3);
              opera_mem_write8(trg+1,b2);
              opera_mem_write8(trg+2,b1);
              opera_mem_write8(trg+3,b0);

              trg += 4;
              len -= 4;
            }

          CLIO.regs[CLIO_REG_SETEXPCTL] |= XB_CPUHASXBUS;
        }

      opera_madam_poke(MADAM_REG_DMATOFR_EXP0_CURLEN,MADAM_DMA_IDLE_LEN);
      opera_clio_fiq_generate(INT0_DEXINT,0);
    }
}

static
void
adbio_set(uint32_t *output_,
          uint32_t  val_,
          uint32_t  bit_)
{
  uint32_t mask;

  mask = (1 << bit_);

  if((val_ & mask) == 0)
    *output_ &= ~mask;
  else if(val_ & (mask | (mask << ADBIO_ENABLE_SHIFT)))
    *output_ |= mask;
}

int
opera_clio_poke(uint32_t addr_,
                uint32_t val_)
{
  int base;
  int i;

  if(!flagtime)
    TIMER_VAL = 0;

  /* ClioInt0/ClioInt1 register aliases. */
  if((addr_ & ~CLIO_FIQ_REG_ALIAS_MASK) == CLIO_REG_SETINT0BITS)
    {
      if(addr_ == CLIO_REG_SETINT0BITS)
        {
          CLIO.regs[CLIO_REG_SETINT0BITS] |= val_;
          if(CLIO.regs[CLIO_REG_SETINT1BITS])
            CLIO.regs[CLIO_REG_SETINT0BITS] |= INT0_SCNDPINT;
          /*
            if(CLIO.regs[CLIO_REG_SETINT0BITS]&CLIO.regs[CLIO_REG_SETINT0ENBITS])
            _arm_SetFIQ();
          */
          return 0;
        }
      else if(addr_ == CLIO_REG_CLRINT0BITS)
        {
          CLIO.regs[CLIO_REG_SETINT0BITS] &= ~val_;
          if(!CLIO.regs[CLIO_REG_SETINT1BITS])
            CLIO.regs[CLIO_REG_SETINT0BITS] &= ~INT0_SCNDPINT;
          return 0;
        }
      else if(addr_ == CLIO_REG_SETINT0ENBITS)
        {
          CLIO.regs[CLIO_REG_SETINT0ENBITS] |= val_;
          /*
            if(CLIO.regs[CLIO_REG_SETINT0BITS] & CLIO.regs[CLIO_REG_SETINT0ENBITS])
            _arm_SetFIQ();
          */
          return 0;
        }
      else if(addr_ == CLIO_REG_CLRINT0ENBITS)
        {
          /* always one for irq31 */
          CLIO.regs[CLIO_REG_SETINT0ENBITS] &= ~val_;
          CLIO.regs[CLIO_REG_SETINT0ENBITS] |= INT0_SCNDPINT;
          return 0;
        }
#if 0
      else if(addr_ == CLIO_REG_SETMODE)
        {
          CLIO.regs[CLIO_REG_SETMODE] |= (val_ & CLIO_SETMODE_MASK);
          return 0;
        }
      else if(addr_ == CLIO_REG_CLRMODE)
        {
          CLIO.regs[CLIO_REG_SETMODE] &= ~val;
          return 0;
        }
#endif
      else if(addr_ == CLIO_REG_SETINT1BITS)
        {
          CLIO.regs[CLIO_REG_SETINT1BITS] |= val_;
          if(CLIO.regs[CLIO_REG_SETINT1BITS])
            CLIO.regs[CLIO_REG_SETINT0BITS] |= INT0_SCNDPINT;
          /*
            if(CLIO.regs[CLIO_REG_SETINT1BITS] & CLIO.regs[CLIO_REG_SETINT1ENBITS])
            _arm_SetFIQ();
          */
          return 0;
        }
      else if(addr_ == CLIO_REG_CLRINT1BITS)
        {
          CLIO.regs[CLIO_REG_SETINT1BITS] &= ~val_;
          if(!CLIO.regs[CLIO_REG_SETINT1BITS])
            CLIO.regs[CLIO_REG_SETINT0BITS] &= ~INT0_SCNDPINT;
          return 0;
        }
      else if(addr_ == CLIO_REG_SETINT1ENBITS)
        {
          CLIO.regs[CLIO_REG_SETINT1ENBITS] |= val_;
          /*
            if(CLIO.regs[CLIO_REG_SETINT1BITS] & CLIO.regs[CLIO_REG_SETINT1ENBITS])
            _arm_SetFIQ();
          */
          return 0;
        }
      else if(addr_ == CLIO_REG_CLRINT1ENBITS)
        {
          CLIO.regs[CLIO_REG_SETINT1ENBITS] &= ~val_;
          return 0;
        }
    }
  else if(addr_ == CLIO_REG_ADBIO)
    {
      for(i = 0; i < ADBIO_COUNT; i++)
        adbio_set(&CLIO.regs[CLIO_REG_ADBIO],val_,i);

      opera_clio_set_rom();

      return 0;
    }
  else if(addr_ == CLIO_REG_FIFOINIT)
    {
      /* clear down the fifos and stop them */
      base = 0;
      CLIO.regs[CLIO_REG_DMAREQEN] &= ~val_;

      for(i = 0; i < CLIO_INPUT_FIFO_COUNT; i++)
        {
          if(val_ & (1 << i))
            {
              base = (CLIO_REG_SETEXPCTL + (i * CLIO_FIFO_REG_STRIDE));
              RLDADR = CURADR = 0;
              RLDLEN = CURLEN = 0;
              opera_clio_fifo_write(base + MADAM_DMA_CURADR_OFFSET,0);
              opera_clio_fifo_write(base + MADAM_DMA_CURLEN_OFFSET,0);
              opera_clio_fifo_write(base + MADAM_DMA_RLDADR_OFFSET,0);
              opera_clio_fifo_write(base + MADAM_DMA_RLDLEN_OFFSET,0);
              val_ &= ~(1 << i);
              CLIO.fifo_i[i].idx = 0;
            }
        }

      for(i = 0; i < CLIO_OUTPUT_FIFO_COUNT; i++)
        {
          if(val_ & (1 << (i + I_DSPPtoDMA_SHIFT)))
            {
              base = (CLIO_REG_EXP_SEL_FIRST + (i * CLIO_FIFO_REG_STRIDE));
              RLDADR = CURADR = 0;
              RLDLEN = CURLEN = 0;
              opera_clio_fifo_write(base + MADAM_DMA_CURADR_OFFSET,0);
              opera_clio_fifo_write(base + MADAM_DMA_CURLEN_OFFSET,0);
              opera_clio_fifo_write(base + MADAM_DMA_RLDADR_OFFSET,0);
              opera_clio_fifo_write(base + MADAM_DMA_RLDLEN_OFFSET,0);
              val_ &= ~(1 << (i + I_DSPPtoDMA_SHIFT));
              CLIO.fifo_o[i].idx = 0;
            }
        }

      return 0;
    }
  else if(addr_ == CLIO_REG_DMAREQEN)
    {
      clio_handle_dma(val_);

      /* TODO: looks hacky, needs to be investigated */
      switch(val_)
        {
        case EN_DMAtofrEXP:
          if(TIMER_VAL < CLIO_DMA_TIMER_VAL_LIMIT)
            TIMER_VAL += CLIO_DMA_TIMER_VAL_STEP;
          flagtime = (opera_clock_cpu_get_freq() / CLIO_DMA_TIMER_FREQ_DIV);
          break;
        default:
          if(!CLIO.regs[CLIO_REG_DMAREQEN])
            TIMER_VAL = 0;
          break;
        }
      return 0;
    }
  else if(addr_ == CLIO_REG_DMAREQDIS)
    {
      CLIO.regs[CLIO_REG_DMAREQEN] &= ~val_;
      return 0;
    }
  else if(addr_ == CLIO_REG_SETEXPCTL)
    {
      if(val_ & XB_DMAON)
        return 0;

      CLIO.regs[CLIO_REG_SETEXPCTL] = val_;
      return 0;
    }
  else if((addr_ >= CLIO_REG_EXP_SEL_FIRST) && (addr_ < CLIO_REG_EXP_SEL_END))
    {
      opera_xbus_set_sel(val_);
      return 0;
    }
  else if((addr_ >= CLIO_REG_EXP_POLL_FIRST) && (addr_ < CLIO_REG_EXP_POLL_END))
    {
      opera_xbus_set_poll(val_);
      return 0;
    }
  else if((addr_ >= CLIO_REG_EXP_CMD_FIRST) && (addr_ < CLIO_REG_EXP_CMD_END))
    {
      /* on FIFO Filled execute the command */
      opera_xbus_fifo_set_cmd(val_);
      return 0;
    }
  else if((addr_ >= CLIO_REG_EXP_DATA_FIRST) && (addr_ < CLIO_REG_EXP_DATA_END))
    {
      /* on FIFO Filled execute the command */
      opera_xbus_fifo_set_data(val_);
      return 0;
    }
  else if(addr_ == CLIO_REG_CSTATBITS)
    {
      CLIO.regs[addr_] = (val_ & ~(CLIO_SoftReset | CLIO_ClearDIPIR));
      if(val_ & CLIO_ClearDIPIR)
        CLIO.regs[addr_] &= ~CLIO_DIPIRReset;
      return (val_ & CLIO_SoftReset);
    }
  else if(addr_ == CLIO_REG_WDOG)
    {
      if((val_ & WDOG_RESTART_MASK) == WDOG_RESTART)
        CLIO.regs[addr_] = WDOG_RESTART;
      return 0;
    }
  else if((addr_ == CLIO_REG_VINT0) || (addr_ == CLIO_REG_VINT1))
    {
      CLIO.regs[addr_] = (val_ & VINT_MASK);
      return 0;
    }
  else if(addr_ == CLIO_REG_HCNT)
    {
      CLIO.regs[addr_] = (val_ & HCNT_MASK);
      return 0;
    }
  else if(addr_ == CLIO_REG_VCNT)
    {
      CLIO.regs[addr_] = (val_ & (VCNT_MASK | VCNT_FIELD));
      return 0;
    }
  else if((addr_ >= CLIO_REG_DSPPN32_FIRST) &&
          (addr_ <= CLIO_REG_DSPPN32_LAST))
    {
      addr_ &= ~CLIO_DSPPN32_MIRROR_MASK;
      CLIO.dsp_word1   = (val_ >> CLIO_WORD_SHIFT);
      CLIO.dsp_word2   = (val_ & CLIO_WORD_MASK);
      CLIO.dsp_address = ((addr_ - CLIO_REG_DSPPN32_FIRST) >> 1);
      opera_dsp_mem_write(CLIO.dsp_address+0,CLIO.dsp_word1);
      opera_dsp_mem_write(CLIO.dsp_address+1,CLIO.dsp_word2);
      return 0;
      /* DSPNRAMWrite 2 DSPW per 1ARMW */
    }
  else if((addr_ >= CLIO_REG_DSPPN16_FIRST) &&
          (addr_ <= CLIO_REG_DSPPN16_LAST))
    {
      addr_ &= ~CLIO_DSPPN16_MIRROR_MASK;
      CLIO.dsp_word1   = (val_ & CLIO_WORD_MASK);
      CLIO.dsp_address = ((addr_ - CLIO_REG_DSPPN16_FIRST) >> 2);
      opera_dsp_mem_write(CLIO.dsp_address,CLIO.dsp_word1);
      return 0;
    }
  else if((addr_ >= CLIO_REG_DSPPEI32_FIRST) &&
          (addr_ <= CLIO_REG_DSPPEI32_LAST))
    {
      CLIO.dsp_address  = ((addr_ - CLIO_REG_DSPPEI32_FIRST) >> 1);
      CLIO.dsp_address &= CLIO_DSPP_IMEM_MASK;
      CLIO.dsp_word1    = (val_ >> CLIO_WORD_SHIFT);
      CLIO.dsp_word2    = (val_ & CLIO_WORD_MASK);
      opera_dsp_imem_write(CLIO.dsp_address+0,CLIO.dsp_word1);
      opera_dsp_imem_write(CLIO.dsp_address+1,CLIO.dsp_word2);
      return 0;
    }
  else if((addr_ >= CLIO_REG_DSPPEI16_FIRST) &&
          (addr_ <= CLIO_REG_DSPPEI16_LAST))
    {
      CLIO.dsp_address  = ((addr_ - CLIO_REG_DSPPEI16_FIRST) >> 2);
      CLIO.dsp_address &= CLIO_DSPP_IMEM_MASK;
      CLIO.dsp_word1    = (val_ & CLIO_WORD_MASK);
      opera_dsp_imem_write(CLIO.dsp_address,CLIO.dsp_word1);
      return 0;
    }
  else if(addr_ == CLIO_REG_DSPPRST1)
    {
      opera_dsp_reset();
      return 0;
    }
  else if(addr_ == CLIO_REG_SEMAPHORE)
    {
      opera_dsp_arm_semaphore_write(val_);
      return 0;
    }
  else if(addr_ == CLIO_REG_DSPPGW)
    {
      opera_dsp_set_running(val_ > 0);
      return 0;
    }
  else if(addr_ == CLIO_REG_SETTM0)
    {
      CLIO.regs[CLIO_REG_SETTM0] |= val_;
      opera_clio_timer_set(val_,0);
      return 0;
    }
  else if(addr_ == CLIO_REG_CLRTM0)
    {
      CLIO.regs[CLIO_REG_SETTM0] &= ~val_;
      opera_clio_timer_clear(val_,0);
      return 0;
    }
  else if(addr_ == CLIO_REG_SETTM1)
    {
      CLIO.regs[CLIO_REG_SETTM1] |= val_;
      opera_clio_timer_set(0,val_);
      return 0;
    }
  else if(addr_ == CLIO_REG_CLRTM1)
    {
      CLIO.regs[CLIO_REG_SETTM1] &= ~val_;
      opera_clio_timer_clear(0,val_);
      return 0;
    }
  else if(addr_ == CLIO_REG_SLACK)
    {
      CLIO.regs[addr_] = (val_ & CLIO_TIMER_SLACK_MASK);
      opera_clock_timer_set_delay(CLIO.regs[addr_]);
      return 0;
    }
  else if(clio_timer_reg_addr(addr_))
    {
      CLIO.regs[addr_] = (val_ & CLIO_WORD_MASK);
      return 0;
    }

  CLIO.regs[addr_] = val_;

  return 0;
}

uint32_t
opera_clio_peek(uint32_t addr_)
{
  /* ClioInt0/ClioInt1 register aliases. */
  if((addr_ & ~CLIO_FIQ_REG_ALIAS_MASK) == CLIO_REG_SETINT0BITS)
    {
      /* By read 40 and 44, 48 and 4c, 60 and 64, 68 and 6c same */
      addr_ &= ~CLIO_TIMER_REG_STRIDE;
      if(addr_ == CLIO_REG_SETINT0BITS)
        return CLIO.regs[CLIO_REG_SETINT0BITS];
      else if(addr_ == CLIO_REG_SETINT0ENBITS)
        return (CLIO.regs[CLIO_REG_SETINT0ENBITS] | INT0_SCNDPINT);
      else if(addr_ == CLIO_REG_SETINT1BITS)
        return CLIO.regs[CLIO_REG_SETINT1BITS];
      else if(addr_ == CLIO_REG_SETINT1ENBITS)
        return CLIO.regs[CLIO_REG_SETINT1ENBITS];
      return 0;
    }
  else if(addr_ == CLIO_REG_RAND)
    return prng32();
  else if(addr_ == CLIO_REG_WDOG)
    return 0;
  else if(addr_ == CLIO_REG_CLRTM0)
    return CLIO.regs[CLIO_REG_SETTM0];
  else if(addr_ == CLIO_REG_CLRTM1)
    return CLIO.regs[CLIO_REG_SETTM1];
  else if(addr_ == CLIO_REG_DMAREQDIS)
    return CLIO.regs[CLIO_REG_DMAREQEN];
  else if (addr_ == CLIO_REG_DIPIR2)
    return XB_DipirNOSR; /* TO CHECK!!! requested by CDROMDIPIR */
  else if((addr_ >= CLIO_REG_EXP_SEL_FIRST) && (addr_ < CLIO_REG_EXP_SEL_END))
    return opera_xbus_get_res();
  else if((addr_ >= CLIO_REG_EXP_POLL_FIRST) && (addr_ < CLIO_REG_EXP_POLL_END))
    return opera_xbus_get_poll();
  else if((addr_ >= CLIO_REG_EXP_CMD_FIRST) && (addr_ < CLIO_REG_EXP_CMD_END))
    return opera_xbus_fifo_get_status();
  else if((addr_ >= CLIO_REG_EXP_DATA_FIRST) && (addr_ < CLIO_REG_EXP_DATA_END))
    return opera_xbus_fifo_get_data();
  else if(addr_ == CLIO_REG_CLIOREV)
    return CLIO_GREEN;
  else if((addr_ == CLIO_REG_VINT0) || (addr_ == CLIO_REG_VINT1))
    return (CLIO.regs[addr_] & VINT_MASK);
  else if(addr_ == CLIO_REG_HCNT)
    return (CLIO.regs[addr_] & HCNT_MASK);
  else if(addr_ == CLIO_REG_VCNT)
    return (CLIO.regs[addr_] & (VCNT_MASK | VCNT_FIELD));
  else if((addr_ >= CLIO_REG_DSPPEO32_FIRST) &&
          (addr_ <= CLIO_REG_DSPPEO32_LAST))
    {
      /* 2DSPW per 1ARMW */
      CLIO.dsp_address  = ((addr_ - CLIO_REG_DSPPEO32_FIRST) >> 1);
      CLIO.dsp_address &= CLIO_DSPP_IMEM_MASK;
      CLIO.dsp_address += CLIO_DSPP_IMEM_READ_BASE;
      CLIO.dsp_word1    = opera_dsp_imem_read(CLIO.dsp_address+0);
      CLIO.dsp_word2    = opera_dsp_imem_read(CLIO.dsp_address+1);
      return ((CLIO.dsp_word1 << CLIO_WORD_SHIFT) | CLIO.dsp_word2);
    }
  else if((addr_ >= CLIO_REG_DSPPEO16_FIRST) &&
          (addr_ <= CLIO_REG_DSPPEO16_LAST))
    {
      CLIO.dsp_address  = ((addr_ - CLIO_REG_DSPPEO16_FIRST) >> 2);
      CLIO.dsp_address &= CLIO_DSPP_IMEM_MASK;
      CLIO.dsp_address += CLIO_DSPP_IMEM_READ_BASE;
      return opera_dsp_imem_read(CLIO.dsp_address);
    }
  else if(addr_ == CLIO_REG_NOISE)
    return prng32();
  else if(addr_ == CLIO_REG_SEMAPHORE)
    return opera_dsp_arm_semaphore_read();
  else if(clio_timer_reg_addr(addr_))
    return (CLIO.regs[addr_] & CLIO_WORD_MASK);

  return CLIO.regs[addr_];
}

void
opera_clio_vcnt_update(int line_,
                       int field_)
{
  CLIO.regs[CLIO_REG_VCNT] = (((field_ << VCNT_FIELD_SHIFT) & VCNT_FIELD) |
                              ((uint32_t)line_ & VCNT_MASK));
}

static
uint32_t
timer_flags(const uint32_t timer_)
{
  return ((CLIO.regs[((timer_ < CLIO_TIMER_SET_COUNT) ?
                      CLIO_REG_SETTM0 : CLIO_REG_SETTM1)] >>
           timer_control_shift(timer_)) & CLIO_TIMER_MASK);
}

static
void
timer_disable(const uint32_t timer_)
{
  CLIO.regs[((timer_ < CLIO_TIMER_SET_COUNT) ?
             CLIO_REG_SETTM0 : CLIO_REG_SETTM1)] &=
    ~(TIMER_DECREMENT << timer_control_shift(timer_));
}

void
opera_clio_timer_execute(uint32_t timer_)
{
  uint32_t *reg;
  uint32_t  count;
  uint32_t  flags;
  uint32_t  underflow;

  timer_ &= CLIO_TIMER_MASK;
  if(timer_ == 0)
    TIMER_CARRY = 0;

  underflow = 0;
  flags = timer_flags(timer_);
  if(flags & TIMER_DECREMENT)
    {
      if((flags & TIMER_CASCADE) && !TIMER_CARRY)
        {
          TIMER_CARRY = 0;
          return;
        }

      reg   = &CLIO.regs[(CLIO_REG_TIMER0 + (timer_ * CLIO_TIMER_PAIR_STRIDE))];
      count = (reg[0] & CLIO_WORD_MASK);
      if(count == 0)
        {
          underflow = 1;
          if(timer_ & 1)
            opera_clio_fiq_generate(INT0_TIMINT1 >> (timer_ >> 1),0);
          if(flags & TIMER_RELOAD)
            reg[0] = (reg[CLIO_TIMER_REG_STRIDE] & CLIO_WORD_MASK);
          else
            {
              reg[0] = CLIO_WORD_MASK;
              timer_disable(timer_);
            }
        }
      else
        {
          reg[0] = (count - 1);
        }
    }

  TIMER_CARRY = underflow;
}

uint32_t
opera_clio_timer_get_delay(void)
{
  return CLIO.regs[CLIO_REG_SLACK];
}

void opera_clio_init(int reason_)
{
  memset(&CLIO,0,sizeof(CLIO));

  //CLIO.regs[CLIO_REG_VINT0]=240;

  CLIO.regs[CLIO_REG_CSTATBITS] = reason_;
  CLIO.regs[CLIO_REG_SETEXPCTL] = XB_CPUHASXBUS;
  CLIO.regs[CLIO_REG_SLACK] = CLIO_DEFAULT_TIMER_SLACK;
  opera_clock_timer_set_delay(CLIO.regs[CLIO_REG_SLACK]);
  MADAM_REGS = opera_madam_registers();
  TIMER_VAL  = 0;
  flagtime   = 0;
  TIMER_CARRY = 0;
}

void
opera_clio_reset(void)
{
  memset(&CLIO,0,sizeof(CLIO));
  TIMER_CARRY = 0;
}

uint16_t
opera_clio_fifo_ei_read(uint16_t channel_)
{
  return opera_mem_read16(((CLIO.fifo_i[channel_].start.addr + CLIO.fifo_i[channel_].idx)));
}

static
void
opera_clio_fifo_eo_write(uint16_t channel_,
                         uint16_t val_)
{
  opera_mem_write16(((CLIO.fifo_o[channel_].start.addr + CLIO.fifo_o[channel_].idx)),val_);
}

static
INLINE
bool
dma_channel_enabled(uint32_t const channel_)
{
  return (CLIO.regs[CLIO_REG_DMAREQEN] & (1 << channel_));
}

uint16_t
opera_clio_fifo_ei(uint16_t channel_)
{
  uint16_t val_;

  if(CLIO.fifo_i[channel_].start.addr == 0)
    return 0;

  if((CLIO.fifo_i[channel_].start.len - CLIO.fifo_i[channel_].idx) > 0)
    {
      val_ = opera_clio_fifo_ei_read(channel_);
      CLIO.fifo_i[channel_].idx += sizeof(uint16_t);
    }
  else
    {
      CLIO.fifo_i[channel_].idx = 0;
      opera_clio_fiq_generate(INT0_DRDINT0 << channel_,0);

      /* reload enabled see patent WO09410641A1, 49.16 */
      if((CLIO.fifo_i[channel_].next.addr != 0) && dma_channel_enabled(channel_))
        {
          CLIO.fifo_i[channel_].start.addr = CLIO.fifo_i[channel_].next.addr;
          CLIO.fifo_i[channel_].start.len  = CLIO.fifo_i[channel_].next.len;

          val_ = opera_clio_fifo_ei_read(channel_);
          CLIO.fifo_i[channel_].idx += sizeof(uint16_t);
        }
      else
        {
          CLIO.fifo_i[channel_].start.addr = 0;
          val_ = 0;
        }
    }

  return val_;


  // JMK SEZ: What is this? It was commented out along with this whole "else"
  //          block, but I had to bring this else block back from the dead
  //          in order to initialize val appropriately.

  // opera_clio_fiq_generate(1<<(channel_+16),0);
}

void
opera_clio_fifo_eo(uint16_t channel_,
                   uint16_t val_)
{
  if(CLIO.fifo_o[channel_].start.addr == 0)
    return;

  if((CLIO.fifo_o[channel_].start.len - CLIO.fifo_o[channel_].idx) > 0)
    {
      opera_clio_fifo_eo_write(channel_,val_);
      CLIO.fifo_o[channel_].idx += sizeof(uint16_t);
    }
  else
    {
      CLIO.fifo_o[channel_].idx = 0;
      opera_clio_fiq_generate(INT0_DDRINT0 << channel_,0);

      /* reload enabled? */
      if((CLIO.fifo_o[channel_].next.addr != 0) && dma_channel_enabled(channel_))
        {
          CLIO.fifo_o[channel_].start.addr = CLIO.fifo_o[channel_].next.addr;
          CLIO.fifo_o[channel_].start.len  = CLIO.fifo_o[channel_].next.len;
        }
      else
        {
          CLIO.fifo_o[channel_].start.addr = 0;
        }
    }
}

uint16_t
opera_clio_fifo_ei_status(uint8_t channel_)
{
  if(CLIO.fifo_i[channel_].start.addr != 0)
    return FIFO_Count1; /* fixme */

  return 0;
}

uint16_t
opera_clio_fifo_eo_status(uint8_t channel_)
{
  if(CLIO.fifo_o[channel_].start.addr != 0)
    return FIFO_Count0;
  return 0;
}

static
INLINE
uint32_t
clio_fifo_channel(uint32_t addr_)
{
  return ((addr_ >> CLIO_FIFO_CHANNEL_SHIFT) & CLIO_FIFO_CHANNEL_MASK);
}

uint32_t
opera_clio_fifo_read(uint32_t addr_)
{
  uint32_t channel;

  channel = clio_fifo_channel(addr_);

  if((addr_ & CLIO_FIFO_INPUT_MASK) == CLIO_FIFO_INPUT_VALUE)
    {
      switch(addr_ & CLIO_FIFO_ADDR_MASK)
        {
        case MADAM_DMA_CURADR_OFFSET:
          return CLIO.fifo_i[channel].start.addr + CLIO.fifo_i[channel].idx;
        case MADAM_DMA_CURLEN_OFFSET:
          return CLIO.fifo_i[channel].start.len - CLIO.fifo_i[channel].idx;
        case MADAM_DMA_RLDADR_OFFSET:
          return CLIO.fifo_i[channel].next.addr;
        case MADAM_DMA_RLDLEN_OFFSET:
          return CLIO.fifo_i[channel].next.len;
        }
    }

  switch(addr_ & CLIO_FIFO_ADDR_MASK)
    {
    case MADAM_DMA_CURADR_OFFSET:
      return CLIO.fifo_o[channel].start.addr + CLIO.fifo_o[channel].idx;
    case MADAM_DMA_CURLEN_OFFSET:
      return CLIO.fifo_o[channel].start.len - CLIO.fifo_o[channel].idx;
    case MADAM_DMA_RLDADR_OFFSET:
      return CLIO.fifo_o[channel].next.addr;
    case MADAM_DMA_RLDLEN_OFFSET:
      return CLIO.fifo_o[channel].next.len;
    }

  return 0;
}

void
opera_clio_fifo_write(uint32_t addr_, uint32_t val_)
{
  uint32_t channel;

  channel = clio_fifo_channel(addr_);

  if((addr_ & CLIO_FIFO_INPUT_MASK) == CLIO_FIFO_INPUT_VALUE)
    {
      switch(addr_ & CLIO_FIFO_ADDR_MASK)
        {
        case MADAM_DMA_CURADR_OFFSET:
          /* see patent WO09410641A1, 46.25 */
          CLIO.fifo_i[channel].start.addr = val_;
          CLIO.fifo_i[channel].next.addr = 0;
          break;
        case MADAM_DMA_CURLEN_OFFSET:
          if(val_ != 0)
            CLIO.fifo_i[channel].start.len = (val_ + CLIO_FIFO_LENGTH_BIAS);
          else
            CLIO.fifo_i[channel].start.len = 0;

          /* see patent WO09410641A1, 46.25 */
          CLIO.fifo_i[channel].next.len = 0;
          break;
        case MADAM_DMA_RLDADR_OFFSET:
          CLIO.fifo_i[channel].next.addr = val_;
          break;
        case MADAM_DMA_RLDLEN_OFFSET:
          if(val_ != 0)
            CLIO.fifo_i[channel].next.len = (val_ + CLIO_FIFO_LENGTH_BIAS);
          else
            CLIO.fifo_i[channel].next.len = 0;
          break;
        }
    }
  else
    {
      switch (addr_ & CLIO_FIFO_ADDR_MASK)
        {
        case MADAM_DMA_CURADR_OFFSET:
          CLIO.fifo_o[channel].start.addr = val_;
          break;
        case MADAM_DMA_CURLEN_OFFSET:
          CLIO.fifo_o[channel].start.len = (val_ + CLIO_FIFO_LENGTH_BIAS);
          break;
        case MADAM_DMA_RLDADR_OFFSET:
          CLIO.fifo_o[channel].next.addr = val_;
          break;
        case MADAM_DMA_RLDLEN_OFFSET:
          CLIO.fifo_o[channel].next.len = (val_ + CLIO_FIFO_LENGTH_BIAS);
          break;
        }
    }
}
