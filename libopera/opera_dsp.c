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

#include "boolean.h"
#include "inline.h"

#include "opera_clio.h"
#include "opera_core.h"
#include "opera_dsp.h"
#include "opera_state.h"
#include "prng16.h"

#include <string.h>

#if 0 //20 bit ALU
#define ALUSIZEMASK 0xFFFFF000
#else //32 bit ALU
#define ALUSIZEMASK 0xFFFFFFFF
#endif

#define TOPBIT 0x80000000

#define SYSTEM_CLOCK_RATE  25000000
#define DEFAULT_SAMPLERATE 44100
#define SYSTEM_TICKS       (((SYSTEM_CLOCK_RATE + DEFAULT_SAMPLERATE - 1) / \
                             DEFAULT_SAMPLERATE) + 1)

#define DSP_AUDIO_STATUS_AUDLOCK 0x8000
#define DSP_AUDIO_STATUS_LFTFULL 0x0002
#define DSP_AUDIO_STATUS_RGTFULL 0x0001
#define DSP_AUDIO_STATUS_MASK    (DSP_AUDIO_STATUS_AUDLOCK | \
                                  DSP_AUDIO_STATUS_LFTFULL | \
                                  DSP_AUDIO_STATUS_RGTFULL)

#define FIFO_Count1   0x00000002
#define INT0_DSPPINT  0x00000800

#define LAST_N_MEM     0x3FF
#define OFFSET_I_MEM   0x100

#define DSPP_SLEEP_OPCODE 0x8380

#define DSPP_AUDIO_INPUT_DMA_COUNT 13
#define DSPP_EI_FIFO_COUNT         15
#define NUM_AUDIO_OUTPUT_DMAS      4

#define DSPP_FIFOHEAD_ZERO_ADDRESS 0x070
#define DSPI_NOISE                 0x0EA

#define DSPP_AUDIO_STATUS_READ 0x0EB
#define DSPP_SEMA4_STATUS_READ 0x0EC
#define DSPP_SEMA4_DATA_READ   0x0ED
#define DSPP_PC_READ           0x0EE
#define DSPP_DSPPCNT_READ      0x0EF

#define DSPP_EI_FIFO_DATA_FIRST   0x0F0
#define DSPP_EI_FIFO_STATUS_FIRST 0x0D0
#define DSPP_EO_FIFO_STATUS_FIRST 0x0E0
#define DSPP_EO_FIFO_DATA_FIRST   0x3F0

#define DSPP_AUDIO_STATUS_WRITE 0x3EB
#define DSPP_SEMA4_ACK_WRITE    0x3EC
#define DSPP_SEMA4_DATA_WRITE   0x3ED
#define DSPP_INT_WRITE          0x3EE
#define DSPP_DSPPRLD_WRITE      0x3EF
#define DSPP_EO_FIFO_FLUSH      0x3FD
#define DSPP_DAC_LEFT           0x3FE
#define DSPP_DAC_RIGHT          0x3FF

#define DSPP_FIFO_CHANNEL_MASK           0x0F
#define DSPP_IMEM_LOW_MASK               0x7F
#define DSPP_IMEM_DIRECT_MASK            0x80
#define DSPP_I_MEM_MIRROR_SIZE           0x200

#define DSPP_SEMA4_STATUS_DSP_ACK   0x0001
#define DSPP_SEMA4_STATUS_CPU_ACK   0x0002
#define DSPP_SEMA4_STATUS_DSP_WROTE 0x0004
#define DSPP_SEMA4_STATUS_CPU_WROTE 0x0008
#define DSPP_SEMA4_DATA_MASK        0xFFFF
#define DSPP_SEMA4_STATUS_SHIFT     16

#pragma pack(push,1)

struct CIFTAG_s
{
  uint32_t BCH_ADDR:10;
  uint32_t FLAG_MASK:2;
  uint32_t FLGSEL:1;
  uint32_t MODE:2;
  uint32_t PAD:1;
};

typedef struct CIFTAG_s CIFTAG_t;

struct BRNTAG_s
{
  uint32_t BCH_ADDR:10;
  uint32_t FLAGM0:1;
  uint32_t FLAGM1:1;
  uint32_t FLAGSEL:1;
  uint32_t MODE0:1;
  uint32_t MODE1:1;
  uint32_t AC:1;
};

typedef struct BRNTAG_s BRNTAG_t;

struct BRNBITS_s
{
  uint32_t BCH_ADDR:10;
  uint32_t bits:5;
  uint32_t AC:1;
};

typedef struct BRNBITS_s BRNBITS_t;

struct AIFTAG_s
{
  uint32_t BS:4;
  uint32_t ALU:4;
  uint32_t MUXB:2;
  uint32_t MUXA:2;
  int32_t  M2SEL:1;
  uint32_t NUMOPS:2;
  int32_t  PAD:1;
};

typedef struct AIFTAG_s AIFTAG_t;

struct IOFTAG_s
{
  int32_t  IMMEDIATE:13;
  int32_t  JUSTIFY:1;
  uint32_t TYPE:2;
};

typedef struct IOFTAG_s IOFTAG_t;

struct NROFTAG_s
{
  uint32_t OP_ADDR:10;
  int32_t  DI:1;
  uint32_t WB1:1;
  uint32_t PAD:1;
  uint32_t TYPE:3;
};

typedef struct NROFTAG_s NROFTAG_t;

struct R2OFTAG_s
{
  uint32_t R1:4;
  int32_t  R1_DI:1;
  uint32_t R2:4;
  int32_t  R2_DI:1;
  uint32_t NUMREGS:1;
  uint32_t WB1:1;
  uint32_t WB2:1;
  uint32_t TYPE:3;
};

typedef struct R2OFTAG_s R2OFTAG_t;

struct R3OFTAG_s
{
  uint32_t R1:4;
  int32_t  R1_DI:1;
  uint32_t R2:4;
  int32_t  R2_DI:1;
  uint32_t R3:4;
  int32_t  R3_DI:1;
  uint32_t TYPE:1;
};

typedef struct R3OFTAG_s R3OFTAG_t;

union ITAG_u
{
  uint32_t  raw;
  AIFTAG_t  aif;
  CIFTAG_t  cif;
  IOFTAG_t  iof;
  NROFTAG_t nrof;
  R2OFTAG_t r2of;
  R3OFTAG_t r3of;
  BRNTAG_t  branch;
  BRNBITS_t br;
};

typedef union ITAG_u ITAG_t;

struct RQFTAG_s
{
  uint32_t BS:1;
  uint32_t ALU2:1;
  uint32_t ALU1:1;
  uint32_t MULT2:1;
  uint32_t MULT1:1;
};

typedef struct RQFTAG_s RQFTAG_t;

union REQ_u
{
  uint8_t  raw;
  RQFTAG_t rq;
};

typedef union REQ_u REQ_t;

/* only for ALU command */
struct INSTTRAS_s
{
  REQ_t req;
  char  BS;                     // 4+1 bits
};

typedef struct INSTTRAS_s INSTTRAS_t;

struct REGSTAG_s
{
  uint32_t PC;                  // 0x0ee
  //  uint16_t NOISE;               // 0x0ea
  uint16_t AudioOutStatus;      // audlock,lftfull,rgtfull -- 0x0eb//0x3eb
  uint16_t Sema4Status;         // 0x0ec // 0x3ec
  uint16_t Sema4Data;           // 0x0ed // 0x3ed
  int16_t  DSPPCNT;             // 0x0ef
  int16_t  DSPPRLD;             // 0x3ef
  int16_t  AUDCNT;
  uint16_t INT;                 // 0x3ee
};

typedef struct REGSTAG_s REGSTAG_t;

struct INTAG_s
{
  int16_t  MULT1;
  int16_t  MULT2;
  int16_t  ALU1;
  int16_t  ALU2;
  int32_t  BS;
  uint16_t RMAP;
  uint16_t nOP_MASK;
  uint16_t WRITEBACK;
  REQ_t    req;
  bool     Running;
  bool     GenFIQ;
};

typedef struct INTAG_s INTAG_t;

enum dsp_write_kind_e
{
  DSP_WRITE_DIRECT,
  DSP_WRITE_INDIRECT,
  DSP_WRITE_REGISTER_DIRECT,
  DSP_WRITE_REGISTER_INDIRECT,
  DSP_WRITE_WRITEBACK
};

typedef enum dsp_write_kind_e dsp_write_kind_t;

enum dsp_branch_delay_kind_e
{
  DSP_BRANCH_DELAY_NONE,
  DSP_BRANCH_DELAY_AFTER_BRANCH,
  DSP_BRANCH_DELAY_AFTER_BFM_TARGET
};

typedef enum dsp_branch_delay_kind_e dsp_branch_delay_kind_t;

struct dsp_branch_delay_s
{
  dsp_branch_delay_kind_t kind;
  uint16_t                branch_target;
  uint16_t                bfm_target;
};

typedef struct dsp_branch_delay_s dsp_branch_delay_t;

struct dsp_s
{
  uint32_t   RBASEx4;
  INSTTRAS_t INSTTRAS[0x8000];
  uint16_t   REGCONV[8][16];
  int        BRCONDTAB[32][32];
  uint16_t   NMem[2048];
  uint16_t   IMem[1024];
  int        REGi;
  REGSTAG_t  dregs;
  INTAG_t    flags;
  int        CPUSupply[16];
};

typedef struct dsp_s dsp_t;

union dsp_alu_flags_u
{
  uint32_t raw;

  struct
  {
    uint8_t zero;
    uint8_t negative;
    uint8_t carry;
    uint8_t overflow;
  };
};

typedef union dsp_alu_flags_u dsp_alu_flags_t;

#pragma pack(pop)

static dsp_t DSP;

static
INLINE
uint16_t
dsp_audio_status(void)
{
  return (DSP.dregs.AudioOutStatus & DSP_AUDIO_STATUS_MASK);
}

static
INLINE
bool
dsp_addr_in_range(uint32_t addr_,
                  uint32_t first_,
                  uint32_t count_)
{
  return ((addr_ - first_) < count_);
}

static
INLINE
uint32_t
hash16(uint32_t i_,
       uint32_t k_)
{
  uint32_t const hash = (i_ * k_);

  return (((hash >> 16) ^ hash) & 0xFFFF);
}

static
INLINE
bool
dsp_control_is_bfm(ITAG_t inst_)
{
  uint32_t control;

  control = ((inst_.raw >> 7) & 0xFF);

  return (inst_.aif.PAD && (control >= 24) && (control <= 31));
}

static
INLINE
bool
dsp_control_executes_in_branch_delay(ITAG_t inst_)
{
  if(!inst_.aif.PAD)
    return false;

  switch((inst_.raw >> 7) & 0xFF)
    {
    case 0: /* NOP */
    case 2: /* RBASE */
    case 3: /* RMAP */
    case 5: /* operand mask */
      return true;
    default:
      break;
    }

  return false;
}

static
INLINE
void
dsp_branch_delay_take(dsp_branch_delay_t *delay_,
                      uint16_t target_)
{
  delay_->kind          = DSP_BRANCH_DELAY_AFTER_BRANCH;
  delay_->branch_target = (target_ & LAST_N_MEM);
}

static
OPERA_FORCEINLINE
int
ADD_CFLAG(const uint32_t a_,
          const uint32_t b_,
          const uint32_t y_)
{
  return ((a_ &  b_ & TOPBIT) ||
          (a_ & ~y_ & TOPBIT) ||
          (b_ & ~y_ & TOPBIT));
}

static
OPERA_FORCEINLINE
int
SUB_CFLAG(const uint32_t a_,
          const uint32_t b_,
          const uint32_t y_)
{
  return (( a_ & ~b_ & TOPBIT) ||
          ( a_ & ~y_ & TOPBIT) ||
          (~b_ & ~y_ & TOPBIT));
}

static
OPERA_FORCEINLINE
int
ADD_VFLAG(const uint32_t a_,
          const uint32_t b_,
          const uint32_t y_)
{
  return (( a_ &  b_ & ~y_ & TOPBIT) ||
          (~a_ & ~b_ &  y_ & TOPBIT));
}

static
OPERA_FORCEINLINE
int
SUB_VFLAG(const uint32_t a_,
          const uint32_t b_,
          const uint32_t y_)
{
  return (( a_ & ~b_ & ~y_ & TOPBIT) ||
          (~a_ &  b_ &  y_ & TOPBIT));
}

/* DSP IREAD (includes EI, I) */
static
uint16_t
dsp_read(uint32_t addr_)
{
  /* addr &= LAST_N_MEM; */
  if(dsp_addr_in_range(addr_,DSPP_EI_FIFO_DATA_FIRST,DSPP_EI_FIFO_COUNT))
    {
      uint32_t channel;

      channel = (addr_ & DSPP_FIFO_CHANNEL_MASK);
      /*
        printf("#DSP read from CPU!!! chan=0x%x\n",addr&0x0f);
        val=IMem[addr-0x80];
      */
      if(DSP.CPUSupply[channel])
        return (DSP.CPUSupply[channel] = 0,
                DSP.IMem[DSPP_FIFOHEAD_ZERO_ADDRESS + channel]);
      if(channel >= DSPP_AUDIO_INPUT_DMA_COUNT)
        return DSP.IMem[DSPP_FIFOHEAD_ZERO_ADDRESS + channel];
      return opera_clio_fifo_ei(channel);
    }

  if(dsp_addr_in_range(addr_,DSPP_FIFOHEAD_ZERO_ADDRESS,DSPP_EI_FIFO_COUNT))
    {
      uint32_t channel;

      channel = (addr_ & DSPP_FIFO_CHANNEL_MASK);
      //printf("#DSP read from CPU!!! chan=0x%x\n",addr&0x0f);
      if(DSP.CPUSupply[channel])
        return (DSP.CPUSupply[channel] = 0,
                DSP.IMem[addr_]);
      if(channel >= DSPP_AUDIO_INPUT_DMA_COUNT)
        return DSP.IMem[addr_];
      return opera_clio_fifo_ei_read(channel);
    }

  if(dsp_addr_in_range(addr_,DSPP_EI_FIFO_STATUS_FIRST,DSPP_EI_FIFO_COUNT))
    {
      uint32_t channel;

      channel = (addr_ & DSPP_FIFO_CHANNEL_MASK);
      if(DSP.CPUSupply[channel])
        return FIFO_Count1;
      if(channel >= DSPP_AUDIO_INPUT_DMA_COUNT)
        return 0;
      return opera_clio_fifo_ei_status(channel);
    }

  if(dsp_addr_in_range(addr_,DSPP_EO_FIFO_STATUS_FIRST,NUM_AUDIO_OUTPUT_DMAS))
    return opera_clio_fifo_eo_status(addr_ & DSPP_FIFO_CHANNEL_MASK);

  switch(addr_)
    {
    case DSPI_NOISE:
      return prng16();
    case DSPP_AUDIO_STATUS_READ:
      return dsp_audio_status();
    case DSPP_SEMA4_STATUS_READ:
      return DSP.dregs.Sema4Status;
    case DSPP_SEMA4_DATA_READ:
      return DSP.dregs.Sema4Data;
    case DSPP_PC_READ:
      return DSP.dregs.PC;
    case DSPP_DSPPCNT_READ:
      return DSP.dregs.DSPPCNT;
    default:
      //printf("#EIRead 0x%3.3X>=0x%4.4X\n",addr, IMem[addr_ & DSPP_IMEM_LOW_MASK]);
      addr_ -= OFFSET_I_MEM;
      if(addr_ < DSPP_I_MEM_MIRROR_SIZE)
        return DSP.IMem[addr_ | OFFSET_I_MEM];
    }

  return DSP.IMem[addr_ & DSPP_IMEM_LOW_MASK];
}

/* DSP IWRITE (includes EO,I) */
static
void
dsp_write(uint32_t addr_,
          uint16_t val_,
          dsp_write_kind_t kind_)
{
  addr_ &= LAST_N_MEM;

  if(dsp_addr_in_range(addr_,DSPP_EO_FIFO_DATA_FIRST,NUM_AUDIO_OUTPUT_DMAS))
    {
      opera_clio_fifo_eo(addr_ & DSPP_FIFO_CHANNEL_MASK,val_);
      return;
    }

  switch(addr_)
    {
    case DSPP_AUDIO_STATUS_WRITE:
      DSP.dregs.AudioOutStatus =
        ((DSP.dregs.AudioOutStatus & ~DSP_AUDIO_STATUS_AUDLOCK) |
         (val_ & DSP_AUDIO_STATUS_AUDLOCK));
      break;
    case DSPP_SEMA4_ACK_WRITE:
      /* DSP write to Sema4ACK. */
      DSP.dregs.Sema4Status |= DSPP_SEMA4_STATUS_DSP_ACK;
      break;
    case DSPP_SEMA4_DATA_WRITE:
      DSP.dregs.Sema4Data   = val_;
      DSP.dregs.Sema4Status = DSPP_SEMA4_STATUS_DSP_WROTE;
      break;
    case DSPP_INT_WRITE:
      DSP.dregs.INT    = val_;
      DSP.flags.GenFIQ = true;
      break;
    case DSPP_DSPPRLD_WRITE:
      if(kind_ == DSP_WRITE_DIRECT)
        DSP.dregs.DSPPRLD = val_;
      break;
    case DSPP_EO_FIFO_FLUSH:
      /* FLUSH EOFIFO */
      opera_clio_fifo_eo_flush(val_ & DSPP_FIFO_CHANNEL_MASK);
      break;
    case DSPP_DAC_LEFT: /* DAC Left channel */
      DSP.IMem[addr_] = val_;
      DSP.dregs.AudioOutStatus |= DSP_AUDIO_STATUS_LFTFULL;
      break;
    case DSPP_DAC_RIGHT: /* DAC Right channel */
      DSP.IMem[addr_] = val_;
      DSP.dregs.AudioOutStatus |= DSP_AUDIO_STATUS_RGTFULL;
      break;
    default:
      if(addr_ < OFFSET_I_MEM)
        return;

      addr_ -= OFFSET_I_MEM;
      if(addr_ < DSPP_I_MEM_MIRROR_SIZE)
        DSP.IMem[addr_ | OFFSET_I_MEM] = val_;
      else
        DSP.IMem[addr_ + OFFSET_I_MEM] = val_;
      break;
    }
}

uint32_t
opera_dsp_state_size_v1(void)
{
  return opera_state_save_size(sizeof(DSP));
}

static
bool
dsp_state_write_payload(opera_state_writer_t *writer_,
                        dsp_t const          *state_)
{
  uint32_t i;
  uint32_t j;

  if(!opera_state_write_u32(writer_,state_->RBASEx4))
    return false;

  for(i = 0; i < 0x8000; i++)
    if(!opera_state_write_u8(writer_,state_->INSTTRAS[i].req.raw) ||
       !opera_state_write_i8(writer_,(int8_t)state_->INSTTRAS[i].BS))
      return false;

  if(!opera_state_write_u16_array(writer_,&state_->REGCONV[0][0],8 * 16))
    return false;

  for(i = 0; i < 32; i++)
    for(j = 0; j < 32; j++)
      if(!opera_state_write_i32(writer_,state_->BRCONDTAB[i][j]))
        return false;

  if(!opera_state_write_u16_array(writer_,state_->NMem,2048) ||
     !opera_state_write_u16_array(writer_,state_->IMem,1024) ||
     !opera_state_write_i32(writer_,state_->REGi) ||
     !opera_state_write_u32(writer_,state_->dregs.PC) ||
     !opera_state_write_u16(writer_,state_->dregs.AudioOutStatus) ||
     !opera_state_write_u16(writer_,state_->dregs.Sema4Status) ||
     !opera_state_write_u16(writer_,state_->dregs.Sema4Data) ||
     !opera_state_write_i16(writer_,state_->dregs.DSPPCNT) ||
     !opera_state_write_i16(writer_,state_->dregs.DSPPRLD) ||
     !opera_state_write_i16(writer_,state_->dregs.AUDCNT) ||
     !opera_state_write_u16(writer_,state_->dregs.INT) ||
     !opera_state_write_i16(writer_,state_->flags.MULT1) ||
     !opera_state_write_i16(writer_,state_->flags.MULT2) ||
     !opera_state_write_i16(writer_,state_->flags.ALU1) ||
     !opera_state_write_i16(writer_,state_->flags.ALU2) ||
     !opera_state_write_i32(writer_,state_->flags.BS) ||
     !opera_state_write_u16(writer_,state_->flags.RMAP) ||
     !opera_state_write_u16(writer_,state_->flags.nOP_MASK) ||
     !opera_state_write_u16(writer_,state_->flags.WRITEBACK) ||
     !opera_state_write_u8(writer_,state_->flags.req.raw) ||
     !opera_state_write_u8(writer_,state_->flags.Running ? 1 : 0) ||
     !opera_state_write_u8(writer_,state_->flags.GenFIQ ? 1 : 0))
    return false;

  for(i = 0; i < 16; i++)
    if(!opera_state_write_i32(writer_,state_->CPUSupply[i]))
      return false;

  return true;
}

static
uint32_t
dsp_state_payload_size(void)
{
  opera_state_writer_t writer;

  opera_state_writer_init(&writer,NULL,UINT32_MAX);
  dsp_state_write_payload(&writer,&DSP);

  return opera_state_writer_used(&writer);
}

uint32_t
opera_dsp_state_size(void)
{
  return opera_state_chunk_size(dsp_state_payload_size());
}

uint32_t
opera_dsp_state_save(void *buf_)
{
  uint32_t payload_size;
  opera_state_writer_t writer;

  payload_size = dsp_state_payload_size();
  opera_state_writer_init(&writer,buf_,opera_state_chunk_size(payload_size));
  opera_state_write_chunk_header(&writer,"DSPP",payload_size);
  dsp_state_write_payload(&writer,&DSP);

  return opera_state_writer_ok(&writer) ? opera_state_writer_used(&writer) : 0;
}

uint32_t
opera_dsp_state_load_v1(const void     *buf_,
                        uint32_t const  size_)
{
  uint32_t rv;

  rv = opera_state_load_sized(&DSP,"DSPP",buf_,size_,sizeof(DSP));
  if(rv != 0)
    DSP.dregs.AudioOutStatus &= DSP_AUDIO_STATUS_MASK;

  return rv;
}

static
bool
dsp_state_read_payload(opera_state_reader_t *reader_,
                       dsp_t                *state_)
{
  uint32_t i;
  uint32_t j;
  int8_t bs;
  uint8_t b;

  memset(state_,0,sizeof(*state_));

  if(!opera_state_read_u32(reader_,&state_->RBASEx4))
    return false;

  for(i = 0; i < 0x8000; i++)
    {
      if(!opera_state_read_u8(reader_,&state_->INSTTRAS[i].req.raw) ||
         !opera_state_read_i8(reader_,&bs))
        return false;
      state_->INSTTRAS[i].BS = (char)bs;
    }

  if(!opera_state_read_u16_array(reader_,&state_->REGCONV[0][0],8 * 16))
    return false;

  for(i = 0; i < 32; i++)
    for(j = 0; j < 32; j++)
      {
        int32_t v;
        if(!opera_state_read_i32(reader_,&v))
          return false;
        state_->BRCONDTAB[i][j] = v;
      }

  if(!opera_state_read_u16_array(reader_,state_->NMem,2048) ||
     !opera_state_read_u16_array(reader_,state_->IMem,1024) ||
     !opera_state_read_i32(reader_,&state_->REGi) ||
     !opera_state_read_u32(reader_,&state_->dregs.PC) ||
     !opera_state_read_u16(reader_,&state_->dregs.AudioOutStatus) ||
     !opera_state_read_u16(reader_,&state_->dregs.Sema4Status) ||
     !opera_state_read_u16(reader_,&state_->dregs.Sema4Data) ||
     !opera_state_read_i16(reader_,&state_->dregs.DSPPCNT) ||
     !opera_state_read_i16(reader_,&state_->dregs.DSPPRLD) ||
     !opera_state_read_i16(reader_,&state_->dregs.AUDCNT) ||
     !opera_state_read_u16(reader_,&state_->dregs.INT) ||
     !opera_state_read_i16(reader_,&state_->flags.MULT1) ||
     !opera_state_read_i16(reader_,&state_->flags.MULT2) ||
     !opera_state_read_i16(reader_,&state_->flags.ALU1) ||
     !opera_state_read_i16(reader_,&state_->flags.ALU2) ||
     !opera_state_read_i32(reader_,&state_->flags.BS) ||
     !opera_state_read_u16(reader_,&state_->flags.RMAP) ||
     !opera_state_read_u16(reader_,&state_->flags.nOP_MASK) ||
     !opera_state_read_u16(reader_,&state_->flags.WRITEBACK) ||
     !opera_state_read_u8(reader_,&state_->flags.req.raw) ||
     !opera_state_read_u8(reader_,&b))
    return false;
  state_->flags.Running = (b != 0);
  if(!opera_state_read_u8(reader_,&b))
    return false;
  state_->flags.GenFIQ = (b != 0);

  for(i = 0; i < 16; i++)
    {
      int32_t v;
      if(!opera_state_read_i32(reader_,&v))
        return false;
      state_->CPUSupply[i] = v;
    }

  return true;
}

uint32_t
opera_dsp_state_load(const void     *buf_,
                     uint32_t const  size_)
{
  dsp_t state;
  opera_state_reader_t reader;
  opera_state_reader_t payload;

  opera_state_reader_init(&reader,buf_,size_);
  if(!opera_state_read_chunk(&reader,"DSPP",&payload) ||
     !dsp_state_read_payload(&payload,&state) ||
     !opera_state_reader_finished(&payload))
    return 0;

  DSP = state;
  DSP.dregs.AudioOutStatus &= DSP_AUDIO_STATUS_MASK;

  return opera_state_reader_used(&reader);
}

static
uint16_t
dsp_operand_load1(void)
{
  uint16_t op;
  ITAG_t operand;

  operand.raw = DSP.NMem[DSP.dregs.PC++];
  switch(operand.nrof.TYPE)
    {
    case 0:
    case 1:
    case 2:
    case 3:
      op = dsp_read(DSP.REGCONV[DSP.REGi][operand.r3of.R3] ^ DSP.RBASEx4);
      if(operand.r3of.R3_DI) /* ??? */
        return dsp_read(op);
      return op;
    case 4:
      // non reg format
      // IT'S an address!!!
      op = dsp_read(operand.nrof.OP_ADDR);
      if(operand.nrof.DI)
        return dsp_read(op);
      return op;
    case 5:
      // if(operand.r2of.NUMREGS) ignore... It's right?
      op = dsp_read(DSP.REGCONV[DSP.REGi][operand.r2of.R1] ^ DSP.RBASEx4);
      if(operand.r2of.R1_DI)
        return dsp_read(op);
      return op;
    case 6:
    case 7:
      return (operand.iof.IMMEDIATE << (operand.iof.JUSTIFY & 3));
    default:
      break;
    }

  return 0;
}

static
void
dsp_operand_load(int requests_)
{
  int idx;
  int op_cnt;
  uint16_t ops[6];
  uint16_t GWRITEBACK;
  ITAG_t operand;

  DSP.flags.WRITEBACK = 0;

  if(requests_ == 0)
    {
      if(DSP.flags.req.raw)
        requests_ = 4;
      else
        return;
    }

  op_cnt     = 0;
  GWRITEBACK = 0;

  do
    {
      operand.raw = DSP.NMem[DSP.dregs.PC++];
      switch(operand.nrof.TYPE)
        {
        case 0:
        case 1:
        case 2:
        case 3:
          ops[op_cnt] = dsp_read(DSP.REGCONV[DSP.REGi][operand.r3of.R3] ^ DSP.RBASEx4);
          if(operand.r3of.R3_DI)
            ops[op_cnt] = dsp_read(ops[op_cnt]);
          op_cnt++;

          ops[op_cnt] = dsp_read(DSP.REGCONV[DSP.REGi][operand.r3of.R2] ^ DSP.RBASEx4);
          if(operand.r3of.R2_DI)
            ops[op_cnt] = dsp_read(ops[op_cnt]);
          op_cnt++;

          /* only R1 can be WRITEBACK */
          DSP.flags.WRITEBACK = (DSP.REGCONV[DSP.REGi][operand.r3of.R1] ^ DSP.RBASEx4);
          ops[op_cnt] = dsp_read(DSP.flags.WRITEBACK);
          if(operand.r3of.R1_DI)
            ops[op_cnt] = dsp_read(ops[op_cnt]);
          op_cnt++;
          break;
        case 4:
          //non reg format ///IT'S an address!!!
          DSP.flags.WRITEBACK = operand.nrof.OP_ADDR;
          ops[op_cnt] = dsp_read(DSP.flags.WRITEBACK);
          if(operand.nrof.DI)
            ops[op_cnt] = dsp_read(ops[op_cnt]);
          op_cnt++;

          if(operand.nrof.WB1)
            GWRITEBACK = DSP.flags.WRITEBACK;
          break;
        case 5:
          //regged 1/2 format
          if(operand.r2of.NUMREGS)
            {
              DSP.flags.WRITEBACK = DSP.REGCONV[DSP.REGi][operand.r2of.R2] ^ DSP.RBASEx4;
              if(operand.r2of.R2_DI)
                DSP.flags.WRITEBACK = dsp_read(DSP.flags.WRITEBACK);
              ops[op_cnt] = dsp_read(DSP.flags.WRITEBACK);
              op_cnt++;

              if(operand.r2of.WB2)
                GWRITEBACK = DSP.flags.WRITEBACK;
            }

          DSP.flags.WRITEBACK = DSP.REGCONV[DSP.REGi][operand.r2of.R1] ^ DSP.RBASEx4;
          if(operand.r2of.R1_DI)
            DSP.flags.WRITEBACK = dsp_read(DSP.flags.WRITEBACK);
          ops[op_cnt] = dsp_read(DSP.flags.WRITEBACK);
          op_cnt++;

          if(operand.r2of.WB1)
            GWRITEBACK = DSP.flags.WRITEBACK;
          break;
        case 6:
        case 7:
          ops[op_cnt] = (operand.iof.IMMEDIATE << (operand.iof.JUSTIFY & 3));
          DSP.flags.WRITEBACK = ops[op_cnt++];
          break;
        default:
          break;
        }
    } while(op_cnt < requests_);


  /* ok let's clean out requests_ (using op_mask) */
  DSP.flags.req.raw &= DSP.flags.nOP_MASK;

  idx = 0;
  if(DSP.flags.req.rq.MULT1)
    DSP.flags.MULT1 = ops[idx++];
  if(DSP.flags.req.rq.MULT2)
    DSP.flags.MULT2 = ops[idx++];

  if(DSP.flags.req.rq.ALU1)
    DSP.flags.ALU1 = ops[idx++];
  if(DSP.flags.req.rq.ALU2)
    DSP.flags.ALU2 = ops[idx++];

  if(DSP.flags.req.rq.BS)
    DSP.flags.BS = (ops[idx++] & 0x1F);

  if(op_cnt != idx)
    {
      if(GWRITEBACK)
        DSP.flags.WRITEBACK = GWRITEBACK;
      /* else
         DSP.flags.WRITEBACK = 0; */
    }
  else
    {
      DSP.flags.WRITEBACK = GWRITEBACK;
    }
}

static
INLINE
uint16_t
dsp_register_base(uint32_t reg_)
{
  uint8_t x;
  uint8_t y;
  uint8_t twi;

  reg_ &= 0x0000000F;
  x     = ((reg_ >> 2) & 1);
  y     = ((reg_ >> 3) & 1);

  switch(DSP.flags.RMAP)
    {
    case 0:
    case 1:
    case 2:
    case 3:
      twi = x;
      break;
    case 4:
      twi = y;
      break;
    case 5:
      twi = !y;
      break;
    case 6:
      twi = x & y;
      break;
    case 7:
      twi = x | y;
      break;
    }

  return ((reg_ & 7) | (twi << 8) | (reg_ >> 3) << 9);
}

void
opera_dsp_init(void)
{
  uint32_t i;
  int32_t a,c;
  ITAG_t inst;

  memset(&DSP,0,sizeof(DSP));

  for(a = 0; a < 16; a++)
    {
      for(c = 0; c < 8; c++)
        {
          DSP.flags.RMAP    = c;
          DSP.REGCONV[c][a] = dsp_register_base(a);
        }
    }

  for(inst.raw = 0; inst.raw < 0x8000; inst.raw++)
    {
      DSP.flags.req.raw = 0;

      if(inst.aif.BS == 0x8)
        DSP.flags.req.rq.BS = 1;

      switch(inst.aif.MUXA)
        {
        case 3:
          DSP.flags.req.rq.MULT1 = 1;
          DSP.flags.req.rq.MULT2 = inst.aif.M2SEL;
          break;
        case 1:
          DSP.flags.req.rq.ALU1 = 1;
          break;
        case 2:
          DSP.flags.req.rq.ALU2 = 1;
          break;
        }

      switch(inst.aif.MUXB)
        {
        case 1:
          DSP.flags.req.rq.ALU1 = 1;
          break;
        case 2:
          DSP.flags.req.rq.ALU2 = 1;
          break;
        case 3:
          DSP.flags.req.rq.MULT1 = 1;
          DSP.flags.req.rq.MULT2 = inst.aif.M2SEL;
          break;
        }

      DSP.INSTTRAS[inst.raw].req.raw = DSP.flags.req.raw;
      DSP.INSTTRAS[inst.raw].BS      = (inst.aif.BS | ((inst.aif.ALU & 8) << (4 - 3)));
    }

  {
    int MD1, MD2, MD3;
    int STAT0, STAT1;
    int NSTAT0, NSTAT1;
    int TDCARE0, TDCARE1;
    int RDCARE;
    int MD12S;
    int SUPER0, SUPER1;
    int ALLZERO, NALLZERO;
    int SDS;
    int NVTest;
    int TMPCS;
    int CZTest, XactTest;
    int MD3S;
    int fExact;

    dsp_alu_flags_t flags;

    for(inst.raw = 0xA000; inst.raw <= 0xFFFF; inst.raw += 1024)
      for(flags.zero = 0; flags.zero < 2; flags.zero++)
        for(flags.negative = 0; flags.negative < 2; flags.negative++)
          for(flags.carry = 0; flags.carry < 2; flags.carry++)
            for(flags.overflow = 0; flags.overflow < 2; flags.overflow++)
              for(fExact = 0; fExact < 2; fExact++)
                {
                  MD1 = !inst.branch.MODE1 && inst.branch.MODE0;
                  MD2 = inst.branch.MODE1 && !inst.branch.MODE0;
                  MD3 = inst.branch.MODE1 && inst.branch.MODE0;

                  STAT0  = (inst.branch.FLAGSEL && flags.carry) || (!inst.branch.FLAGSEL && flags.negative);
                  STAT1  = (inst.branch.FLAGSEL && flags.zero) || (!inst.branch.FLAGSEL && flags.overflow);
                  NSTAT0 = STAT0 != MD2;
                  NSTAT1 = STAT1 != MD2;

                  TDCARE1 = !inst.branch.FLAGM1 || (inst.branch.FLAGM1 && NSTAT0);
                  TDCARE0 = !inst.branch.FLAGM0 || (inst.branch.FLAGM0 && NSTAT1);

                  RDCARE = !inst.branch.FLAGM1 && !inst.branch.FLAGM0;

                  MD12S = TDCARE1 && TDCARE0 && (inst.branch.MODE1!=inst.branch.MODE0) && !RDCARE;

                  SUPER0 = MD1 && !inst.branch.FLAGSEL && RDCARE;
                  SUPER1 = MD1 && inst.branch.FLAGSEL &&  RDCARE;

                  ALLZERO  = SUPER0 && flags.zero && fExact;
                  NALLZERO = SUPER1 && !(flags.zero && fExact);

                  SDS = ALLZERO || NALLZERO;

                  NVTest   = ((((flags.negative != flags.overflow) ||
                                (flags.zero && inst.branch.FLAGM0)) != inst.branch.FLAGM1) &&
                              !inst.branch.FLAGSEL);
                  TMPCS    = flags.carry && !flags.zero;
                  CZTest   = (TMPCS != inst.branch.FLAGM0) && inst.branch.FLAGSEL && !inst.branch.FLAGM1;
                  XactTest = (fExact != inst.branch.FLAGM0) && inst.branch.FLAGSEL && inst.branch.FLAGM1;

                  MD3S = ((NVTest || CZTest || XactTest) && MD3);

                  DSP.BRCONDTAB[inst.br.bits][fExact+((flags.raw*0x10080402)>>24)] = (MD12S || MD3S || SDS);
                }
  }

  DSP.flags.Running = false;
  DSP.flags.GenFIQ  = false;
  DSP.dregs.DSPPRLD = SYSTEM_TICKS;
  DSP.dregs.AUDCNT  = SYSTEM_TICKS;

  opera_dsp_reset();

  /*
   * CPU reads pack this status into the upper halfword. Portfolio OS checks
   * SEMAPHORE_DSPP_WROTE as 0x40000, matching DSP-wrote status bit 2.
   */
  DSP.dregs.Sema4Status = 0;

  for(i = 0; i < sizeof(DSP.NMem)/sizeof(DSP.NMem[0]); i++)
    DSP.NMem[i] = DSPP_SLEEP_OPCODE; /* sleep */

  for(i = 0; i < 16; i++)
    DSP.CPUSupply[i] = 0;
}

void
opera_dsp_reset(void)
{
  DSP.dregs.DSPPCNT  = DSP.dregs.DSPPRLD;
  DSP.dregs.PC       = 0;
  DSP.RBASEx4        = 0;
  DSP.REGi           = 0;
  DSP.flags.nOP_MASK = ~0;
}

uint32_t
opera_dsp_loop(void)
{
  uint32_t Y;	/* accumulator */
  uint32_t BOP; /* 1st & 2nd operand */
  dsp_alu_flags_t flags;

  DSP.dregs.AudioOutStatus &= DSP_AUDIO_STATUS_AUDLOCK;

  if(DSP.flags.Running)
    {
      uint32_t AOP    = 0;      /* 1st operand */
      uint32_t RBSR   = 0;	/* return address */
      int      fExact = 0;
      bool     work   = true;
      dsp_branch_delay_t branch_delay = { DSP_BRANCH_DELAY_NONE, 0, 0 };

      opera_dsp_reset();

      Y   = 0;
      BOP = 0;
      flags.raw = 0;

      do
        {
          ITAG_t inst;
          bool   redirect_after_slot;
          bool   bfm_target_slot;
          uint16_t redirect_target;
          uint16_t bfm_target;

          redirect_after_slot = false;
          redirect_target     = 0;
          bfm_target_slot = (branch_delay.kind == DSP_BRANCH_DELAY_AFTER_BFM_TARGET);
          bfm_target      = branch_delay.bfm_target;
          if(bfm_target_slot)
            branch_delay.kind = DSP_BRANCH_DELAY_NONE;

          inst.raw = DSP.NMem[DSP.dregs.PC++];
          if(branch_delay.kind == DSP_BRANCH_DELAY_AFTER_BRANCH)
            {
              uint16_t branch_target;

              branch_target = branch_delay.branch_target;
              branch_delay.kind = DSP_BRANCH_DELAY_NONE;

              if(dsp_control_is_bfm(inst))
                {
                  branch_delay.kind       = DSP_BRANCH_DELAY_AFTER_BFM_TARGET;
                  branch_delay.bfm_target = (inst.cif.BCH_ADDR & LAST_N_MEM);
                  DSP.dregs.PC            = branch_target;
                  continue;
                }
              else if(dsp_control_executes_in_branch_delay(inst))
                {
                  redirect_after_slot = true;
                  redirect_target     = branch_target;
                }
              else
                {
                  DSP.dregs.PC = branch_target;
                  continue;
                }
            }

          if(bfm_target_slot)
            {
              if(!dsp_control_executes_in_branch_delay(inst))
                {
                  DSP.dregs.PC = bfm_target;
                  continue;
                }

              redirect_after_slot = true;
              redirect_target     = bfm_target;
            }

          if(inst.aif.PAD)
            { // Control instruction
              switch((inst.raw >> 7) & 0xFF)
                {
                case 0:         /* NOP TODO */
                  break;
                case 1:         /* branch accum */
                  dsp_branch_delay_take(&branch_delay,(Y >> 16));
                  break;
                case 2:         /* set rbase */
                  DSP.RBASEx4 = ((inst.cif.BCH_ADDR & 0x3F) << 2);
                  break;
                case 3:         /* set rmap */
                  DSP.REGi = (inst.cif.BCH_ADDR & 7);
                  break;
                case 4:         /* RTS */
                  dsp_branch_delay_take(&branch_delay,RBSR);
                  break;
                case 5:         /* set op_mask */
                  DSP.flags.nOP_MASK = ~(inst.cif.BCH_ADDR & 0x1F);
                  break;
                case 6:         /* -not used2- ins */
                  break;
                case 7:         /* sleep */
                  work = false;
                  break;
                case 8:
                case 9:
                case 10:
                case 11:
                case 12:
                case 13:
                case 14:
                case 15:
                  /* jump */
                  dsp_branch_delay_take(&branch_delay,inst.cif.BCH_ADDR);
                  break;
                case 16:
                case 17:
                case 18:
                case 19:
                case 20:
                case 21:
                case 22:
                case 23:
                  /* jsr */
                  RBSR = DSP.dregs.PC;
                  dsp_branch_delay_take(&branch_delay,inst.cif.BCH_ADDR);
                  break;
                case 24:
                case 25:
                case 26:
                case 27:
                case 28:
                case 29:
                case 30:
                case 31:
                  /* Branch-From only redirects from a pending branch slot. */
                  break;
                case 32:
                case 33:
                case 34:
                case 35:
                case 36:
                case 37:
                case 38:
                case 39:
                case 40:
                case 41:
                case 42:
                case 43:
                case 44:
                case 45:
                case 46:
                case 47: /* MOVEREG */
                  {
                    uint16_t op;
                    uint16_t addr;
                    dsp_write_kind_t kind;

                    op   = dsp_operand_load1();
                    addr = DSP.REGCONV[DSP.REGi][inst.r2of.R1] ^ DSP.RBASEx4;
                    if(inst.r2of.R1_DI)
                      {
                        addr = dsp_read(addr);
                        kind = DSP_WRITE_REGISTER_INDIRECT;
                      }
                    else
                      {
                        kind = DSP_WRITE_REGISTER_DIRECT;
                      }
                    dsp_write(addr,op,kind);
                  }
                  break;
                case 48:
                case 49:
                case 50:
                case 51:
                case 52:
                case 53:
                case 54:
                case 55:
                case 56:
                case 57:
                case 58:
                case 59:
                case 60:
                case 61:
                case 62:
                case 63: /* move */
                  {
                    uint16_t op;
                    uint16_t addr;
                    dsp_write_kind_t kind;

                    op   = dsp_operand_load1();
                    addr = inst.cif.BCH_ADDR;
                    if(inst.nrof.DI)
                      {
                        addr = dsp_read(addr);
                        kind = DSP_WRITE_INDIRECT;
                      }
                    else
                      {
                        kind = DSP_WRITE_DIRECT;
                      }
                    dsp_write(addr,op,kind);
                  }
                  break;
                default: /* condition branch */
                  if(1 & DSP.BRCONDTAB[inst.br.bits][fExact+((flags.raw*0x10080402)>>24)])
                    dsp_branch_delay_take(&branch_delay,inst.cif.BCH_ADDR);
                  break;
                }
            }
          else
            {
              /* ALU instruction */
              DSP.flags.req.raw = DSP.INSTTRAS[inst.raw].req.raw;
              DSP.flags.BS      = DSP.INSTTRAS[inst.raw].BS;

              dsp_operand_load(inst.aif.NUMOPS);

              switch(inst.aif.MUXA)
                {
                case 3:
                  if(inst.aif.M2SEL == 0)
                    {
                      if((inst.aif.ALU == 3) || (inst.aif.ALU == 5))  // ACSBU signal
                        AOP = (flags.carry ? ((int)DSP.flags.MULT1<<16) & ALUSIZEMASK : 0);
                      else
                        AOP = (((int)DSP.flags.MULT1 * (((int32_t)Y >> 15) & ~1)) & ALUSIZEMASK);
                    }
                  else
                    {
                      AOP = (((int)DSP.flags.MULT1 * (int)DSP.flags.MULT2 * 2) & ALUSIZEMASK);
                    }
                  break;
                case 1:
                  AOP = (DSP.flags.ALU1 << 16);
                  break;
                case 0:
                  AOP = Y;
                  break;
                case 2:
                  AOP = (DSP.flags.ALU2 << 16);
                  break;
                }

              /* ACSBU signal */
              if((inst.aif.ALU == 3) || (inst.aif.ALU == 5))
                {
                  BOP = (flags.carry << 16);
                }
              else
                {
                  switch(inst.aif.MUXB)
                    {
                    case 0:
                      BOP = Y;
                      break;
                    case 1:
                      BOP = (DSP.flags.ALU1 << 16);
                      break;
                    case 2:
                      BOP = (DSP.flags.ALU2 << 16);
                      break;
                    case 3:
                      if(inst.aif.M2SEL == 0) // ACSBU == 0 here always
                        BOP = (((int)DSP.flags.MULT1 * (((int32_t)Y >> 15)) & ~1) & ALUSIZEMASK);
                      else
                        BOP = (((int)DSP.flags.MULT1 * (int)DSP.flags.MULT2 * 2) & ALUSIZEMASK);
                      break;
                    }
                }

              /* Any ALU op. change overflow and possible carry */
              flags.carry    = 0;
              flags.overflow = 0;
              switch(inst.aif.ALU)
                {
                case 0:
                  Y = AOP;
                  break;
                  //*
                case 1:
                  Y = (0 - BOP);
                  flags.carry    = SUB_CFLAG(0,BOP,Y);
                  flags.overflow = SUB_VFLAG(0,BOP,Y);
                  break;
                case 2:
                case 3:
                  Y = (AOP + BOP);
                  flags.carry    = ADD_CFLAG(AOP,BOP,Y);
                  flags.overflow = ADD_VFLAG(AOP,BOP,Y);
                  break;
                case 4:
                case 5:
                  Y = (AOP - BOP);
                  flags.carry    = SUB_CFLAG(AOP,BOP,Y);
                  flags.overflow = SUB_VFLAG(AOP,BOP,Y);
                  break;
                case 6:
                  Y = (AOP + 0x1000);
                  flags.carry    = ADD_CFLAG(AOP,0x1000,Y);
                  flags.overflow = ADD_VFLAG(AOP,0x1000,Y);
                  break;
                case 7:
                  Y = (AOP - 0x1000);
                  flags.carry    = SUB_CFLAG(AOP,0x1000,Y);
                  flags.overflow = SUB_VFLAG(AOP,0x1000,Y);
                  break;
                case 8:		// A
                  Y = AOP;
                  break;
                case 9:		// NOT A
                  Y = (AOP ^ ALUSIZEMASK);
                  break;
                case 10:	// A AND B
                  Y = (AOP & BOP);
                  break;
                case 11:	// A NAND B
                  Y = ((AOP & BOP) ^ ALUSIZEMASK);
                  break;
                case 12:	// A OR B
                  Y= (AOP | BOP);
                  break;
                case 13:	// A NOR B
                  Y = ((AOP | BOP) ^ ALUSIZEMASK);
                  break;
                case 14:	// A XOR B
                  Y = (AOP ^ BOP);
                  break;
                case 15:	// A XNOR B
                  Y = (AOP ^ BOP ^ ALUSIZEMASK);
                  break;
                }

              flags.zero     = ((Y & 0xFFFF0000) ? 0 : 1);
              flags.negative = ((Y >> 31) ? 1 : 0);
              fExact         = ((Y & 0x0000F000) ? 0 : 1);

              //and BarrelShifter
              switch(DSP.flags.BS)
                {
                case 1:
                case 17:
                  Y = Y << 1;
                  break;
                case 2:
                case 18:
                  Y = Y << 2;
                  break;
                case 3:
                case 19:
                  Y = Y << 3;
                  break;
                case 4:
                case 20:
                  Y = Y << 4;
                  break;
                case 5:
                case 21:
                  Y = Y << 5;
                  break;
                case 6:
                case 22:
                  Y = Y << 8;
                  break;

                  //arithmetic shifts
                case 9:
                  Y  = ((int32_t)Y >> 16);
                  Y &= ALUSIZEMASK;
                  break;
                case 10:
                  Y  = ((int32_t)Y >> 8);
                  Y &= ALUSIZEMASK;
                  break;
                case 11:
                  Y  = ((int32_t)Y >> 5);
                  Y &= ALUSIZEMASK;
                  break;
                case 12:
                  Y  = ((int32_t)Y >> 4);
                  Y &= ALUSIZEMASK;
                  break;
                case 13:
                  Y  = ((int32_t)Y >> 3);
                  Y &= ALUSIZEMASK;
                  break;
                case 14:
                  Y  = ((int32_t)Y >> 2);
                  Y &= ALUSIZEMASK;
                  break;
                case 15:
                  Y  = ((int32_t)Y >> 1);
                  Y &= ALUSIZEMASK;
                  break;

                  // logocal shift
                case 7:         // CLIP ari
                case 23:        // CLIP log
                  if(1 & flags.overflow)
                    {
                      if(1 & flags.negative)
                        Y = 0x7FFFF000;
                      else
                        Y = 0x80000000;
                    }
                  break;
                case 8:         // Load operand load sameself again (ari)
                case 24:        // same, but logicalshift
                  {
                    //int temp=flags.carry;
                    flags.carry = ((signed)Y < 0); // shift out bit to Carry
                    //Y=Y<<1;
                    //Y|=temp<<16;
                    Y = (((Y << 1) & 0xFFFE0000)   |
                         (flags.carry ? 1<<16 : 0) |
                         (Y & 0xF000));
                  }
                  break;
                case 25:
                  Y  = ((uint32_t)Y >> 16);
                  Y &= ALUSIZEMASK;
                  break;
                case 26:
                  Y  = ((uint32_t)Y >> 8);
                  Y &= ALUSIZEMASK;
                  break;
                case 27:
                  Y  = ((uint32_t)Y >> 5);
                  Y &= ALUSIZEMASK;
                  break;
                case 28:
                  Y  = ((uint32_t)Y >> 4);
                  Y &= ALUSIZEMASK;
                  break;
                case 29:
                  Y  = ((uint32_t)Y >> 3);
                  Y &= ALUSIZEMASK;
                  break;
                case 30:
                  Y  = ((uint32_t)Y >> 2);
                  Y &= ALUSIZEMASK;
                  break;
                case 31:
                  Y  = ((uint32_t)Y >> 1);
                  Y &= ALUSIZEMASK;
                  break;
                }

              if(DSP.flags.WRITEBACK)
                dsp_write(DSP.flags.WRITEBACK,((int32_t)Y) >> 16,DSP_WRITE_WRITEBACK);
            }

          if(redirect_after_slot)
            {
              branch_delay.kind = DSP_BRANCH_DELAY_NONE;
              DSP.dregs.PC      = redirect_target;
            }

        } while(work);


      if(1 & DSP.flags.GenFIQ)
        {
          DSP.flags.GenFIQ = false;
          opera_clio_fiq_generate(INT0_DSPPINT,0); /* AudioFIQ */
        }

      DSP.dregs.DSPPCNT -= SYSTEM_TICKS;
      if(DSP.dregs.DSPPCNT <= 0)
        DSP.dregs.DSPPCNT += DSP.dregs.DSPPRLD;
    }

  return ((DSP.IMem[DSPP_DAC_RIGHT] << 16) | DSP.IMem[DSPP_DAC_LEFT]);
}

/* CPU writes NMEM of DSP */
void
opera_dsp_mem_write(uint16_t addr_,
                     uint16_t val_)
{
  //mwriteh(addr,val);
  DSP.NMem[addr_ & LAST_N_MEM] = val_;
}

void
opera_dsp_set_running(int val_)
{
  DSP.flags.Running = (val_ & 1);
}

/* CPU writes to EI,I of DSP */
void
opera_dsp_imem_write(uint16_t addr_,
                      uint16_t val_)
{
  if(dsp_addr_in_range(addr_,DSPP_FIFOHEAD_ZERO_ADDRESS,DSPP_EI_FIFO_COUNT))
    {
      uint32_t channel;

      channel = (addr_ - DSPP_FIFOHEAD_ZERO_ADDRESS);
      DSP.CPUSupply[channel]               = 1;
      DSP.IMem[addr_ & DSPP_IMEM_LOW_MASK] = val_;
    }
  else if(!(addr_ & DSPP_IMEM_DIRECT_MASK))
    {
      DSP.IMem[addr_ & DSPP_IMEM_LOW_MASK] = val_;
    }
}

void
opera_dsp_arm_semaphore_write(uint32_t val_)
{
  /* CPU writes Sema4Data low 16 bits; CPU ACK writes are not modeled. */
  DSP.dregs.Sema4Data   = (val_ & DSPP_SEMA4_DATA_MASK);
  DSP.dregs.Sema4Status = DSPP_SEMA4_STATUS_CPU_WROTE;
}

/* CPU reads from EO,I of DSP */
uint16_t
opera_dsp_imem_read(uint16_t addr_)
{
  if(dsp_addr_in_range(addr_,DSPP_EO_FIFO_DATA_FIRST,NUM_AUDIO_OUTPUT_DMAS))
    return opera_clio_fifo_eo_read(addr_ & DSPP_FIFO_CHANNEL_MASK);

  switch(addr_)
    {
    case DSPP_AUDIO_STATUS_WRITE:
      return dsp_audio_status();
    case DSPP_SEMA4_ACK_WRITE:
      return DSP.dregs.Sema4Status;
    case DSPP_SEMA4_DATA_WRITE:
      return DSP.dregs.Sema4Data;
    case DSPP_INT_WRITE:
      return DSP.dregs.INT;
    case DSPP_DSPPRLD_WRITE:
      return DSP.dregs.DSPPRLD;
    default:
      break;
    }

  return DSP.IMem[addr_];
}

uint32_t
opera_dsp_arm_semaphore_read(void)
{
  return ((DSP.dregs.Sema4Status << DSPP_SEMA4_STATUS_SHIFT) |
          DSP.dregs.Sema4Data);
}
