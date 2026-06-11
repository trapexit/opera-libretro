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

#define TOPBIT       0x80000000
#define SYSTEM_TICKS 568        /* ceil(((25000000 / 44100) + 1)) */

#define DSP_NMEM_EXEC_WORDS      1024
#define DSP_ADD_INSN             0x6627
#define DSP_ADPCMHALFMONO_71_WORDS 69
#define DSP_ADPCMMONO_51_WORDS   49
#define DSP_ADPCMVARMONO_70_WORDS 68
#define DSP_ADPCMVARMONO_BLOCK_92_WORDS 90
#define DSP_ADPCMVARSTEREO_77_WORDS 75
#define DSP_ADPCMDUCK22S_158_WORDS 156
#define DSP_ADPCMDUCK22S_242_WORDS 240
#define DSP_ADPCMDUCK22S_250_WORDS 248
#define DSP_BENCHMARK_6_INSN     0x6640
#define DSP_BENCHMARK_6_WORDS    4
#define DSP_DCSQXDHALFMONO_59_WORDS 57
#define DSP_DCSQXDHALFMONO_62_WORDS 60
#define DSP_DCSQXDHALFSTEREO_92_WORDS 90
#define DSP_DCSQXDMONO_42_WORDS 40
#define DSP_DCSQXDSTEREO_61_WORDS 59
#define DSP_DCSQXDSTEREO_64_WORDS 62
#define DSP_DCSQXDVARMONO_56_WORDS 54
#define DSP_DIRECTOUT_INSN       0x4627
#define DSP_DIRECTOUT_MIX_LEFT   0x106
#define DSP_DIRECTOUT_MIX_RIGHT  0x107
#define DSP_DSPPDHDR_WORDS       35
#define DSP_DREG_PC              0x0EE
#define DSP_MIXER2X2_WORDS       18
#define DSP_MIXER4X2_WORDS       30
#define DSP_MIXER8X2_WORDS       54
#define DSP_SUBTRACT_INSN        0x6647

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

typedef bool (*dsp_fast_handler_t)(uint32_t       *Y_,
                                   dsp_alu_flags_t *flags_,
                                   int            *fExact_,
                                   uint32_t       *RBSR_,
                                   bool           *work_);

static dsp_fast_handler_t DSP_FAST_TABLE[DSP_NMEM_EXEC_WORDS];
static bool               DSP_FAST_DIRTY   = true;
static int                DSP_FAST_ENABLED = 1;

static uint16_t dsp_read(uint32_t addr_);
static void     dsp_write(uint32_t addr_, uint16_t val_);
static uint16_t dsp_operand_load1(void);
static void     dsp_operand_load(int requests_);
static void     dsp_interpret_step(uint32_t        *Y_,
                                   dsp_alu_flags_t *flags_,
                                   int             *fExact_,
                                   uint32_t        *RBSR_,
                                   bool            *work_);
static bool     dsp_fast_adpcmhalfmono_71_374(uint32_t        *Y_,
                                              dsp_alu_flags_t *flags_,
                                              int             *fExact_,
                                              uint32_t        *RBSR_,
                                              bool            *work_);
static bool     dsp_fast_adpcmhalfmono_71_3cd(uint32_t        *Y_,
                                              dsp_alu_flags_t *flags_,
                                              int             *fExact_,
                                              uint32_t        *RBSR_,
                                              bool            *work_);
static bool     dsp_fast_adpcmmono_51(uint32_t        *Y_,
                                      dsp_alu_flags_t *flags_,
                                      int             *fExact_,
                                      uint32_t        *RBSR_,
                                      bool            *work_);
static bool     dsp_fast_adpcmvarmono_70(uint32_t        *Y_,
                                         dsp_alu_flags_t *flags_,
                                         int             *fExact_,
                                         uint32_t        *RBSR_,
                                         bool            *work_);
static bool     dsp_fast_adpcmvarmono_block_92(uint32_t        *Y_,
                                               dsp_alu_flags_t *flags_,
                                               int             *fExact_,
                                               uint32_t        *RBSR_,
                                               bool            *work_);
static bool     dsp_fast_adpcmvarstereo_77(uint32_t        *Y_,
                                           dsp_alu_flags_t *flags_,
                                           int             *fExact_,
                                           uint32_t        *RBSR_,
                                           bool            *work_);
static bool     dsp_fast_adpcmduck22s_158(uint32_t        *Y_,
                                          dsp_alu_flags_t *flags_,
                                          int             *fExact_,
                                          uint32_t        *RBSR_,
                                          bool            *work_);
static bool     dsp_fast_adpcmduck22s_242(uint32_t        *Y_,
                                          dsp_alu_flags_t *flags_,
                                          int             *fExact_,
                                          uint32_t        *RBSR_,
                                          bool            *work_);
static bool     dsp_fast_adpcmduck22s_250(uint32_t        *Y_,
                                          dsp_alu_flags_t *flags_,
                                          int             *fExact_,
                                          uint32_t        *RBSR_,
                                          bool            *work_);
static bool     dsp_fast_benchmark_6(uint32_t        *Y_,
                                     dsp_alu_flags_t *flags_,
                                     int             *fExact_,
                                     uint32_t        *RBSR_,
                                     bool            *work_);
static bool     dsp_fast_dcsqxdhalfmono_59(uint32_t        *Y_,
                                           dsp_alu_flags_t *flags_,
                                           int             *fExact_,
                                           uint32_t        *RBSR_,
                                           bool            *work_);
static bool     dsp_fast_dcsqxdhalfmono_62(uint32_t        *Y_,
                                           dsp_alu_flags_t *flags_,
                                           int             *fExact_,
                                           uint32_t        *RBSR_,
                                           bool            *work_);
static bool     dsp_fast_dcsqxdhalfstereo_92(uint32_t        *Y_,
                                             dsp_alu_flags_t *flags_,
                                             int             *fExact_,
                                             uint32_t        *RBSR_,
                                             bool            *work_);
static bool     dsp_fast_dcsqxdmono_42(uint32_t        *Y_,
                                       dsp_alu_flags_t *flags_,
                                       int             *fExact_,
                                       uint32_t        *RBSR_,
                                       bool            *work_);
static bool     dsp_fast_dcsqxdstereo_61(uint32_t        *Y_,
                                         dsp_alu_flags_t *flags_,
                                         int             *fExact_,
                                         uint32_t        *RBSR_,
                                         bool            *work_);
static bool     dsp_fast_dcsqxdstereo_64(uint32_t        *Y_,
                                         dsp_alu_flags_t *flags_,
                                         int             *fExact_,
                                         uint32_t        *RBSR_,
                                         bool            *work_);
static bool     dsp_fast_dcsqxdvarmono_56(uint32_t        *Y_,
                                          dsp_alu_flags_t *flags_,
                                          int             *fExact_,
                                          uint32_t        *RBSR_,
                                          bool            *work_);
static bool     dsp_fast_add(uint32_t        *Y_,
                              dsp_alu_flags_t *flags_,
                              int             *fExact_,
                              uint32_t        *RBSR_,
                              bool            *work_);
static bool     dsp_fast_directout(uint32_t        *Y_,
                                    dsp_alu_flags_t *flags_,
                                   int             *fExact_,
                                   uint32_t        *RBSR_,
                                   bool            *work_);
static bool     dsp_fast_dsppdhdr(uint32_t        *Y_,
                                  dsp_alu_flags_t *flags_,
                                  int             *fExact_,
                                  uint32_t        *RBSR_,
                                  bool            *work_);
static bool     dsp_fast_interpret_block(uint32_t         start_,
                                         uint32_t         end_,
                                         uint32_t        *Y_,
                                         dsp_alu_flags_t *flags_,
                                         int             *fExact_,
                                         uint32_t        *RBSR_,
                                         bool            *work_);
static bool     dsp_fast_mixer2x2(uint32_t        *Y_,
                                   dsp_alu_flags_t *flags_,
                                  int             *fExact_,
                                  uint32_t        *RBSR_,
                                  bool            *work_);
static bool     dsp_fast_mixer4x2(uint32_t        *Y_,
                                   dsp_alu_flags_t *flags_,
                                  int             *fExact_,
                                  uint32_t        *RBSR_,
                                  bool            *work_);
static bool     dsp_fast_mixer8x2(uint32_t        *Y_,
                                   dsp_alu_flags_t *flags_,
                                  int             *fExact_,
                                  uint32_t        *RBSR_,
                                  bool            *work_);
static bool     dsp_fast_multiply(uint32_t        *Y_,
                                   dsp_alu_flags_t *flags_,
                                  int             *fExact_,
                                  uint32_t        *RBSR_,
                                  bool            *work_);
static bool     dsp_fast_subtract(uint32_t        *Y_,
                                   dsp_alu_flags_t *flags_,
                                  int             *fExact_,
                                  uint32_t        *RBSR_,
                                  bool            *work_);

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
  uint16_t val;

  /* addr &= 0x3FF; */
  switch(addr_)
    {
    case 0xEA:
      return prng16();
    case 0xEB:
      return DSP.dregs.AudioOutStatus;
    case 0xEC:
      return DSP.dregs.Sema4Status;
    case 0xED:
      return DSP.dregs.Sema4Data;
    case 0xEE:
      return DSP.dregs.PC;
    case 0xEF:
      return DSP.dregs.DSPPCNT;
    case 0xF0:
    case 0xF1:
    case 0xF2:
    case 0xF3:
    case 0xF4:
    case 0xF5:
    case 0xF6:
    case 0xF7:
    case 0xF8:
    case 0xF9:
    case 0xFA:
    case 0xFB:
    case 0xFC:
      /*
        printf("#DSP read from CPU!!! chan=0x%x\n",addr&0x0f);
        val=IMem[addr-0x80];
      */
      if(DSP.CPUSupply[addr_ - 0xF0])
        return (DSP.CPUSupply[addr_ - 0xF0] = 0, DSP.IMem[addr_ - 0x80]);
      return opera_clio_fifo_ei(addr_ & 0x0F);
    case 0x70:
    case 0x71:
    case 0x72:
    case 0x73:
    case 0x74:
    case 0x75:
    case 0x76:
    case 0x77:
    case 0x78:
    case 0x79:
    case 0x7A:
    case 0x7B:
    case 0x7C:
      //printf("#DSP read from CPU!!! chan=0x%x\n",addr&0x0f);
      if(DSP.CPUSupply[addr_ - 0x70])
        return (DSP.CPUSupply[addr_ - 0x70] = 0, DSP.IMem[addr_]);
      return opera_clio_fifo_ei_read(addr_ & 0x0F);
    case 0xD0:
    case 0xD1:
    case 0xD2:
    case 0xD3:
    case 0xD4:
    case 0xD5:
    case 0xD6:
    case 0xD7:
    case 0xD8:
    case 0xD9:
    case 0xDA:
    case 0xDB:
    case 0xDC:
    case 0xDD:
    case 0xDE:
      /*
        what is last two case's?
        if(CPUSupply[addr&0x0f])
        {
        CPUSupply[addr&0x0f]=0;
        printf("#DSP read from CPU!!! chan=0x%x\n",addr&0x0f);
        return IMem[0x70+addr&0x0f];
        }
        else
        printf("#DSP read EIFifo status 0x%4.4X\n",addr&0x0f);
      */
      if(DSP.CPUSupply[addr_ & 0x0F])
        return 2;
      return opera_clio_fifo_ei_status(addr_ & 0x0F);
    case 0xE0:
    case 0xE1:
    case 0xE2:
    case 0xE3:
      return opera_clio_fifo_eo_status(addr_ & 0x0F);
    default:
      //printf("#EIRead 0x%3.3X>=0x%4.4X\n",addr, IMem[addr_ & 0x7F]);
      addr_ -= 0x100;
      if(addr_ < 0x200)
        return DSP.IMem[addr_ | 0x100];
    }

  return DSP.IMem[addr_ & 0x7F];
}

/* DSP IWRITE (includes EO,I) */
static
void
dsp_write(uint32_t addr_,
          uint16_t val_)
{
  addr_ &= 0x3FF;
  switch(addr_)
    {
    case 0x3EB:
      DSP.dregs.AudioOutStatus = val_;
      break;
    case 0x3EC:
      /* DSP write to Sema4ACK */
      DSP.dregs.Sema4Status |= 0x01;
      break;
    case 0x3ED:
      DSP.dregs.Sema4Data   = val_;
      DSP.dregs.Sema4Status = 0x4;  /* DSP write to Sema4Data */
      break;
    case 0x3EE:
      DSP.dregs.INT    = val_;
      DSP.flags.GenFIQ = true;
      break;
    case 0x3EF:
      DSP.dregs.DSPPRLD = val_;
      break;
    case 0x3F0:
    case 0x3F1:
    case 0x3F2:
    case 0x3F3:
      opera_clio_fifo_eo(addr_ & 0x0F,val_);
      break;
    case 0x3FD:
      /* FLUSH EOFIFO */
      break;
    case 0x3FE: /* DAC Left channel */
    case 0x3FF: /* DAC Right channel */
      DSP.IMem[addr_] = val_;
      break;
    default:
      if(addr_ < 0x100)
        return;

      addr_ -= 0x100;
      if(addr_ < 0x200)
        DSP.IMem[addr_ | 0x100] = val_;
      else
        DSP.IMem[addr_ + 0x100] = val_;
      break;
    }
}

static
bool
dsp_fast_enabled(void)
{
  return (DSP_FAST_ENABLED != 0);
}

static
void
dsp_fast_invalidate(void)
{
  DSP_FAST_DIRTY = true;
}

static
bool
dsp_fast_direct_operand(ITAG_t const operand_)
{
  return ((operand_.nrof.TYPE == 4) && !operand_.nrof.DI);
}

static
bool
dsp_fast_direct_source_operand(ITAG_t const operand_)
{
  return (dsp_fast_direct_operand(operand_) &&
          (operand_.nrof.OP_ADDR != DSP_DREG_PC) &&
          (operand_.nrof.WB1 == 0));
}

static
bool
dsp_fast_direct_dest_operand(ITAG_t const operand_)
{
  return (dsp_fast_direct_operand(operand_) &&
          (operand_.nrof.WB1 != 0));
}

static
bool
dsp_fast_multiply_product_insn(uint16_t const insn_,
                               uint32_t const numops_)
{
  ITAG_t inst;

  inst.raw = insn_;

  return (!inst.aif.PAD &&
          (inst.aif.BS == 0) &&
          ((inst.aif.ALU == 0) || (inst.aif.ALU == 8)) &&
          (inst.aif.MUXA == 3) &&
          (inst.aif.MUXB == 0) &&
          (inst.aif.M2SEL != 0) &&
          (inst.aif.NUMOPS == numops_));
}

static
bool
dsp_fast_multiply_insn(uint16_t insn_)
{
  return dsp_fast_multiply_product_insn(insn_,3);
}

static
bool
dsp_fast_multiply_accumulate_insn(uint16_t insn_)
{
  ITAG_t inst;

  inst.raw = insn_;

  return (!inst.aif.PAD &&
          (inst.aif.BS == 7) &&
          (inst.aif.ALU == 2) &&
          (inst.aif.MUXA == 3) &&
          (inst.aif.MUXB == 0) &&
          (inst.aif.M2SEL != 0) &&
          (inst.aif.NUMOPS == 2));
}

static
bool
dsp_fast_accum_mix_insn(uint16_t insn_)
{
  ITAG_t inst;

  inst.raw = insn_;

  return (!inst.aif.PAD &&
          (inst.aif.BS == 7) &&
          (inst.aif.ALU == 2) &&
          (inst.aif.MUXA == 1) &&
          (inst.aif.MUXB == 0) &&
          (inst.aif.M2SEL == 0) &&
          (inst.aif.NUMOPS == 2));
}

static
bool
dsp_fast_nop_insn(uint16_t insn_)
{
  return ((insn_ & 0xFF80) == 0x8000);
}

static
bool
dsp_fast_pattern_match(uint32_t const        pc_,
                       uint32_t const        words_,
                       uint32_t const *const vals_,
                       uint32_t const *const masks_)
{
  uint32_t i;

  if((words_ > DSP_NMEM_EXEC_WORDS) ||
     (pc_ > (DSP_NMEM_EXEC_WORDS - words_)))
    return false;

  for(i = 0; i < words_; i++)
    {
      uint32_t const insn = DSP.NMem[pc_ + i];

      if((insn & masks_[i]) != (vals_[i] & masks_[i]))
        return false;

      if((masks_[i] & 0x00010000) && ((masks_[i] & 0x03FF) == 0))
        if((insn & 0x03FF) != ((pc_ + (vals_[i] & 0x03FF)) & 0x03FF))
          return false;
    }

  return true;
}

static
bool
dsp_fast_add_match(uint32_t pc_)
{
  ITAG_t src1;
  ITAG_t src2;
  ITAG_t dst;

  if(pc_ > (DSP_NMEM_EXEC_WORDS - 4))
    return false;

  if(DSP.NMem[pc_] != DSP_ADD_INSN)
    return false;

  src1.raw = DSP.NMem[pc_ + 1];
  src2.raw = DSP.NMem[pc_ + 2];
  dst.raw  = DSP.NMem[pc_ + 3];

  return (dsp_fast_direct_source_operand(src1) &&
          dsp_fast_direct_source_operand(src2) &&
          dsp_fast_direct_dest_operand(dst));
}

static
bool
dsp_fast_dsppdhdr_base_match(uint32_t const pc_)
{
  static uint32_t const vals[DSP_DSPPDHDR_WORDS] = {
    0x0000A80E,0x0000D81F,0x00002400,0x0000A004,
    0x0000A808,0x00004486,0x0000A4A6,0x00008417,
    0x00004480,0x0000A705,0x000041A0,0x0000DF00,
    0x0000A006,0x0000841D,0x00004640,0x0000A809,
    0x0000F000,0x00009006,0x0000A007,0x00004620,
    0x0000A804,0x0000F000,0x0000A81D,0x00004480,
    0x0000A705,0x000041A0,0x0000DF00,0x0000A007,
    0x0000841F,0x00004486,0x0000A4A7,0x00007D40,
    0x000024C6,0x00005C40,0x0000A4E9
  };
  static uint32_t const masks[DSP_DSPPDHDR_WORDS] = {
    0x0001FC00,0x0001FC00,0x0000FFFF,0x0000FFFF,
    0x0001FC00,0x0000FFFF,0x0000FFFF,0x0001FC00,
    0x0000FFFF,0x0000FFFF,0x0000FFFF,0x0000FFFF,
    0x0000FFFF,0x0001FC00,0x0000FFFF,0x0000FFFF,
    0x0000FFFF,0x0000FFFF,0x0000FFFF,0x0000FFFF,
    0x0000FFFF,0x0000FFFF,0x0001FC00,0x0000FFFF,
    0x0000FFFF,0x0000FFFF,0x0000FFFF,0x0000FFFF,
    0x0001FC00,0x0000FFFF,0x0000FFFF,0x0000FFFF,
    0x0000FFFF,0x0000FFFF,0x0000FFFF
  };
  return dsp_fast_pattern_match(pc_,DSP_DSPPDHDR_WORDS,vals,masks);
}

static
bool
dsp_fast_dsppdhdr_base_for_pc(uint32_t const  pc_,
                              uint32_t       *base_)
{
  static uint32_t const offsets[] = {0x00,0x08,0x0E,0x17,0x1D,0x1F};
  uint32_t i;

  for(i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++)
    if(pc_ >= offsets[i])
      {
        uint32_t const base = pc_ - offsets[i];

        if(dsp_fast_dsppdhdr_base_match(base))
          {
            *base_ = base;
            return true;
          }
      }

  return false;
}

static
bool
dsp_fast_dsppdhdr_match(uint32_t pc_)
{
  uint32_t base;

  return dsp_fast_dsppdhdr_base_for_pc(pc_,&base);
}

static
bool
dsp_fast_adpcmduck22s_158_base_match(uint32_t const pc_)
{
  static uint32_t const vals[DSP_ADPCMDUCK22S_158_WORDS] = {
    0x00008100,0x000046A0,0x00008105,0x0000C001,
    0x0000D460,0x00006620,0x00008000,0x0000C000,
    0x00008309,0x00008000,0x0000E49C,0x00004620,
    0x00008800,0x0000F000,0x0000A822,0x0000981D,
    0x00008024,0x00002400,0x0000801F,0x0000B42C,
    0x00004480,0x0000801E,0x00008102,0x0000208A,
    0x0000803F,0x000066A0,0x00008102,0x0000C0FF,
    0x0000802F,0x0000982B,0x00008023,0x00009828,
    0x0000C000,0x0000842C,0x00004480,0x00008000,
    0x00008000,0x00002140,0x0000D77F,0x0000D42A,
    0x00009800,0x0000C001,0x00002486,0x00008831,
    0x00009004,0x00008038,0x00009006,0x0000803A,
    0x00004484,0x00008041,0x0000A00A,0x00009007,
    0x00008036,0x00008869,0x0000984C,0x0000A007,
    0x00009800,0x0000A004,0x00009800,0x0000A006,
    0x00009004,0x00008047,0x00009006,0x00008049,
    0x0000900A,0x00008000,0x00009007,0x00008045,
    0x00008869,0x00009856,0x0000A007,0x00009800,
    0x0000A004,0x00009800,0x0000A006,0x0000240F,
    0x00008054,0x00005C20,0x00008053,0x0000E800,
    0x00004D27,0x0000805B,0x00008907,0x00009800,
    0x00008065,0x0000240F,0x0000805E,0x00005C20,
    0x0000805D,0x0000E800,0x00004D27,0x00008062,
    0x00008906,0x00009800,0x00008061,0x0000849C,
    0x00007D27,0x00008000,0x00008066,0x00008906,
    0x00007D27,0x00008000,0x00008000,0x00008907,
    0x0000849C,0x000046A0,0x0000A80A,0x0000D000,
    0x00006620,0x0000C000,0x0000A4C8,0x00008000,
    0x00008000,0x00009004,0x0000A018,0x000046A0,
    0x0000A00A,0x0000EE00,0x000021C0,0x0000C800,
    0x00004C80,0x0000A004,0x0000A009,0x00002400,
    0x0000A00A,0x0000C881,0x00002110,0x0000A809,
    0x00008000,0x00004627,0x0000B4E9,0x0000248A,
    0x0000A00A,0x000021AC,0x0000C070,0x00004420,
    0x0000C000,0x0000A008,0x00008000,0x00008000,
    0x00004627,0x0000B4D8,0x00008000,0x0000EC93,
    0x00009006,0x0000C000,0x00008499,0x00004640,
    0x0000A006,0x0000C059,0x0000E099,0x00009006,
    0x0000C058,0x00008200,0x00002400,0x00008000
  };
  static uint32_t const masks[DSP_ADPCMDUCK22S_158_WORDS] = {
    0x0000FFC0,0x0000FFFF,0x0000FFFF,0x0000FFFF,
    0x0001FC00,0x0000FFFF,0x0002FC00,0x0000FFFF,
    0x0000FFFF,0x0000FFFF,0x0001FC00,0x0000FFFF,
    0x0002FC00,0x0000FFFF,0x0001FC00,0x0002FC00,
    0x0002FC00,0x0000FFFF,0x0002FC00,0x0001FC00,
    0x0000FFFF,0x0002FC00,0x0000FFFF,0x0000FFFF,
    0x0002FC00,0x0000FFFF,0x0000FFFF,0x0000FFFF,
    0x0002FC00,0x0000FC00,0x0000FC00,0x0000FC00,
    0x0000FFFF,0x0001FC00,0x0000FFFF,0x0000FC00,
    0x0000FC00,0x0000FFFF,0x0000FFFF,0x0001FC00,
    0x0000FC00,0x0000FFFF,0x0000FFFF,0x0000FC00,
    0x0000FFFF,0x0002FC00,0x0000FFFF,0x0000FC00,
    0x0000FFFF,0x0000FC00,0x0000FFFF,0x0000FFFF,
    0x0002FC00,0x0001FC00,0x0000FC00,0x0000FFFF,
    0x0000FC00,0x0000FFFF,0x0000FC00,0x0000FFFF,
    0x0000FFFF,0x0002FC00,0x0000FFFF,0x0000FC00,
    0x0000FFFF,0x0000FC00,0x0000FFFF,0x0002FC00,
    0x0001FC00,0x0000FC00,0x0000FFFF,0x0000FC00,
    0x0000FFFF,0x0000FC00,0x0000FFFF,0x0000FFFF,
    0x0000FC00,0x0000FFFF,0x0002FC00,0x0000FFFF,
    0x0000FFFF,0x0002FC00,0x0000FFFF,0x0000FC00,
    0x0000FC00,0x0000FFFF,0x0000FC00,0x0000FFFF,
    0x0002FC00,0x0000FFFF,0x0000FFFF,0x0000FC00,
    0x0000FFFF,0x0000FC00,0x0000FC00,0x0001FC00,
    0x0000FFFF,0x0000FC00,0x0000FC00,0x0000FFFF,
    0x0000FFFF,0x0000FC00,0x0000FC00,0x0000FFFF,
    0x0001FC00,0x0000FFFF,0x0000FFFF,0x0000FFFF,
    0x0000FFFF,0x0002FC00,0x0000FFFF,0x0000FFFF,
    0x0000FFFF,0x0000FFFF,0x0000FFFF,0x0000FFFF,
    0x0000FFFF,0x0000FFFF,0x0000FFFF,0x0000FFFF,
    0x0000FFFF,0x0000FFFF,0x0000FFFF,0x0000FFFF,
    0x0000FFFF,0x0001FC00,0x0000FFFF,0x0000FFFF,
    0x0000FFFF,0x0000FFFF,0x0000FFFF,0x0000FFFF,
    0x0000FFFF,0x0000FFFF,0x0000FFFF,0x0000FFFF,
    0x0002FC00,0x0000FFFF,0x0000FFFF,0x0000FFFF,
    0x0000FFFF,0x0000FFFF,0x0000FFFF,0x0001FC00,
    0x0000FFFF,0x0000FFFF,0x0001FC00,0x0000FFFF,
    0x0000FFFF,0x0000FFFF,0x0001FC00,0x0000FFFF,
    0x0000FFFF,0x0000FFFF,0x0000FFFF,0x0002FC00
  };

  return dsp_fast_pattern_match(pc_,DSP_ADPCMDUCK22S_158_WORDS,vals,masks);
}

static
bool
dsp_fast_adpcmduck22s_158_base_for_pc(uint32_t const  pc_,
                                      uint32_t       *base_)
{
  static uint32_t const offsets[] = {
    0x00,0x60,0x22,0x2C,0x2A,0x36,0x69,0x45,0x81,0x93,0x99
  };
  uint32_t i;

  for(i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++)
    if(pc_ >= offsets[i])
      {
        uint32_t const base = pc_ - offsets[i];

        if(dsp_fast_adpcmduck22s_158_base_match(base))
          {
            *base_ = base;
            return true;
          }
      }

  return false;
}

static
bool
dsp_fast_adpcmduck22s_158_match(uint32_t pc_)
{
  uint32_t base;

  return dsp_fast_adpcmduck22s_158_base_for_pc(pc_,&base);
}

static
bool
dsp_fast_adpcmduck22s_242_base_match(uint32_t const pc_)
{
  static uint32_t const vals[DSP_ADPCMDUCK22S_242_WORDS] = {
    0x00008100,0x00006620,0x00008000,0x0000C000,
    0x00008309,0x00008000,0x0000C82B,0x00002140,
    0x0000F000,0x00004120,0x0000C06B,0x0000A008,
    0x00008000,0x00008000,0x00009018,0x00008000,
    0x00006640,0x0000C000,0x0000C001,0x00008017,
    0x00008000,0x00008000,0x00009886,0x00008019,
    0x00009800,0x0000801B,0x00009800,0x0000801D,
    0x00009800,0x0000804B,0x00009800,0x0000C002,
    0x00009800,0x0000C004,0x00009800,0x0000C006,
    0x00009800,0x0000C008,0x00009866,0x0000C000,
    0x000098B1,0x0000C000,0x000084E8,0x0000D434,
    0x00009837,0x0000C001,0x00009800,0x0000C001,
    0x00009800,0x0000C000,0x00008000,0x000084E8,
    0x0000900A,0x00008057,0x000046E0,0x0000885A,
    0x0000C001,0x00008000,0x0000D459,0x00002400,
    0x00008048,0x00008000,0x0000B43B,0x0000900A,
    0x0000804C,0x00008000,0x00004640,0x0000A00A,
    0x0000D77F,0x00008000,0x0000D457,0x00002400,
    0x00008000,0x00008000,0x0000B447,0x0000984F,
    0x00008000,0x00008000,0x0000662A,0x00008053,
    0x0000C000,0x00008064,0x000066A0,0x00008000,
    0x0000C0FF,0x000080AF,0x0000843B,0x0000989E,
    0x0000A00A,0x000046A0,0x000080A5,0x0000C001,
    0x00008000,0x0000B461,0x00004626,0x0000A80A,
    0x0000C000,0x00009004,0x00008097,0x00009006,
    0x00008099,0x00009007,0x0000809B,0x000046A0,
    0x0000A80A,0x0000D000,0x00006620,0x0000C0B6,
    0x0000A4C8,0x00008000,0x00008000,0x00009004,
    0x0000A018,0x000046A0,0x0000A00A,0x0000EE00,
    0x000021C0,0x0000C800,0x00004C80,0x0000A004,
    0x0000A009,0x00002400,0x0000A00A,0x0000C87F,
    0x00002110,0x0000A809,0x00008000,0x00004627,
    0x0000B4E9,0x0000248A,0x0000A00A,0x000021AC,
    0x0000C070,0x00004420,0x0000C0D1,0x0000A008,
    0x00008000,0x00008000,0x00004627,0x0000B4D8,
    0x00008000,0x0000EC91,0x00009006,0x0000C000,
    0x00008497,0x00004640,0x0000A006,0x0000C059,
    0x0000E097,0x00009006,0x0000C058,0x00009800,
    0x0000A004,0x00009800,0x0000A006,0x000098EA,
    0x0000A007,0x0000900A,0x00008000,0x00008000,
    0x00008000,0x00004624,0x0000A80A,0x0000C000,
    0x000046A0,0x00008000,0x0000C001,0x00008000,
    0x0000B4AC,0x00004626,0x0000A80A,0x0000C000,
    0x00009004,0x000080E2,0x00009006,0x000080E4,
    0x00009007,0x000080E6,0x000046A0,0x0000A80A,
    0x0000D000,0x00006620,0x0000C000,0x0000A4C8,
    0x00008000,0x00008000,0x00009004,0x0000A018,
    0x000046A0,0x0000A00A,0x0000EE00,0x000021C0,
    0x0000C800,0x00004C80,0x0000A004,0x0000A009,
    0x00002400,0x0000A00A,0x0000C8CA,0x00002110,
    0x0000A809,0x00008000,0x00004627,0x0000B4E9,
    0x0000248A,0x0000A00A,0x000021AC,0x0000C070,
    0x00004420,0x0000C000,0x0000A008,0x00008000,
    0x00008000,0x00004627,0x0000B4D8,0x00008000,
    0x0000ECDC,0x00009006,0x0000C000,0x000084E2,
    0x00004640,0x0000A006,0x0000C059,0x0000E0E2,
    0x00009006,0x0000C058,0x00009800,0x0000A004,
    0x00009800,0x0000A006,0x000098EE,0x0000A007,
    0x00007D27,0x000080ED,0x00008000,0x00008906,
    0x00007D27,0x00008000,0x00008000,0x00008907
  };
  static uint32_t const masks[DSP_ADPCMDUCK22S_242_WORDS] = {
    0x0000FFC0,0x0000FFFF,0x0002FC00,0x0000FFFF,
    0x0000FFFF,0x0000FFFF,0x0001FC00,0x0000FFFF,
    0x0000FFFF,0x0000FFFF,0x0002FC00,0x0000FFFF,
    0x0000FFFF,0x0000FFFF,0x0000FFFF,0x0002FC00,
    0x0000FFFF,0x0000FFFF,0x0000FFFF,0x0002FC00,
    0x0000FFFF,0x0000FFFF,0x0002FC00,0x0000FC00,
    0x0002FC00,0x0000FC00,0x0002FC00,0x0000FC00,
    0x0002FC00,0x0000FC00,0x0002FC00,0x0000FFFF,
    0x0002FC00,0x0000FFFF,0x0002FC00,0x0000FFFF,
    0x0002FC00,0x0000FFFF,0x0002FC00,0x0000FFFF,
    0x0002FC00,0x0000FFFF,0x0001FC00,0x0001FC00,
    0x0002FC00,0x0000FFFF,0x0002FC00,0x0000FFFF,
    0x0002FC00,0x0000FFFF,0x0000FFFF,0x0001FC00,
    0x0000FFFF,0x0002FC00,0x0000FFFF,0x0000FC00,
    0x0000FFFF,0x0000FFFF,0x0001FC00,0x0000FFFF,
    0x0102FC00,0x0000FFFF,0x0001FC00,0x0000FFFF,
    0x0002FC00,0x0000FFFF,0x0000FFFF,0x0000FFFF,
    0x0000FFFF,0x0000FFFF,0x0001FC00,0x0000FFFF,
    0x0000FC00,0x0000FFFF,0x0001FC00,0x0000FC00,
    0x0000FC00,0x0000FFFF,0x0000FFFF,0x0000FC00,
    0x0000FFFF,0x0002FC00,0x0000FFFF,0x0000FC00,
    0x0000FFFF,0x0002FC00,0x0001FC00,0x0000FC00,
    0x0000FFFF,0x0000FFFF,0x0000FC00,0x0000FFFF,
    0x0000FFFF,0x0001FC00,0x0000FFFF,0x0000FFFF,
    0x0000FFFF,0x0000FFFF,0x0002FC00,0x0000FFFF,
    0x0000FC00,0x0000FFFF,0x0000FC00,0x0000FFFF,
    0x0000FFFF,0x0000FFFF,0x0000FFFF,0x0000FC00,
    0x0000FFFF,0x0000FFFF,0x0000FFFF,0x0000FFFF,
    0x0000FFFF,0x0000FFFF,0x0000FFFF,0x0000FFFF,
    0x0000FFFF,0x0000FFFF,0x0000FFFF,0x0000FFFF,
    0x0000FFFF,0x0000FFFF,0x0000FFFF,0x0001FC00,
    0x0000FFFF,0x0000FFFF,0x0000FFFF,0x0000FFFF,
    0x0000FFFF,0x0000FFFF,0x0000FFFF,0x0000FFFF,
    0x0000FFFF,0x0000FFFF,0x0000FC00,0x0000FFFF,
    0x0000FFFF,0x0000FFFF,0x0000FFFF,0x0000FFFF,
    0x0000FFFF,0x0001FC00,0x0000FFFF,0x0000FFFF,
    0x0001FC00,0x0000FFFF,0x0000FFFF,0x0000FFFF,
    0x0001FC00,0x0000FFFF,0x0000FFFF,0x0000FC00,
    0x0000FFFF,0x0000FC00,0x0000FFFF,0x0000FC00,
    0x0000FFFF,0x0000FFFF,0x0000FC00,0x0000FFFF,
    0x0000FFFF,0x0000FFFF,0x0000FFFF,0x0000FFFF,
    0x0000FFFF,0x0000FC00,0x0000FFFF,0x0000FFFF,
    0x0001FC00,0x0000FFFF,0x0000FFFF,0x0000FFFF,
    0x0000FFFF,0x0002FC00,0x0000FFFF,0x0000FC00,
    0x0000FFFF,0x0000FC00,0x0000FFFF,0x0000FFFF,
    0x0000FFFF,0x0000FFFF,0x0000FC00,0x0000FFFF,
    0x0000FFFF,0x0000FFFF,0x0000FFFF,0x0000FFFF,
    0x0000FFFF,0x0000FFFF,0x0000FFFF,0x0000FFFF,
    0x0000FFFF,0x0000FFFF,0x0000FFFF,0x0000FFFF,
    0x0000FFFF,0x0000FFFF,0x0001FC00,0x0000FFFF,
    0x0000FFFF,0x0000FFFF,0x0000FFFF,0x0000FFFF,
    0x0000FFFF,0x0000FFFF,0x0000FFFF,0x0000FFFF,
    0x0000FFFF,0x0000FC00,0x0000FFFF,0x0000FFFF,
    0x0000FFFF,0x0000FFFF,0x0000FFFF,0x0000FFFF,
    0x0001FC00,0x0000FFFF,0x0000FFFF,0x0001FC00,
    0x0000FFFF,0x0000FFFF,0x0000FFFF,0x0001FC00,
    0x0000FFFF,0x0000FFFF,0x0000FC00,0x0000FFFF,
    0x0000FC00,0x0000FFFF,0x0000FC00,0x0000FFFF,
    0x0000FFFF,0x0002FC00,0x0000FC00,0x0000FFFF,
    0x0000FFFF,0x0000FC00,0x0000FC00,0x0000FFFF
  };

  return dsp_fast_pattern_match(pc_,DSP_ADPCMDUCK22S_242_WORDS,vals,masks);
}

static
bool
dsp_fast_adpcmduck22s_242_base_for_pc(uint32_t const  pc_,
                                      uint32_t       *base_)
{
  static uint32_t const offsets[] = {
    0x00,0x2B,0xE8,0x34,0x59,0x3B,0x57,0x47,
    0x61,0x7F,0x91,0x97,0xAC,0xCA,0xDC,0xE2
  };
  uint32_t i;

  for(i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++)
    if(pc_ >= offsets[i])
      {
        uint32_t const base = pc_ - offsets[i];

        if(dsp_fast_adpcmduck22s_242_base_match(base))
          {
            *base_ = base;
            return true;
          }
      }

  return false;
}

static
bool
dsp_fast_adpcmduck22s_242_match(uint32_t pc_)
{
  uint32_t base;

  return dsp_fast_adpcmduck22s_242_base_for_pc(pc_,&base);
}

static
bool
dsp_fast_adpcmduck22s_250_base_match(uint32_t const pc_)
{
  static uint32_t const vals[DSP_ADPCMDUCK22S_250_WORDS] = {
    0x00008100,0x00004620,0x00008006,0x0000C001,
    0x000041A0,0x0000C001,0x00008000,0x00008000,
    0x0000D4F0,0x00006620,0x00008000,0x0000C000,
    0x00008309,0x00008000,0x0000C833,0x00002140,
    0x0000F000,0x00004120,0x0000C073,0x0000A008,
    0x00008000,0x00008000,0x00009018,0x00008000,
    0x00006640,0x0000C000,0x0000C001,0x0000801F,
    0x00008000,0x00008000,0x0000988E,0x00008021,
    0x00009800,0x00008023,0x00009800,0x00008025,
    0x00009800,0x00008053,0x00009800,0x0000C002,
    0x00009800,0x0000C004,0x00009800,0x0000C006,
    0x00009800,0x0000C008,0x0000986E,0x0000C000,
    0x000098B9,0x0000C000,0x000084F0,0x0000D43C,
    0x0000983F,0x0000C001,0x00009800,0x0000C001,
    0x00009800,0x0000C000,0x00008000,0x000084F0,
    0x0000900A,0x0000805F,0x000046E0,0x00008862,
    0x0000C001,0x00008000,0x0000D461,0x00002400,
    0x00008050,0x00008000,0x0000B443,0x0000900A,
    0x00008054,0x00008000,0x00004640,0x0000A00A,
    0x0000D77F,0x00008000,0x0000D45F,0x00002400,
    0x00008000,0x00008000,0x0000B44F,0x00009857,
    0x00008000,0x00008000,0x0000662A,0x0000805B,
    0x0000C000,0x0000806C,0x000066A0,0x00008000,
    0x0000C0FF,0x000080B7,0x00008443,0x000098A6,
    0x0000A00A,0x000046A0,0x000080AD,0x0000C001,
    0x00008000,0x0000B469,0x00004626,0x0000A80A,
    0x0000C000,0x00009004,0x0000809F,0x00009006,
    0x000080A1,0x00009007,0x000080A3,0x000046A0,
    0x0000A80A,0x0000D000,0x00006620,0x0000C0BE,
    0x0000A4C8,0x00008000,0x00008000,0x00009004,
    0x0000A018,0x000046A0,0x0000A00A,0x0000EE00,
    0x000021C0,0x0000C800,0x00004C80,0x0000A004,
    0x0000A009,0x00002400,0x0000A00A,0x0000C887,
    0x00002110,0x0000A809,0x00008000,0x00004627,
    0x0000B4E9,0x0000248A,0x0000A00A,0x000021AC,
    0x0000C070,0x00004420,0x0000C0D9,0x0000A008,
    0x00008000,0x00008000,0x00004627,0x0000B4D8,
    0x00008000,0x0000EC99,0x00009006,0x0000C000,
    0x0000849F,0x00004640,0x0000A006,0x0000C059,
    0x0000E09F,0x00009006,0x0000C058,0x00009800,
    0x0000A004,0x00009800,0x0000A006,0x000098F2,
    0x0000A007,0x0000900A,0x00008000,0x00008000,
    0x00008000,0x00004624,0x0000A80A,0x0000C000,
    0x000046A0,0x00008000,0x0000C001,0x00008000,
    0x0000B4B4,0x00004626,0x0000A80A,0x0000C000,
    0x00009004,0x000080EA,0x00009006,0x000080EC,
    0x00009007,0x000080EE,0x000046A0,0x0000A80A,
    0x0000D000,0x00006620,0x0000C000,0x0000A4C8,
    0x00008000,0x00008000,0x00009004,0x0000A018,
    0x000046A0,0x0000A00A,0x0000EE00,0x000021C0,
    0x0000C800,0x00004C80,0x0000A004,0x0000A009,
    0x00002400,0x0000A00A,0x0000C8D2,0x00002110,
    0x0000A809,0x00008000,0x00004627,0x0000B4E9,
    0x0000248A,0x0000A00A,0x000021AC,0x0000C070,
    0x00004420,0x0000C000,0x0000A008,0x00008000,
    0x00008000,0x00004627,0x0000B4D8,0x00008000,
    0x0000ECE4,0x00009006,0x0000C000,0x000084EA,
    0x00004640,0x0000A006,0x0000C059,0x0000E0EA,
    0x00009006,0x0000C058,0x00009800,0x0000A004,
    0x00009800,0x0000A006,0x000098F6,0x0000A007,
    0x00007D27,0x000080F5,0x00008000,0x00008906,
    0x00007D27,0x00008000,0x00008000,0x00008907
  };
  static uint32_t const masks[DSP_ADPCMDUCK22S_250_WORDS] = {
    0x0000FFC0,0x0000FFFF,0x0002FC00,0x0000FFFF,
    0x0000FFFF,0x0000FFFF,0x0000FC00,0x0000FFFF,
    0x0001FC00,0x0000FFFF,0x0002FC00,0x0000FFFF,
    0x0000FFFF,0x0000FFFF,0x0001FC00,0x0000FFFF,
    0x0000FFFF,0x0000FFFF,0x0002FC00,0x0000FFFF,
    0x0000FFFF,0x0000FFFF,0x0000FFFF,0x0002FC00,
    0x0000FFFF,0x0000FFFF,0x0000FFFF,0x0002FC00,
    0x0000FFFF,0x0000FFFF,0x0002FC00,0x0000FC00,
    0x0002FC00,0x0000FC00,0x0002FC00,0x0000FC00,
    0x0002FC00,0x0000FC00,0x0002FC00,0x0000FFFF,
    0x0002FC00,0x0000FFFF,0x0002FC00,0x0000FFFF,
    0x0002FC00,0x0000FFFF,0x0002FC00,0x0000FFFF,
    0x0002FC00,0x0000FFFF,0x0001FC00,0x0001FC00,
    0x0002FC00,0x0000FFFF,0x0002FC00,0x0000FFFF,
    0x0002FC00,0x0000FFFF,0x0000FFFF,0x0001FC00,
    0x0000FFFF,0x0002FC00,0x0000FFFF,0x0000FC00,
    0x0000FFFF,0x0000FFFF,0x0001FC00,0x0000FFFF,
    0x0102FC00,0x0000FFFF,0x0001FC00,0x0000FFFF,
    0x0002FC00,0x0000FFFF,0x0000FFFF,0x0000FFFF,
    0x0000FFFF,0x0000FFFF,0x0001FC00,0x0000FFFF,
    0x0000FC00,0x0000FFFF,0x0001FC00,0x0000FC00,
    0x0000FC00,0x0000FFFF,0x0000FFFF,0x0000FC00,
    0x0000FFFF,0x0002FC00,0x0000FFFF,0x0000FC00,
    0x0000FFFF,0x0002FC00,0x0001FC00,0x0000FC00,
    0x0000FFFF,0x0000FFFF,0x0000FC00,0x0000FFFF,
    0x0000FFFF,0x0001FC00,0x0000FFFF,0x0000FFFF,
    0x0000FFFF,0x0000FFFF,0x0002FC00,0x0000FFFF,
    0x0000FC00,0x0000FFFF,0x0000FC00,0x0000FFFF,
    0x0000FFFF,0x0000FFFF,0x0000FFFF,0x0000FC00,
    0x0000FFFF,0x0000FFFF,0x0000FFFF,0x0000FFFF,
    0x0000FFFF,0x0000FFFF,0x0000FFFF,0x0000FFFF,
    0x0000FFFF,0x0000FFFF,0x0000FFFF,0x0000FFFF,
    0x0000FFFF,0x0000FFFF,0x0000FFFF,0x0001FC00,
    0x0000FFFF,0x0000FFFF,0x0000FFFF,0x0000FFFF,
    0x0000FFFF,0x0000FFFF,0x0000FFFF,0x0000FFFF,
    0x0000FFFF,0x0000FFFF,0x0000FC00,0x0000FFFF,
    0x0000FFFF,0x0000FFFF,0x0000FFFF,0x0000FFFF,
    0x0000FFFF,0x0001FC00,0x0000FFFF,0x0000FFFF,
    0x0001FC00,0x0000FFFF,0x0000FFFF,0x0000FFFF,
    0x0001FC00,0x0000FFFF,0x0000FFFF,0x0000FC00,
    0x0000FFFF,0x0000FC00,0x0000FFFF,0x0000FC00,
    0x0000FFFF,0x0000FFFF,0x0000FC00,0x0000FFFF,
    0x0000FFFF,0x0000FFFF,0x0000FFFF,0x0000FFFF,
    0x0000FFFF,0x0000FC00,0x0000FFFF,0x0000FFFF,
    0x0001FC00,0x0000FFFF,0x0000FFFF,0x0000FFFF,
    0x0000FFFF,0x0002FC00,0x0000FFFF,0x0000FC00,
    0x0000FFFF,0x0000FC00,0x0000FFFF,0x0000FFFF,
    0x0000FFFF,0x0000FFFF,0x0000FC00,0x0000FFFF,
    0x0000FFFF,0x0000FFFF,0x0000FFFF,0x0000FFFF,
    0x0000FFFF,0x0000FFFF,0x0000FFFF,0x0000FFFF,
    0x0000FFFF,0x0000FFFF,0x0000FFFF,0x0000FFFF,
    0x0000FFFF,0x0000FFFF,0x0001FC00,0x0000FFFF,
    0x0000FFFF,0x0000FFFF,0x0000FFFF,0x0000FFFF,
    0x0000FFFF,0x0000FFFF,0x0000FFFF,0x0000FFFF,
    0x0000FFFF,0x0000FC00,0x0000FFFF,0x0000FFFF,
    0x0000FFFF,0x0000FFFF,0x0000FFFF,0x0000FFFF,
    0x0001FC00,0x0000FFFF,0x0000FFFF,0x0001FC00,
    0x0000FFFF,0x0000FFFF,0x0000FFFF,0x0001FC00,
    0x0000FFFF,0x0000FFFF,0x0000FC00,0x0000FFFF,
    0x0000FC00,0x0000FFFF,0x0000FC00,0x0000FFFF,
    0x0000FFFF,0x0002FC00,0x0000FC00,0x0000FFFF,
    0x0000FFFF,0x0000FC00,0x0000FC00,0x0000FFFF
  };

  return dsp_fast_pattern_match(pc_,DSP_ADPCMDUCK22S_250_WORDS,vals,masks);
}

static
bool
dsp_fast_adpcmduck22s_250_base_for_pc(uint32_t const  pc_,
                                      uint32_t       *base_)
{
  static uint32_t const offsets[] = {
    0x00,0xF0,0x33,0x3C,0x61,0x43,0x5F,0x4F,
    0x69,0x87,0x99,0x9F,0xB4,0xD2,0xE4,0xEA
  };
  uint32_t i;

  for(i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++)
    if(pc_ >= offsets[i])
      {
        uint32_t const base = pc_ - offsets[i];

        if(dsp_fast_adpcmduck22s_250_base_match(base))
          {
            *base_ = base;
            return true;
          }
      }

  return false;
}

static
bool
dsp_fast_adpcmduck22s_250_match(uint32_t pc_)
{
  uint32_t base;

  return dsp_fast_adpcmduck22s_250_base_for_pc(pc_,&base);
}

static
bool
dsp_fast_adpcmhalfmono_71_374_base_match(uint32_t const pc_)
{
  static uint32_t const vals[DSP_ADPCMHALFMONO_71_WORDS] = {
    0x00002400,0x00008000,0x0000B400,0x00008100,
    0x00002400,0x00008011,0x0000B413,0x00009006,
    0x0000C000,0x00009007,0x0000C000,0x00009004,
    0x0000C007,0x00009814,0x0000C003,0x00002480,
    0x00008025,0x00009800,0x0000C000,0x00004620,
    0x0000881D,0x0000C001,0x000021A0,0x0000C001,
    0x0000B41C,0x00002400,0x0000803D,0x00008442,
    0x000046A0,0x00008021,0x0000C004,0x0000D42B,
    0x000046A0,0x0000802C,0x0000C002,0x0000D428,
    0x00004480,0x00008000,0x00008029,0x00008437,
    0x00002484,0x00008031,0x00008437,0x000046A0,
    0x00008000,0x0000C002,0x0000D434,0x00008000,
    0x00002486,0x00008035,0x00008437,0x00008000,
    0x00002486,0x00008000,0x00000084,0x00008800,
    0x00004120,0x0000C000,0x00008102,0x0000000F,
    0x00005C20,0x00008040,0x0000E800,0x00008000,
    0x00009800,0x00008102,0x00004C80,0x00008000,
    0x00008000
  };
  static uint32_t const masks[DSP_ADPCMHALFMONO_71_WORDS] = {
    0x0000FFFF,0x0102FC00,0x0001FC00,0x0000FFC0,
    0x0000FFFF,0x0002FC00,0x0001FC00,0x0000FFFF,
    0x0000FFFF,0x0000FFFF,0x0000FFFF,0x0000FFFF,
    0x0000FFFF,0x0002FC00,0x0000FFFF,0x0000FFFF,
    0x0002FC00,0x0000FC00,0x0000FFFF,0x0000FFFF,
    0x0000FC00,0x0000FFFF,0x0000FFFF,0x0000FFFF,
    0x0001FC00,0x0000FFFF,0x0002FC00,0x0001FC00,
    0x0000FFFF,0x0000FC00,0x0000FFFF,0x0001FC00,
    0x0000FFFF,0x0000FC00,0x0000FFFF,0x0001FC00,
    0x0000FFFF,0x0000FC00,0x0002FC00,0x0001FC00,
    0x0000FFFF,0x0000FC00,0x0001FC00,0x0000FFFF,
    0x0000FC00,0x0000FFFF,0x0001FC00,0x0000FFFF,
    0x0000FFFF,0x0000FC00,0x0001FC00,0x0000FFFF,
    0x0000FFFF,0x0000FC00,0x0000FFFF,0x0002FC00,
    0x0000FFFF,0x0000FFFF,0x0000FFFF,0x0000FFFF,
    0x0000FFFF,0x0000FC00,0x0000FFFF,0x0000FFFF,
    0x0000FC00,0x0000FFFF,0x0000FFFF,0x0002FC00,
    0x0002FC00
  };

  return dsp_fast_pattern_match(pc_,DSP_ADPCMHALFMONO_71_WORDS,vals,masks);
}

static
bool
dsp_fast_adpcmhalfmono_71_374_base_for_pc(uint32_t const  pc_,
                                          uint32_t       *base_)
{
  static uint32_t const offsets[] = {
    0x00,0x13,0x1C,0x42,0x2B,0x28,0x37,0x34,0x38
  };
  uint32_t i;

  for(i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++)
    if(pc_ >= offsets[i])
      {
        uint32_t const base = pc_ - offsets[i];

        if(dsp_fast_adpcmhalfmono_71_374_base_match(base))
          {
            *base_ = base;
            return true;
          }
      }

  return false;
}

static
bool
dsp_fast_adpcmhalfmono_71_374_match(uint32_t pc_)
{
  uint32_t base;

  return dsp_fast_adpcmhalfmono_71_374_base_for_pc(pc_,&base);
}

static
bool
dsp_fast_adpcmhalfmono_71_3cd_base_match(uint32_t const pc_)
{
  static uint32_t const vals[DSP_ADPCMHALFMONO_71_WORDS] = {
    0x00002400,0x00008000,0x0000B445,0x00008100,
    0x00002400,0x00008011,0x0000B413,0x00009006,
    0x0000C000,0x00009007,0x0000C000,0x00009004,
    0x0000C007,0x00009814,0x0000C003,0x00002480,
    0x00008025,0x00009800,0x0000C000,0x00004620,
    0x0000881D,0x0000C001,0x000021A0,0x0000C001,
    0x0000B41C,0x00002400,0x0000803D,0x00008442,
    0x000046A0,0x00008021,0x0000C004,0x0000D42B,
    0x000046A0,0x0000802C,0x0000C002,0x0000D428,
    0x00004480,0x00008000,0x00008029,0x00008437,
    0x00002484,0x00008031,0x00008437,0x000046A0,
    0x00008000,0x0000C002,0x0000D434,0x00008000,
    0x00002486,0x00008035,0x00008437,0x00008000,
    0x00002486,0x00008000,0x00000084,0x00008800,
    0x00004120,0x0000C000,0x00008102,0x0000000F,
    0x00005C20,0x00008040,0x0000E800,0x00008000,
    0x00009800,0x00008102,0x00004C80,0x00008000,
    0x00008000
  };
  static uint32_t const masks[DSP_ADPCMHALFMONO_71_WORDS] = {
    0x0000FFFF,0x0102FC00,0x0001FC00,0x0000FFC0,
    0x0000FFFF,0x0002FC00,0x0001FC00,0x0000FFFF,
    0x0000FFFF,0x0000FFFF,0x0000FFFF,0x0000FFFF,
    0x0000FFFF,0x0002FC00,0x0000FFFF,0x0000FFFF,
    0x0002FC00,0x0000FC00,0x0000FFFF,0x0000FFFF,
    0x0000FC00,0x0000FFFF,0x0000FFFF,0x0000FFFF,
    0x0001FC00,0x0000FFFF,0x0002FC00,0x0001FC00,
    0x0000FFFF,0x0000FC00,0x0000FFFF,0x0001FC00,
    0x0000FFFF,0x0000FC00,0x0000FFFF,0x0001FC00,
    0x0000FFFF,0x0000FC00,0x0002FC00,0x0001FC00,
    0x0000FFFF,0x0000FC00,0x0001FC00,0x0000FFFF,
    0x0000FC00,0x0000FFFF,0x0001FC00,0x0000FFFF,
    0x0000FFFF,0x0000FC00,0x0001FC00,0x0000FFFF,
    0x0000FFFF,0x0000FC00,0x0000FFFF,0x0002FC00,
    0x0000FFFF,0x0000FFFF,0x0000FFFF,0x0000FFFF,
    0x0000FFFF,0x0000FC00,0x0000FFFF,0x0000FFFF,
    0x0000FC00,0x0000FFFF,0x0000FFFF,0x0002FC00,
    0x0002FC00
  };

  return dsp_fast_pattern_match(pc_,DSP_ADPCMHALFMONO_71_WORDS,vals,masks);
}

static
bool
dsp_fast_adpcmhalfmono_71_3cd_base_for_pc(uint32_t const  pc_,
                                          uint32_t       *base_)
{
  static uint32_t const offsets[] = {
    0x00,0x13,0x1C,0x42,0x2B,0x28,0x37,0x34,0x38
  };
  uint32_t i;

  for(i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++)
    if(pc_ >= offsets[i])
      {
        uint32_t const base = pc_ - offsets[i];

        if(dsp_fast_adpcmhalfmono_71_3cd_base_match(base))
          {
            *base_ = base;
            return true;
          }
      }

  return false;
}

static
bool
dsp_fast_adpcmhalfmono_71_3cd_match(uint32_t pc_)
{
  uint32_t base;

  return dsp_fast_adpcmhalfmono_71_3cd_base_for_pc(pc_,&base);
}

static
bool
dsp_fast_adpcmmono_51_base_match(uint32_t const pc_)
{
  static uint32_t const vals[DSP_ADPCMMONO_51_WORDS] = {
    0x00008100,0x00002400,0x0000800E,0x0000B410,
    0x00009006,0x0000C000,0x00009007,0x0000C000,
    0x00009004,0x0000C007,0x00009811,0x0000C003,
    0x00002480,0x00008025,0x00009800,0x0000C000,
    0x00004620,0x00008015,0x0000C001,0x000041A0,
    0x0000C003,0x0000801C,0x00008000,0x0000B424,
    0x00002140,0x0000C001,0x0000B428,0x00004640,
    0x00008000,0x0000C002,0x0000B42B,0x00008000,
    0x00002486,0x00008026,0x00000084,0x0000842D,
    0x00004480,0x00008000,0x00008029,0x0000842D,
    0x00002484,0x0000802C,0x0000842D,0x00002486,
    0x00008000,0x00008800,0x00004C80,0x00008000,
    0x00008000
  };
  static uint32_t const masks[DSP_ADPCMMONO_51_WORDS] = {
    0x0000FFC0,0x0000FFFF,0x0002FC00,0x0001FC00,
    0x0000FFFF,0x0000FFFF,0x0000FFFF,0x0000FFFF,
    0x0000FFFF,0x0000FFFF,0x0002FC00,0x0000FFFF,
    0x0000FFFF,0x0002FC00,0x0000FC00,0x0000FFFF,
    0x0000FFFF,0x0000FC00,0x0000FFFF,0x0000FFFF,
    0x0000FFFF,0x0000FC00,0x0000FFFF,0x0001FC00,
    0x0000FFFF,0x0000FFFF,0x0001FC00,0x0000FFFF,
    0x0000FC00,0x0000FFFF,0x0001FC00,0x0000FFFF,
    0x0000FFFF,0x0002FC00,0x0000FFFF,0x0001FC00,
    0x0000FFFF,0x0000FC00,0x0000FC00,0x0001FC00,
    0x0000FFFF,0x0000FC00,0x0001FC00,0x0000FFFF,
    0x0000FC00,0x0002FC00,0x0000FFFF,0x0002FC00,
    0x0002FC00
  };

  return dsp_fast_pattern_match(pc_,DSP_ADPCMMONO_51_WORDS,vals,masks);
}

static
bool
dsp_fast_adpcmmono_51_base_for_pc(uint32_t const  pc_,
                                  uint32_t       *base_)
{
  static uint32_t const offsets[] = {
    0x00,0x10,0x24,0x28,0x2B,0x2D,0x2E
  };
  uint32_t i;

  for(i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++)
    if(pc_ >= offsets[i])
      {
        uint32_t const base = pc_ - offsets[i];

        if(dsp_fast_adpcmmono_51_base_match(base))
          {
            *base_ = base;
            return true;
          }
      }

  return false;
}

static
bool
dsp_fast_adpcmmono_51_match(uint32_t pc_)
{
  uint32_t base;

  return dsp_fast_adpcmmono_51_base_for_pc(pc_,&base);
}

static
bool
dsp_fast_adpcmvarmono_70_base_match(uint32_t const pc_)
{
  static uint32_t const vals[DSP_ADPCMVARMONO_70_WORDS] = {
    0x00008100,0x00002400,0x0000800E,0x0000B411,
    0x00009006,0x0000C000,0x00009007,0x0000C000,
    0x00009004,0x0000C007,0x0000981B,0x0000C003,
    0x00002480,0x00008025,0x00009800,0x0000C000,
    0x00008444,0x00004620,0x00008816,0x00008000,
    0x0000C83A,0x00004640,0x0000883B,0x0000F000,
    0x0000983C,0x00008039,0x00004620,0x00008821,
    0x0000C001,0x000021A0,0x0000C002,0x0000D42B,
    0x000046A0,0x0000802C,0x0000C001,0x0000D428,
    0x00004480,0x00008000,0x00008029,0x00008437,
    0x00002484,0x00008031,0x00008437,0x000046A0,
    0x00008000,0x0000C001,0x0000D434,0x00008000,
    0x00002486,0x00008035,0x00008437,0x00008000,
    0x00002486,0x00008000,0x00000084,0x00008800,
    0x00002000,0x0000803F,0x00007D40,0x00008040,
    0x0000803D,0x00008000,0x00005C40,0x00008000,
    0x00008000,0x00004C80,0x00008000,0x00008000
  };
  static uint32_t const masks[DSP_ADPCMVARMONO_70_WORDS] = {
    0x0000FFC0,0x0000FFFF,0x0002FC00,0x0001FC00,
    0x0000FFFF,0x0000FFFF,0x0000FFFF,0x0000FFFF,
    0x0000FFFF,0x0000FFFF,0x0002FC00,0x0000FFFF,
    0x0000FFFF,0x0002FC00,0x0000FC00,0x0000FFFF,
    0x0001FC00,0x0000FFFF,0x0002FC00,0x0002FC00,
    0x0001FC00,0x0000FFFF,0x0000FC00,0x0000FFFF,
    0x0002FC00,0x0002FC00,0x0000FFFF,0x0000FC00,
    0x0000FFFF,0x0000FFFF,0x0000FFFF,0x0001FC00,
    0x0000FFFF,0x0000FC00,0x0000FFFF,0x0001FC00,
    0x0000FFFF,0x0000FC00,0x0002FC00,0x0001FC00,
    0x0000FFFF,0x0000FC00,0x0001FC00,0x0000FFFF,
    0x0000FC00,0x0000FFFF,0x0001FC00,0x0000FFFF,
    0x0000FFFF,0x0000FC00,0x0001FC00,0x0000FFFF,
    0x0000FFFF,0x0000FC00,0x0000FFFF,0x0002FC00,
    0x0000FFFF,0x0000FC00,0x0000FFFF,0x0000FC00,
    0x0000FC00,0x0000FC00,0x0000FFFF,0x0000FC00,
    0x0000FC00,0x0000FFFF,0x0002FC00,0x0002FC00
  };

  return dsp_fast_pattern_match(pc_,DSP_ADPCMVARMONO_70_WORDS,vals,masks);
}

static
bool
dsp_fast_adpcmvarmono_70_base_for_pc(uint32_t const  pc_,
                                     uint32_t       *base_)
{
  static uint32_t const offsets[] = {
    0x00,0x11,0x3A,0x2B,0x28,0x37,0x34,0x38
  };
  uint32_t i;

  for(i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++)
    if(pc_ >= offsets[i])
      {
        uint32_t const base = pc_ - offsets[i];

        if(dsp_fast_adpcmvarmono_70_base_match(base))
          {
            *base_ = base;
            return true;
          }
      }

  return false;
}

static
bool
dsp_fast_adpcmvarmono_70_match(uint32_t pc_)
{
  uint32_t base;

  return dsp_fast_adpcmvarmono_70_base_for_pc(pc_,&base);
}

static
bool
dsp_fast_adpcmvarmono_block_92_base_match(uint32_t const pc_)
{
  static uint32_t const vals[DSP_ADPCMVARMONO_BLOCK_92_WORDS] = {
    0x00008100,0x00002400,0x00008000,0x0000B45A,
    0x00002400,0x00008011,0x0000B414,0x00009006,
    0x0000C000,0x00009007,0x0000C000,0x00009004,
    0x0000C007,0x0000981E,0x0000C003,0x00002480,
    0x0000802B,0x00009800,0x0000C000,0x0000845A,
    0x00004620,0x00008819,0x00008000,0x0000C850,
    0x00004640,0x00008851,0x0000F000,0x00009852,
    0x0000804F,0x00004620,0x00008824,0x0000C001,
    0x000021A0,0x0000C002,0x0000D441,0x000046A0,
    0x00008042,0x0000C001,0x0000D43E,0x00002400,
    0x00008031,0x0000D438,0x00004480,0x00008037,
    0x00008033,0x000021AA,0x0000DF00,0x00004120,
    0x0000C001,0x00008039,0x000066A0,0x0000803C,
    0x0000C0FF,0x0000A006,0x00009007,0x0000803B,
    0x00002470,0x00008800,0x00004480,0x00008000,
    0x0000803F,0x0000844D,0x00002484,0x00008047,
    0x0000844D,0x000046A0,0x00008000,0x0000C001,
    0x0000D44A,0x00008000,0x00002486,0x0000804B,
    0x0000844D,0x00008000,0x00002486,0x00008000,
    0x00000084,0x00008800,0x00002000,0x00008055,
    0x00007D40,0x00008056,0x00008053,0x00008000,
    0x00005C40,0x00008000,0x00008000,0x00004C80,
    0x00008000,0x00008000
  };
  static uint32_t const masks[DSP_ADPCMVARMONO_BLOCK_92_WORDS] = {
    0x0000FFC0,0x0000FFFF,0x0102FC00,0x0001FC00,
    0x0000FFFF,0x0002FC00,0x0001FC00,0x0000FFFF,
    0x0000FFFF,0x0000FFFF,0x0000FFFF,0x0000FFFF,
    0x0000FFFF,0x0002FC00,0x0000FFFF,0x0000FFFF,
    0x0002FC00,0x0000FC00,0x0000FFFF,0x0001FC00,
    0x0000FFFF,0x0002FC00,0x0002FC00,0x0001FC00,
    0x0000FFFF,0x0000FC00,0x0000FFFF,0x0002FC00,
    0x0002FC00,0x0000FFFF,0x0000FC00,0x0000FFFF,
    0x0000FFFF,0x0000FFFF,0x0001FC00,0x0000FFFF,
    0x0000FC00,0x0000FFFF,0x0001FC00,0x0000FFFF,
    0x0002FC00,0x0001FC00,0x0000FFFF,0x0000FC00,
    0x0002FC00,0x0000FFFF,0x0000FFFF,0x0000FFFF,
    0x0000FFFF,0x0000FC00,0x0000FFFF,0x0000FC00,
    0x0000FFFF,0x0000FFFF,0x0000FFFF,0x0000FC00,
    0x0000FFFF,0x0000FC00,0x0000FFFF,0x0000FC00,
    0x0000FC00,0x0001FC00,0x0000FFFF,0x0000FC00,
    0x0001FC00,0x0000FFFF,0x0000FC00,0x0000FFFF,
    0x0001FC00,0x0000FFFF,0x0000FFFF,0x0000FC00,
    0x0001FC00,0x0000FFFF,0x0000FFFF,0x0000FC00,
    0x0000FFFF,0x0002FC00,0x0000FFFF,0x0000FC00,
    0x0000FFFF,0x0000FC00,0x0000FC00,0x0000FC00,
    0x0000FFFF,0x0000FC00,0x0000FC00,0x0000FFFF,
    0x0002FC00,0x0002FC00
  };

  return dsp_fast_pattern_match(pc_,DSP_ADPCMVARMONO_BLOCK_92_WORDS,vals,masks);
}

static
bool
dsp_fast_adpcmvarmono_block_92_base_for_pc(uint32_t const  pc_,
                                           uint32_t       *base_)
{
  static uint32_t const offsets[] = {
    0x00,0x14,0x50,0x41,0x3E,0x38,0x4D,0x4A,0x4E
  };
  uint32_t i;

  for(i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++)
    if(pc_ >= offsets[i])
      {
        uint32_t const base = pc_ - offsets[i];

        if(dsp_fast_adpcmvarmono_block_92_base_match(base))
          {
            *base_ = base;
            return true;
          }
      }

  return false;
}

static
bool
dsp_fast_adpcmvarmono_block_92_match(uint32_t pc_)
{
  uint32_t base;

  return dsp_fast_adpcmvarmono_block_92_base_for_pc(pc_,&base);
}

static
bool
dsp_fast_adpcmvarstereo_77_base_match(uint32_t const pc_)
{
  static uint32_t const vals[DSP_ADPCMVARSTEREO_77_WORDS] = {
    0x00002400,0x00008013,0x0000B416,0x00008100,
    0x00009006,0x0000C000,0x00009007,0x0000C000,
    0x00009004,0x0000C007,0x00008100,0x00009006,
    0x0000C000,0x00009007,0x0000C000,0x00009004,
    0x0000C007,0x00009822,0x0000C001,0x00009800,
    0x0000C000,0x0000844B,0x00004620,0x0000881B,
    0x00008000,0x0000C837,0x00004640,0x00008838,
    0x0000F000,0x00009839,0x00008030,0x00009843,
    0x00008036,0x00004620,0x00008800,0x0000C001,
    0x000021A0,0x0000C001,0x0000D42B,0x00004480,
    0x00008000,0x0000802C,0x0000842D,0x00002486,
    0x00008833,0x00008100,0x00008834,0x00002000,
    0x0000803C,0x00008100,0x00002484,0x00008000,
    0x00008800,0x00002000,0x00008046,0x00007D40,
    0x0000803D,0x0000803A,0x00008000,0x00005C40,
    0x00008000,0x00008042,0x00004C80,0x00008049,
    0x00008000,0x00007D40,0x00008047,0x00008044,
    0x00008000,0x00005C40,0x00008000,0x00008000,
    0x00004C80,0x00008000,0x00008000
  };
  static uint32_t const masks[DSP_ADPCMVARSTEREO_77_WORDS] = {
    0x0000FFFF,0x0002FC00,0x0001FC00,0x0000FFC0,
    0x0000FFFF,0x0000FFFF,0x0000FFFF,0x0000FFFF,
    0x0000FFFF,0x0000FFFF,0x0000FFC0,0x0000FFFF,
    0x0000FFFF,0x0000FFFF,0x0000FFFF,0x0000FFFF,
    0x0000FFFF,0x0002FC00,0x0000FFFF,0x0000FC00,
    0x0000FFFF,0x0001FC00,0x0000FFFF,0x0002FC00,
    0x0002FC00,0x0001FC00,0x0000FFFF,0x0000FC00,
    0x0000FFFF,0x0002FC00,0x0002FC00,0x0002FC00,
    0x0002FC00,0x0000FFFF,0x0000FC00,0x0000FFFF,
    0x0000FFFF,0x0000FFFF,0x0001FC00,0x0000FFFF,
    0x0002FC00,0x0002FC00,0x0001FC00,0x0000FFFF,
    0x0000FC00,0x0000FFC0,0x0002FC00,0x0000FFFF,
    0x0000FC00,0x0000FFC0,0x0000FFFF,0x0000FC00,
    0x0000FC00,0x0000FFFF,0x0000FC00,0x0000FFFF,
    0x0000FC00,0x0000FC00,0x0000FC00,0x0000FFFF,
    0x0000FC00,0x0000FC00,0x0000FFFF,0x0002FC00,
    0x0002FC00,0x0000FFFF,0x0000FC00,0x0000FC00,
    0x0000FC00,0x0000FFFF,0x0000FC00,0x0000FC00,
    0x0000FFFF,0x0000FC00,0x0002FC00
  };

  return dsp_fast_pattern_match(pc_,DSP_ADPCMVARSTEREO_77_WORDS,vals,masks);
}

static
bool
dsp_fast_adpcmvarstereo_77_base_for_pc(uint32_t const  pc_,
                                       uint32_t       *base_)
{
  static uint32_t const offsets[] = {
    0x00,0x16,0x37,0x2B,0x2D,0x2F,0x34,0x35
  };
  uint32_t i;

  for(i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++)
    if(pc_ >= offsets[i])
      {
        uint32_t const base = pc_ - offsets[i];

        if(dsp_fast_adpcmvarstereo_77_base_match(base))
          {
            *base_ = base;
            return true;
          }
      }

  return false;
}

static
bool
dsp_fast_adpcmvarstereo_77_match(uint32_t pc_)
{
  uint32_t base;

  return dsp_fast_adpcmvarstereo_77_base_for_pc(pc_,&base);
}

static
bool
dsp_fast_benchmark_6_match(uint32_t const pc_)
{
  static uint32_t const vals[DSP_BENCHMARK_6_WORDS] = {
    0x00006640,0x00008104,0x000080EF,0x00008000
  };
  static uint32_t const masks[DSP_BENCHMARK_6_WORDS] = {
    0x0000FFFF,0x0000FFFF,0x0000FFFF,0x0002FC00
  };

  return dsp_fast_pattern_match(pc_,DSP_BENCHMARK_6_WORDS,vals,masks);
}

static
bool
dsp_fast_dcsqxdhalfmono_59_base_match(uint32_t const pc_)
{
  static uint32_t const vals[DSP_DCSQXDHALFMONO_59_WORDS] = {
    0x00004620,0x00008808,0x0000C001,0x00008000,
    0x000021A0,0x0000C001,0x0000D433,0x000046A0,
    0x00008000,0x0000C002,0x0000D412,0x00004480,
    0x00008000,0x00008013,0x000041A0,0x0000DF00,
    0x00008014,0x00008415,0x00004486,0x00008000,
    0x0000801A,0x00000000,0x0000E819,0x00008000,
    0x00000010,0x00004C80,0x0000801D,0x00008021,
    0x000046A0,0x00008000,0x0000C100,0x0000B424,
    0x00004627,0x00008025,0x00008826,0x00008427,
    0x00004480,0x00008000,0x00008000,0x00004120,
    0x0000C000,0x00008102,0x0000000F,0x00005C20,
    0x0000802F,0x0000E800,0x00008000,0x00009834,
    0x00008102,0x00008435,0x00008000,0x00002400,
    0x00008000,0x00004C80,0x00008000,0x00008000,
    0x00008000
  };
  static uint32_t const masks[DSP_DCSQXDHALFMONO_59_WORDS] = {
    0x0000FFFF,0x0002FC00,0x0000FFFF,0x0000FFFF,
    0x0000FFFF,0x0000FFFF,0x0001FC00,0x0000FFFF,
    0x0000FC00,0x0000FFFF,0x0001FC00,0x0000FFFF,
    0x0002FC00,0x0002FC00,0x0000FFFF,0x0000FFFF,
    0x0002FC00,0x0001FC00,0x0000FFFF,0x0000FC00,
    0x0000FC00,0x0000FFFF,0x0001FC00,0x0000FFFF,
    0x0000FFFF,0x0000FFFF,0x0000FC00,0x0002FC00,
    0x0000FFFF,0x0000FC00,0x0000FFFF,0x0001FC00,
    0x0000FFFF,0x0000FC00,0x0002FC00,0x0001FC00,
    0x0000FFFF,0x0000FC00,0x0000FC00,0x0000FFFF,
    0x0000FFFF,0x0000FFFF,0x0000FFFF,0x0000FFFF,
    0x0002FC00,0x0000FFFF,0x0000FFFF,0x0000FC00,
    0x0000FFFF,0x0001FC00,0x0000FFFF,0x0000FFFF,
    0x0000FC00,0x0000FFFF,0x0002FC00,0x0002FC00,
    0x0000FFFF
  };

  return dsp_fast_pattern_match(pc_,DSP_DCSQXDHALFMONO_59_WORDS,vals,masks);
}

static
bool
dsp_fast_dcsqxdhalfmono_59_base_for_pc(uint32_t const  pc_,
                                      uint32_t       *base_)
{
  static uint32_t const offsets[] = {
    0x00,0x33,0x12,0x15,0x19,0x24,0x27,0x35
  };
  uint32_t i;

  for(i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++)
    if(pc_ >= offsets[i])
      {
        uint32_t const base = pc_ - offsets[i];

        if(dsp_fast_dcsqxdhalfmono_59_base_match(base))
          {
            *base_ = base;
            return true;
          }
      }

  return false;
}

static
bool
dsp_fast_dcsqxdhalfmono_59_match(uint32_t pc_)
{
  uint32_t base;

  return dsp_fast_dcsqxdhalfmono_59_base_for_pc(pc_,&base);
}

static
bool
dsp_fast_dcsqxdhalfmono_62_base_match(uint32_t const pc_)
{
  static uint32_t const vals[DSP_DCSQXDHALFMONO_62_WORDS] = {
    0x00002400,0x00008000,0x0000B43C,0x00004620,
    0x0000880B,0x0000C001,0x00008000,0x000021A0,
    0x0000C001,0x0000D436,0x000046A0,0x00008000,
    0x0000C002,0x0000D415,0x00004480,0x00008000,
    0x00008016,0x000041A0,0x0000DF00,0x00008017,
    0x00008418,0x00004486,0x00008000,0x0000801D,
    0x00000000,0x0000E81C,0x00008000,0x00000010,
    0x00004C80,0x00008020,0x00008024,0x000046A0,
    0x00008000,0x0000C100,0x0000B427,0x00004627,
    0x00008028,0x00008829,0x0000842A,0x00004480,
    0x00008000,0x00008000,0x00004120,0x0000C000,
    0x00008102,0x0000000F,0x00005C20,0x00008032,
    0x0000E800,0x00008000,0x00009837,0x00008102,
    0x00008438,0x00008000,0x00002400,0x00008000,
    0x00004C80,0x00008000,0x00008000,0x00008000
  };
  static uint32_t const masks[DSP_DCSQXDHALFMONO_62_WORDS] = {
    0x0000FFFF,0x0102FC00,0x0001FC00,0x0000FFFF,
    0x0002FC00,0x0000FFFF,0x0000FFFF,0x0000FFFF,
    0x0000FFFF,0x0001FC00,0x0000FFFF,0x0000FC00,
    0x0000FFFF,0x0001FC00,0x0000FFFF,0x0002FC00,
    0x0002FC00,0x0000FFFF,0x0000FFFF,0x0002FC00,
    0x0001FC00,0x0000FFFF,0x0000FC00,0x0000FC00,
    0x0000FFFF,0x0001FC00,0x0000FFFF,0x0000FFFF,
    0x0000FFFF,0x0000FC00,0x0002FC00,0x0000FFFF,
    0x0000FC00,0x0000FFFF,0x0001FC00,0x0000FFFF,
    0x0000FC00,0x0002FC00,0x0001FC00,0x0000FFFF,
    0x0000FC00,0x0000FC00,0x0000FFFF,0x0000FFFF,
    0x0000FFFF,0x0000FFFF,0x0000FFFF,0x0002FC00,
    0x0000FFFF,0x0000FFFF,0x0000FC00,0x0000FFFF,
    0x0001FC00,0x0000FFFF,0x0000FFFF,0x0000FC00,
    0x0000FFFF,0x0002FC00,0x0002FC00,0x0000FFFF
  };

  return dsp_fast_pattern_match(pc_,DSP_DCSQXDHALFMONO_62_WORDS,vals,masks);
}

static
bool
dsp_fast_dcsqxdhalfmono_62_base_for_pc(uint32_t const  pc_,
                                      uint32_t       *base_)
{
  static uint32_t const offsets[] = {
    0x00,0x36,0x15,0x18,0x1C,0x27,0x2A,0x38
  };
  uint32_t i;

  for(i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++)
    if(pc_ >= offsets[i])
      {
        uint32_t const base = pc_ - offsets[i];

        if(dsp_fast_dcsqxdhalfmono_62_base_match(base))
          {
            *base_ = base;
            return true;
          }
      }

  return false;
}

static
bool
dsp_fast_dcsqxdhalfmono_62_match(uint32_t pc_)
{
  uint32_t base;

  return dsp_fast_dcsqxdhalfmono_62_base_for_pc(pc_,&base);
}

static
bool
dsp_fast_dcsqxdhalfstereo_92_base_match(uint32_t const pc_)
{
  static uint32_t const vals[DSP_DCSQXDHALFSTEREO_92_WORDS] = {
    0x00004620,0x00008800,0x0000F000,0x0000A80D,
    0x00007C80,0x00008033,0x0000800A,0x00008037,
    0x00007C80,0x00008053,0x00008036,0x00008057,
    0x0000845A,0x00004620,0x00008000,0x0000C000,
    0x0000B45A,0x00004480,0x00008000,0x00008018,
    0x000041A0,0x0000DF00,0x0000801B,0x00004486,
    0x00008000,0x0000803B,0x00002400,0x00008020,
    0x0000E81F,0x00008000,0x00000010,0x00004C80,
    0x00008023,0x00008027,0x000046A0,0x00008000,
    0x0000C100,0x0000B42A,0x00004620,0x0000802B,
    0x0000882C,0x0000842D,0x00004480,0x00008041,
    0x00008000,0x00008000,0x00004120,0x0000C000,
    0x00008039,0x0000000F,0x00005C20,0x00008038,
    0x0000E800,0x00004C80,0x00008056,0x00008000,
    0x00009800,0x00008000,0x00002400,0x00008040,
    0x0000E83F,0x00008000,0x00000010,0x00004C80,
    0x00008043,0x00008047,0x000046A0,0x00008000,
    0x0000C100,0x0000B44A,0x00004620,0x0000804B,
    0x0000884C,0x0000844D,0x00004480,0x00008000,
    0x00008000,0x00008000,0x00004120,0x0000C000,
    0x00008059,0x0000000F,0x00005C20,0x00008058,
    0x0000E800,0x00004C80,0x00008000,0x00008000,
    0x00009800,0x00008000
  };
  static uint32_t const masks[DSP_DCSQXDHALFSTEREO_92_WORDS] = {
    0x0000FFFF,0x0002FC00,0x0000FFFF,0x0001FC00,
    0x0000FFFF,0x0002FC00,0x0002FC00,0x0002FC00,
    0x0000FFFF,0x0002FC00,0x0000FC00,0x0002FC00,
    0x0001FC00,0x0000FFFF,0x0102FC00,0x0000FFFF,
    0x0001FC00,0x0000FFFF,0x0002FC00,0x0002FC00,
    0x0000FFFF,0x0000FFFF,0x0002FC00,0x0000FFFF,
    0x0000FC00,0x0002FC00,0x0000FFFF,0x0000FC00,
    0x0001FC00,0x0000FFFF,0x0000FFFF,0x0000FFFF,
    0x0000FC00,0x0002FC00,0x0000FFFF,0x0000FC00,
    0x0000FFFF,0x0001FC00,0x0000FFFF,0x0000FC00,
    0x0002FC00,0x0001FC00,0x0000FFFF,0x0000FC00,
    0x0000FC00,0x0000FFFF,0x0000FFFF,0x0000FFFF,
    0x0002FC00,0x0000FFFF,0x0000FFFF,0x0000FC00,
    0x0000FFFF,0x0000FFFF,0x0000FC00,0x0000FC00,
    0x0000FC00,0x0000FC00,0x0000FFFF,0x0000FC00,
    0x0001FC00,0x0000FFFF,0x0000FFFF,0x0000FFFF,
    0x0000FC00,0x0000FC00,0x0000FFFF,0x0000FC00,
    0x0000FFFF,0x0001FC00,0x0000FFFF,0x0000FC00,
    0x0002FC00,0x0001FC00,0x0000FFFF,0x0000FC00,
    0x0000FC00,0x0000FFFF,0x0000FFFF,0x0000FFFF,
    0x0002FC00,0x0000FFFF,0x0000FFFF,0x0000FC00,
    0x0000FFFF,0x0000FFFF,0x0000FC00,0x0000FC00,
    0x0000FC00,0x0000FC00
  };

  return dsp_fast_pattern_match(pc_,DSP_DCSQXDHALFSTEREO_92_WORDS,vals,masks);
}

static
bool
dsp_fast_dcsqxdhalfstereo_92_base_for_pc(uint32_t const  pc_,
                                        uint32_t       *base_)
{
  static uint32_t const offsets[] = {
    0x00,0x0D,0x1F,0x2A,0x2D,0x3F,0x4A,0x4D
  };
  uint32_t i;

  for(i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++)
    if(pc_ >= offsets[i])
      {
        uint32_t const base = pc_ - offsets[i];

        if(dsp_fast_dcsqxdhalfstereo_92_base_match(base))
          {
            *base_ = base;
            return true;
          }
      }

  return false;
}

static
bool
dsp_fast_dcsqxdhalfstereo_92_match(uint32_t pc_)
{
  uint32_t base;

  return dsp_fast_dcsqxdhalfstereo_92_base_for_pc(pc_,&base);
}

static
bool
dsp_fast_dcsqxdmono_42_base_match(uint32_t const pc_)
{
  static uint32_t const vals[DSP_DCSQXDMONO_42_WORDS] = {
    0x00004620,0x00008000,0x0000C000,0x0000B428,
    0x00004620,0x00008800,0x0000F000,0x0000A80C,
    0x00004486,0x0000800E,0x00008011,0x00008412,
    0x00004480,0x00008000,0x00008000,0x000041A0,
    0x0000DF00,0x00008017,0x00000000,0x0000E816,
    0x00008000,0x00000010,0x00004C80,0x0000801A,
    0x0000801E,0x000046A0,0x00008000,0x0000C100,
    0x0000B421,0x00004627,0x00008022,0x00008823,
    0x00008424,0x00004480,0x00008000,0x00008000,
    0x00008000,0x00004C80,0x00008000,0x00008000
  };
  static uint32_t const masks[DSP_DCSQXDMONO_42_WORDS] = {
    0x0000FFFF,0x0102FC00,0x0000FFFF,0x0001FC00,
    0x0000FFFF,0x0002FC00,0x0000FFFF,0x0001FC00,
    0x0000FFFF,0x0002FC00,0x0002FC00,0x0001FC00,
    0x0000FFFF,0x0002FC00,0x0000FC00,0x0000FFFF,
    0x0000FFFF,0x0000FC00,0x0000FFFF,0x0001FC00,
    0x0000FFFF,0x0000FFFF,0x0000FFFF,0x0000FC00,
    0x0002FC00,0x0000FFFF,0x0000FC00,0x0000FFFF,
    0x0001FC00,0x0000FFFF,0x0000FC00,0x0002FC00,
    0x0001FC00,0x0000FFFF,0x0000FC00,0x0000FC00,
    0x0000FFFF,0x0000FFFF,0x0002FC00,0x0002FC00
  };

  return dsp_fast_pattern_match(pc_,DSP_DCSQXDMONO_42_WORDS,vals,masks);
}

static
bool
dsp_fast_dcsqxdmono_42_base_for_pc(uint32_t const  pc_,
                                  uint32_t       *base_)
{
  static uint32_t const offsets[] = {
    0x00,0x0C,0x12,0x16,0x21,0x24
  };
  uint32_t i;

  for(i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++)
    if(pc_ >= offsets[i])
      {
        uint32_t const base = pc_ - offsets[i];

        if(dsp_fast_dcsqxdmono_42_base_match(base))
          {
            *base_ = base;
            return true;
          }
      }

  return false;
}

static
bool
dsp_fast_dcsqxdmono_42_match(uint32_t pc_)
{
  uint32_t base;

  return dsp_fast_dcsqxdmono_42_base_for_pc(pc_,&base);
}

static
bool
dsp_fast_dcsqxdstereo_61_base_match(uint32_t const pc_)
{
  static uint32_t const vals[DSP_DCSQXDSTEREO_61_WORDS] = {
    0x00004620,0x00008000,0x0000C000,0x0000B43B,
    0x00004480,0x00008000,0x0000800B,0x000041A0,
    0x0000DF00,0x0000800E,0x00004486,0x00008000,
    0x00008025,0x00002400,0x00008013,0x0000E812,
    0x00008000,0x00000010,0x00004C80,0x00008016,
    0x0000801A,0x000046A0,0x00008000,0x0000C100,
    0x0000B41D,0x00004620,0x0000801E,0x0000881F,
    0x00008420,0x00004480,0x0000802B,0x00008000,
    0x00008000,0x00004C80,0x00008039,0x00008000,
    0x00002400,0x0000802A,0x0000E829,0x00008000,
    0x00000010,0x00004C80,0x0000802D,0x00008031,
    0x000046A0,0x00008000,0x0000C100,0x0000B434,
    0x00004620,0x00008035,0x00008836,0x00008437,
    0x00004480,0x00008000,0x00008000,0x00008000,
    0x00004C80,0x00008000,0x00008000
  };
  static uint32_t const masks[DSP_DCSQXDSTEREO_61_WORDS] = {
    0x0000FFFF,0x0102FC00,0x0000FFFF,0x0001FC00,
    0x0000FFFF,0x0002FC00,0x0002FC00,0x0000FFFF,
    0x0000FFFF,0x0002FC00,0x0000FFFF,0x0000FC00,
    0x0002FC00,0x0000FFFF,0x0000FC00,0x0001FC00,
    0x0000FFFF,0x0000FFFF,0x0000FFFF,0x0000FC00,
    0x0002FC00,0x0000FFFF,0x0000FC00,0x0000FFFF,
    0x0001FC00,0x0000FFFF,0x0000FC00,0x0002FC00,
    0x0001FC00,0x0000FFFF,0x0000FC00,0x0000FC00,
    0x0000FFFF,0x0000FFFF,0x0002FC00,0x0002FC00,
    0x0000FFFF,0x0000FC00,0x0001FC00,0x0000FFFF,
    0x0000FFFF,0x0000FFFF,0x0000FC00,0x0000FC00,
    0x0000FFFF,0x0000FC00,0x0000FFFF,0x0001FC00,
    0x0000FFFF,0x0000FC00,0x0002FC00,0x0001FC00,
    0x0000FFFF,0x0000FC00,0x0000FC00,0x0000FFFF,
    0x0000FFFF,0x0000FC00,0x0002FC00
  };

  return dsp_fast_pattern_match(pc_,DSP_DCSQXDSTEREO_61_WORDS,vals,masks);
}

static
bool
dsp_fast_dcsqxdstereo_61_base_for_pc(uint32_t const  pc_,
                                    uint32_t       *base_)
{
  static uint32_t const offsets[] = {
    0x00,0x12,0x1D,0x20,0x29,0x34,0x37
  };
  uint32_t i;

  for(i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++)
    if(pc_ >= offsets[i])
      {
        uint32_t const base = pc_ - offsets[i];

        if(dsp_fast_dcsqxdstereo_61_base_match(base))
          {
            *base_ = base;
            return true;
          }
      }

  return false;
}

static
bool
dsp_fast_dcsqxdstereo_61_match(uint32_t pc_)
{
  uint32_t base;

  return dsp_fast_dcsqxdstereo_61_base_for_pc(pc_,&base);
}

static
bool
dsp_fast_dcsqxdstereo_64_base_match(uint32_t const pc_)
{
  static uint32_t const vals[DSP_DCSQXDSTEREO_64_WORDS] = {
    0x00004620,0x00008000,0x0000C000,0x0000B43B,
    0x00004480,0x00008000,0x0000800B,0x000041A0,
    0x0000DF00,0x0000800E,0x00004486,0x00008000,
    0x00008025,0x00002400,0x00008013,0x0000E812,
    0x00008000,0x00000010,0x00004C80,0x00008016,
    0x0000801A,0x000046A0,0x00008000,0x0000C100,
    0x0000B41D,0x00004620,0x0000801E,0x0000881F,
    0x00008420,0x00004480,0x0000802B,0x00008000,
    0x00008000,0x00004C80,0x00008039,0x00008000,
    0x00002400,0x0000802A,0x0000E829,0x00008000,
    0x00000010,0x00004C80,0x0000802D,0x00008031,
    0x000046A0,0x00008000,0x0000C100,0x0000B434,
    0x00004620,0x00008035,0x00008836,0x00008437,
    0x00004480,0x00008000,0x00008000,0x00008000,
    0x00004C80,0x00008000,0x00008000,0x00008000,
    0x00008000,0x00008000
  };
  static uint32_t const masks[DSP_DCSQXDSTEREO_64_WORDS] = {
    0x0000FFFF,0x0102FC00,0x0000FFFF,0x0001FC00,
    0x0000FFFF,0x0002FC00,0x0002FC00,0x0000FFFF,
    0x0000FFFF,0x0002FC00,0x0000FFFF,0x0000FC00,
    0x0002FC00,0x0000FFFF,0x0000FC00,0x0001FC00,
    0x0000FFFF,0x0000FFFF,0x0000FFFF,0x0000FC00,
    0x0002FC00,0x0000FFFF,0x0000FC00,0x0000FFFF,
    0x0001FC00,0x0000FFFF,0x0000FC00,0x0002FC00,
    0x0001FC00,0x0000FFFF,0x0000FC00,0x0000FC00,
    0x0000FFFF,0x0000FFFF,0x0002FC00,0x0002FC00,
    0x0000FFFF,0x0000FC00,0x0001FC00,0x0000FFFF,
    0x0000FFFF,0x0000FFFF,0x0000FC00,0x0000FC00,
    0x0000FFFF,0x0000FC00,0x0000FFFF,0x0001FC00,
    0x0000FFFF,0x0000FC00,0x0002FC00,0x0001FC00,
    0x0000FFFF,0x0000FC00,0x0000FC00,0x0000FFFF,
    0x0000FFFF,0x0000FC00,0x0002FC00,0x0000FFFF,
    0x0000FFFF,0x0000FFFF
  };

  return dsp_fast_pattern_match(pc_,DSP_DCSQXDSTEREO_64_WORDS,vals,masks);
}

static
bool
dsp_fast_dcsqxdstereo_64_base_for_pc(uint32_t const  pc_,
                                    uint32_t       *base_)
{
  static uint32_t const offsets[] = {
    0x00,0x3B,0x12,0x1D,0x20,0x29,0x34,0x37
  };
  uint32_t i;

  for(i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++)
    if(pc_ >= offsets[i])
      {
        uint32_t const base = pc_ - offsets[i];

        if(dsp_fast_dcsqxdstereo_64_base_match(base))
          {
            *base_ = base;
            return true;
          }
      }

  return false;
}

static
bool
dsp_fast_dcsqxdstereo_64_match(uint32_t pc_)
{
  uint32_t base;

  return dsp_fast_dcsqxdstereo_64_base_for_pc(pc_,&base);
}

static
bool
dsp_fast_dcsqxdvarmono_56_base_match(uint32_t const pc_)
{
  static uint32_t const vals[DSP_DCSQXDVARMONO_56_WORDS] = {
    0x00002400,0x00008000,0x0000B436,0x00004620,
    0x00008808,0x00008000,0x0000C82C,0x00004640,
    0x0000882D,0x0000F000,0x0000982E,0x00008027,
    0x00004620,0x00008800,0x0000F000,0x0000A814,
    0x00004486,0x00008016,0x00008019,0x0000841A,
    0x00004480,0x00008000,0x00008000,0x000041A0,
    0x0000DF00,0x0000801F,0x00000000,0x0000E81E,
    0x00008000,0x00000010,0x00004C80,0x00008022,
    0x00008026,0x000046A0,0x00008000,0x0000C100,
    0x0000B429,0x00004627,0x0000802A,0x00008829,
    0x0000842C,0x00009831,0x00008000,0x00008000,
    0x00007D40,0x00008032,0x0000802F,0x00008000,
    0x00005C40,0x00008000,0x00008000,0x00004C80,
    0x00008000,0x00008000
  };
  static uint32_t const masks[DSP_DCSQXDVARMONO_56_WORDS] = {
    0x0000FFFF,0x0102FC00,0x0001FC00,0x0000FFFF,
    0x0002FC00,0x0002FC00,0x0001FC00,0x0000FFFF,
    0x0000FC00,0x0000FFFF,0x0002FC00,0x0002FC00,
    0x0000FFFF,0x0002FC00,0x0000FFFF,0x0001FC00,
    0x0000FFFF,0x0002FC00,0x0002FC00,0x0001FC00,
    0x0000FFFF,0x0002FC00,0x0000FC00,0x0000FFFF,
    0x0000FFFF,0x0000FC00,0x0000FFFF,0x0001FC00,
    0x0000FFFF,0x0000FFFF,0x0000FFFF,0x0000FC00,
    0x0002FC00,0x0000FFFF,0x0000FC00,0x0000FFFF,
    0x0001FC00,0x0000FFFF,0x0000FC00,0x0000FC00,
    0x0001FC00,0x0000FC00,0x0000FC00,0x0000FFFF,
    0x0000FFFF,0x0000FC00,0x0000FC00,0x0000FC00,
    0x0000FFFF,0x0000FC00,0x0000FC00,0x0000FFFF,
    0x0002FC00,0x0002FC00
  };

  return dsp_fast_pattern_match(pc_,DSP_DCSQXDVARMONO_56_WORDS,vals,masks);
}

static
bool
dsp_fast_dcsqxdvarmono_56_base_for_pc(uint32_t const  pc_,
                                      uint32_t       *base_)
{
  static uint32_t const offsets[] = {
    0x00,0x2C,0x14,0x1A,0x1E,0x29
  };
  uint32_t i;

  for(i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++)
    if(pc_ >= offsets[i])
      {
        uint32_t const base = pc_ - offsets[i];

        if(dsp_fast_dcsqxdvarmono_56_base_match(base))
          {
            *base_ = base;
            return true;
          }
      }

  return false;
}

static
bool
dsp_fast_dcsqxdvarmono_56_match(uint32_t pc_)
{
  uint32_t base;

  return dsp_fast_dcsqxdvarmono_56_base_for_pc(pc_,&base);
}

static
bool
dsp_fast_mixer_channel_match(uint32_t const pc_,
                             uint32_t const off_,
                             uint32_t const terms_,
                             uint32_t const mix_addr_)
{
  uint32_t term;
  ITAG_t mix;
  ITAG_t accum;

  if(!dsp_fast_multiply_product_insn(DSP.NMem[pc_ + off_],2))
    return false;

  for(term = 1; term < terms_; term++)
    if(!dsp_fast_multiply_accumulate_insn(DSP.NMem[pc_ + off_ + (term * 3)]))
      return false;

  if(!dsp_fast_accum_mix_insn(DSP.NMem[pc_ + off_ + (terms_ * 3)]))
    return false;

  for(term = 0; term < terms_; term++)
    {
      ITAG_t src1;
      ITAG_t src2;

      src1.raw = DSP.NMem[pc_ + off_ + (term * 3) + 1];
      src2.raw = DSP.NMem[pc_ + off_ + (term * 3) + 2];

      if(!dsp_fast_direct_source_operand(src1) ||
         !dsp_fast_direct_source_operand(src2))
        return false;
    }

  mix.raw   = DSP.NMem[pc_ + off_ + (terms_ * 3) + 1];
  accum.raw = DSP.NMem[pc_ + off_ + (terms_ * 3) + 2];

  return (dsp_fast_direct_dest_operand(mix) &&
          (mix.nrof.OP_ADDR == mix_addr_) &&
          dsp_fast_direct_source_operand(accum));
}

static
bool
dsp_fast_mixer2x2_match(uint32_t pc_)
{
  if(pc_ > (DSP_NMEM_EXEC_WORDS - DSP_MIXER2X2_WORDS))
    return false;

  return (dsp_fast_mixer_channel_match(pc_,0,2,DSP_DIRECTOUT_MIX_LEFT) &&
          dsp_fast_mixer_channel_match(pc_,9,2,DSP_DIRECTOUT_MIX_RIGHT));
}

static
bool
dsp_fast_mixer4x2_match(uint32_t pc_)
{
  if(pc_ > (DSP_NMEM_EXEC_WORDS - DSP_MIXER4X2_WORDS))
    return false;

  return (dsp_fast_mixer_channel_match(pc_,0,4,DSP_DIRECTOUT_MIX_LEFT) &&
          dsp_fast_mixer_channel_match(pc_,15,4,DSP_DIRECTOUT_MIX_RIGHT));
}

static
bool
dsp_fast_mixer8x2_match_gap(uint32_t const pc_,
                            uint32_t const gap_)
{
  uint32_t const right_off = 27 + gap_;

  if(pc_ > (DSP_NMEM_EXEC_WORDS - (DSP_MIXER8X2_WORDS + gap_)))
    return false;

  if((gap_ != 0) && !dsp_fast_nop_insn(DSP.NMem[pc_ + 27]))
    return false;

  return (dsp_fast_mixer_channel_match(pc_,0,8,DSP_DIRECTOUT_MIX_LEFT) &&
          dsp_fast_mixer_channel_match(pc_,right_off,8,DSP_DIRECTOUT_MIX_RIGHT));
}

static
bool
dsp_fast_mixer8x2_match(uint32_t pc_)
{
  return (dsp_fast_mixer8x2_match_gap(pc_,0) ||
          dsp_fast_mixer8x2_match_gap(pc_,1));
}

static
bool
dsp_fast_multiply_match(uint32_t pc_)
{
  ITAG_t src1;
  ITAG_t src2;
  ITAG_t dst;

  if(pc_ > (DSP_NMEM_EXEC_WORDS - 4))
    return false;

  if(!dsp_fast_multiply_insn(DSP.NMem[pc_]))
    return false;

  src1.raw = DSP.NMem[pc_ + 1];
  src2.raw = DSP.NMem[pc_ + 2];
  dst.raw  = DSP.NMem[pc_ + 3];

  return (dsp_fast_direct_source_operand(src1) &&
          dsp_fast_direct_source_operand(src2) &&
          dsp_fast_direct_dest_operand(dst));
}

static
bool
dsp_fast_subtract_match(uint32_t pc_)
{
  ITAG_t src1;
  ITAG_t src2;
  ITAG_t dst;

  if(pc_ > (DSP_NMEM_EXEC_WORDS - 4))
    return false;

  if(DSP.NMem[pc_] != DSP_SUBTRACT_INSN)
    return false;

  src1.raw = DSP.NMem[pc_ + 1];
  src2.raw = DSP.NMem[pc_ + 2];
  dst.raw  = DSP.NMem[pc_ + 3];

  return (dsp_fast_direct_source_operand(src1) &&
          dsp_fast_direct_source_operand(src2) &&
          dsp_fast_direct_dest_operand(dst));
}

static
bool
dsp_fast_directout_match(uint32_t pc_)
{
  ITAG_t mix_left;
  ITAG_t in_left;
  ITAG_t mix_right;
  ITAG_t in_right;

  if(pc_ > (DSP_NMEM_EXEC_WORDS - 6))
    return false;

  if((DSP.NMem[pc_ + 0] != DSP_DIRECTOUT_INSN) ||
     (DSP.NMem[pc_ + 3] != DSP_DIRECTOUT_INSN))
    return false;

  mix_left.raw  = DSP.NMem[pc_ + 1];
  in_left.raw   = DSP.NMem[pc_ + 2];
  mix_right.raw = DSP.NMem[pc_ + 4];
  in_right.raw  = DSP.NMem[pc_ + 5];

  if(!dsp_fast_direct_operand(mix_left) ||
     !dsp_fast_direct_source_operand(in_left) ||
     !dsp_fast_direct_operand(mix_right) ||
     !dsp_fast_direct_source_operand(in_right))
    return false;

  return ((mix_left.nrof.OP_ADDR == DSP_DIRECTOUT_MIX_LEFT) &&
          (mix_left.nrof.WB1 != 0) &&
          (mix_right.nrof.OP_ADDR == DSP_DIRECTOUT_MIX_RIGHT) &&
          (mix_right.nrof.WB1 != 0));
}

static
void
dsp_fast_rebuild(void)
{
  uint32_t pc;

  memset(DSP_FAST_TABLE,0,sizeof(DSP_FAST_TABLE));

  if(dsp_fast_enabled())
    for(pc = 0; pc < DSP_NMEM_EXEC_WORDS; pc++)
      {
        if(dsp_fast_adpcmduck22s_158_match(pc))
          DSP_FAST_TABLE[pc] = dsp_fast_adpcmduck22s_158;
        else if(dsp_fast_adpcmduck22s_242_match(pc))
          DSP_FAST_TABLE[pc] = dsp_fast_adpcmduck22s_242;
        else if(dsp_fast_adpcmduck22s_250_match(pc))
          DSP_FAST_TABLE[pc] = dsp_fast_adpcmduck22s_250;
        else if(dsp_fast_adpcmhalfmono_71_374_match(pc))
          DSP_FAST_TABLE[pc] = dsp_fast_adpcmhalfmono_71_374;
        else if(dsp_fast_adpcmhalfmono_71_3cd_match(pc))
          DSP_FAST_TABLE[pc] = dsp_fast_adpcmhalfmono_71_3cd;
        else if(dsp_fast_adpcmmono_51_match(pc))
          DSP_FAST_TABLE[pc] = dsp_fast_adpcmmono_51;
        else if(dsp_fast_adpcmvarmono_70_match(pc))
          DSP_FAST_TABLE[pc] = dsp_fast_adpcmvarmono_70;
        else if(dsp_fast_adpcmvarmono_block_92_match(pc))
          DSP_FAST_TABLE[pc] = dsp_fast_adpcmvarmono_block_92;
        else if(dsp_fast_adpcmvarstereo_77_match(pc))
          DSP_FAST_TABLE[pc] = dsp_fast_adpcmvarstereo_77;
        else if(dsp_fast_benchmark_6_match(pc))
          DSP_FAST_TABLE[pc] = dsp_fast_benchmark_6;
        else if(dsp_fast_dcsqxdhalfmono_59_match(pc))
          DSP_FAST_TABLE[pc] = dsp_fast_dcsqxdhalfmono_59;
        else if(dsp_fast_dcsqxdhalfmono_62_match(pc))
          DSP_FAST_TABLE[pc] = dsp_fast_dcsqxdhalfmono_62;
        else if(dsp_fast_dcsqxdhalfstereo_92_match(pc))
          DSP_FAST_TABLE[pc] = dsp_fast_dcsqxdhalfstereo_92;
        else if(dsp_fast_dcsqxdmono_42_match(pc))
          DSP_FAST_TABLE[pc] = dsp_fast_dcsqxdmono_42;
        else if(dsp_fast_dcsqxdstereo_64_match(pc))
          DSP_FAST_TABLE[pc] = dsp_fast_dcsqxdstereo_64;
        else if(dsp_fast_dcsqxdstereo_61_match(pc))
          DSP_FAST_TABLE[pc] = dsp_fast_dcsqxdstereo_61;
        else if(dsp_fast_dcsqxdvarmono_56_match(pc))
          DSP_FAST_TABLE[pc] = dsp_fast_dcsqxdvarmono_56;
        else if(dsp_fast_directout_match(pc))
          DSP_FAST_TABLE[pc] = dsp_fast_directout;
        else if(dsp_fast_add_match(pc))
          DSP_FAST_TABLE[pc] = dsp_fast_add;
        else if(dsp_fast_dsppdhdr_match(pc))
          DSP_FAST_TABLE[pc] = dsp_fast_dsppdhdr;
        else if(dsp_fast_mixer8x2_match(pc))
          DSP_FAST_TABLE[pc] = dsp_fast_mixer8x2;
        else if(dsp_fast_mixer4x2_match(pc))
          DSP_FAST_TABLE[pc] = dsp_fast_mixer4x2;
        else if(dsp_fast_mixer2x2_match(pc))
          DSP_FAST_TABLE[pc] = dsp_fast_mixer2x2;
        else if(dsp_fast_multiply_match(pc))
          DSP_FAST_TABLE[pc] = dsp_fast_multiply;
        else if(dsp_fast_subtract_match(pc))
          DSP_FAST_TABLE[pc] = dsp_fast_subtract;
      }

  DSP_FAST_DIRTY = false;
}

static
void
dsp_fast_add_clip_write_direct(uint16_t         insn_,
                               uint32_t         dst_addr_,
                               uint32_t         src1_addr_,
                               uint32_t         src2_addr_,
                               uint32_t        *Y_,
                               dsp_alu_flags_t *flags_,
                               int             *fExact_)
{
  uint32_t a;
  uint32_t b;
  uint32_t y;

  DSP.flags.req.raw    = DSP.INSTTRAS[insn_].req.raw;
  DSP.flags.BS         = DSP.INSTTRAS[insn_].BS;
  DSP.flags.WRITEBACK  = dst_addr_;
  DSP.flags.ALU1       = dsp_read(src1_addr_);
  DSP.flags.ALU2       = dsp_read(src2_addr_);

  a = ((uint32_t)(uint16_t)DSP.flags.ALU1 << 16);
  b = ((uint32_t)(uint16_t)DSP.flags.ALU2 << 16);
  y = (a + b);

  flags_->carry    = ADD_CFLAG(a,b,y);
  flags_->overflow = ADD_VFLAG(a,b,y);
  flags_->zero     = ((y & 0xFFFF0000) ? 0 : 1);
  flags_->negative = ((y >> 31) ? 1 : 0);
  *fExact_         = ((y & 0x0000F000) ? 0 : 1);

  if(1 & flags_->overflow)
    y = (flags_->negative ? 0x7FFFF000 : 0x80000000);

  *Y_ = y;
  dsp_write(DSP.flags.WRITEBACK,((int32_t)y) >> 16);
}

static
void
dsp_fast_add_clip_write(uint16_t         dst_op_,
                        uint16_t         src_op_,
                        uint32_t        *Y_,
                        dsp_alu_flags_t *flags_,
                        int             *fExact_)
{
  ITAG_t dst;
  ITAG_t src;

  dst.raw = dst_op_;
  src.raw = src_op_;

  dsp_fast_add_clip_write_direct(DSP_DIRECTOUT_INSN,
                                 dst.nrof.OP_ADDR,
                                 dst.nrof.OP_ADDR,
                                 src.nrof.OP_ADDR,
                                 Y_,flags_,fExact_);
}

static
bool
dsp_fast_add(uint32_t        *Y_,
             dsp_alu_flags_t *flags_,
             int             *fExact_,
             uint32_t        *RBSR_,
             bool            *work_)
{
  uint32_t pc;
  ITAG_t src1;
  ITAG_t src2;
  ITAG_t dst;

  if(DSP.flags.nOP_MASK != 0xFFFF)
    return false;

  pc = DSP.dregs.PC;
  if(!dsp_fast_add_match(pc))
    return false;

  src1.raw = DSP.NMem[pc + 1];
  src2.raw = DSP.NMem[pc + 2];
  dst.raw  = DSP.NMem[pc + 3];

  dsp_fast_add_clip_write_direct(DSP_ADD_INSN,
                                 dst.nrof.OP_ADDR,
                                 src1.nrof.OP_ADDR,
                                 src2.nrof.OP_ADDR,
                                 Y_,flags_,fExact_);

  DSP.dregs.PC = pc + 4;

  return true;
}

static
void
dsp_fast_set_product_flags(uint32_t const  y_,
                           dsp_alu_flags_t *flags_,
                           int             *fExact_)
{
  flags_->carry    = 0;
  flags_->overflow = 0;
  flags_->zero     = ((y_ & 0xFFFF0000) ? 0 : 1);
  flags_->negative = ((y_ >> 31) ? 1 : 0);
  *fExact_         = ((y_ & 0x0000F000) ? 0 : 1);
}

static
uint32_t
dsp_fast_multiply_product_value(int16_t const src1_,
                                int16_t const src2_)
{
  return (uint32_t)(((int64_t)src1_ * (int64_t)src2_ * 2) & ALUSIZEMASK);
}

static
void
dsp_fast_multiply_product_to_y(uint16_t         insn_,
                               uint16_t         src1_op_,
                               uint16_t         src2_op_,
                               uint32_t        *Y_,
                               dsp_alu_flags_t *flags_,
                               int             *fExact_)
{
  ITAG_t src1;
  ITAG_t src2;

  src1.raw = src1_op_;
  src2.raw = src2_op_;

  DSP.flags.req.raw    = DSP.INSTTRAS[insn_].req.raw;
  DSP.flags.BS         = DSP.INSTTRAS[insn_].BS;
  DSP.flags.WRITEBACK  = 0;
  DSP.flags.MULT1      = dsp_read(src1.nrof.OP_ADDR);
  DSP.flags.MULT2      = dsp_read(src2.nrof.OP_ADDR);

  *Y_ = dsp_fast_multiply_product_value(DSP.flags.MULT1,DSP.flags.MULT2);
  dsp_fast_set_product_flags(*Y_,flags_,fExact_);
}

static
void
dsp_fast_multiply_accumulate_y_clip(uint16_t         insn_,
                                    uint16_t         src1_op_,
                                    uint16_t         src2_op_,
                                    uint32_t        *Y_,
                                    dsp_alu_flags_t *flags_,
                                    int             *fExact_)
{
  ITAG_t src1;
  ITAG_t src2;
  uint32_t a;
  uint32_t b;
  uint32_t y;

  src1.raw = src1_op_;
  src2.raw = src2_op_;

  DSP.flags.req.raw    = DSP.INSTTRAS[insn_].req.raw;
  DSP.flags.BS         = DSP.INSTTRAS[insn_].BS;
  DSP.flags.WRITEBACK  = 0;
  DSP.flags.MULT1      = dsp_read(src1.nrof.OP_ADDR);
  DSP.flags.MULT2      = dsp_read(src2.nrof.OP_ADDR);

  a = dsp_fast_multiply_product_value(DSP.flags.MULT1,DSP.flags.MULT2);
  b = *Y_;
  y = (a + b);

  flags_->carry    = ADD_CFLAG(a,b,y);
  flags_->overflow = ADD_VFLAG(a,b,y);
  flags_->zero     = ((y & 0xFFFF0000) ? 0 : 1);
  flags_->negative = ((y >> 31) ? 1 : 0);
  *fExact_         = ((y & 0x0000F000) ? 0 : 1);

  if(1 & flags_->overflow)
    y = (flags_->negative ? 0x7FFFF000 : 0x80000000);

  *Y_ = y;
}

static
void
dsp_fast_add_y_to_mix_clip(uint16_t         insn_,
                           uint16_t         mix_op_,
                           uint16_t         accum_op_,
                           uint32_t        *Y_,
                           dsp_alu_flags_t *flags_,
                           int             *fExact_)
{
  ITAG_t mix;
  ITAG_t accum;
  uint32_t a;
  uint32_t b;
  uint32_t y;

  mix.raw   = mix_op_;
  accum.raw = accum_op_;

  DSP.flags.req.raw    = DSP.INSTTRAS[insn_].req.raw;
  DSP.flags.BS         = DSP.INSTTRAS[insn_].BS;
  DSP.flags.WRITEBACK  = mix.nrof.OP_ADDR;
  DSP.flags.ALU1       = dsp_read(mix.nrof.OP_ADDR);
  (void)dsp_read(accum.nrof.OP_ADDR);

  a = ((uint32_t)(uint16_t)DSP.flags.ALU1 << 16);
  b = *Y_;
  y = (a + b);

  flags_->carry    = ADD_CFLAG(a,b,y);
  flags_->overflow = ADD_VFLAG(a,b,y);
  flags_->zero     = ((y & 0xFFFF0000) ? 0 : 1);
  flags_->negative = ((y >> 31) ? 1 : 0);
  *fExact_         = ((y & 0x0000F000) ? 0 : 1);

  if(1 & flags_->overflow)
    y = (flags_->negative ? 0x7FFFF000 : 0x80000000);

  *Y_ = y;
  dsp_write(DSP.flags.WRITEBACK,((int32_t)y) >> 16);
}

static
void
dsp_fast_subtract_clip_write_direct(uint16_t         insn_,
                                    uint32_t         dst_addr_,
                                    uint32_t         src1_addr_,
                                    uint32_t         src2_addr_,
                                    uint32_t        *Y_,
                                    dsp_alu_flags_t *flags_,
                                    int             *fExact_)
{
  uint32_t a;
  uint32_t b;
  uint32_t y;

  DSP.flags.req.raw    = DSP.INSTTRAS[insn_].req.raw;
  DSP.flags.BS         = DSP.INSTTRAS[insn_].BS;
  DSP.flags.WRITEBACK  = dst_addr_;
  DSP.flags.ALU1       = dsp_read(src1_addr_);
  DSP.flags.ALU2       = dsp_read(src2_addr_);

  a = ((uint32_t)(uint16_t)DSP.flags.ALU1 << 16);
  b = ((uint32_t)(uint16_t)DSP.flags.ALU2 << 16);
  y = (a - b);

  flags_->carry    = SUB_CFLAG(a,b,y);
  flags_->overflow = SUB_VFLAG(a,b,y);
  flags_->zero     = ((y & 0xFFFF0000) ? 0 : 1);
  flags_->negative = ((y >> 31) ? 1 : 0);
  *fExact_         = ((y & 0x0000F000) ? 0 : 1);

  if(1 & flags_->overflow)
    y = (flags_->negative ? 0x7FFFF000 : 0x80000000);

  *Y_ = y;
  dsp_write(DSP.flags.WRITEBACK,((int32_t)y) >> 16);
}

static
bool
dsp_fast_subtract(uint32_t        *Y_,
                  dsp_alu_flags_t *flags_,
                  int             *fExact_,
                  uint32_t        *RBSR_,
                  bool            *work_)
{
  uint32_t pc;
  ITAG_t src1;
  ITAG_t src2;
  ITAG_t dst;

  if(DSP.flags.nOP_MASK != 0xFFFF)
    return false;

  pc = DSP.dregs.PC;
  if(!dsp_fast_subtract_match(pc))
    return false;

  src1.raw = DSP.NMem[pc + 1];
  src2.raw = DSP.NMem[pc + 2];
  dst.raw  = DSP.NMem[pc + 3];

  dsp_fast_subtract_clip_write_direct(DSP_SUBTRACT_INSN,
                                      dst.nrof.OP_ADDR,
                                      src1.nrof.OP_ADDR,
                                      src2.nrof.OP_ADDR,
                                      Y_,flags_,fExact_);

  DSP.dregs.PC = pc + 4;

  return true;
}

static
void
dsp_fast_multiply_write_direct(uint16_t         insn_,
                               uint32_t         dst_addr_,
                               uint32_t         src1_addr_,
                               uint32_t         src2_addr_,
                               uint32_t        *Y_,
                               dsp_alu_flags_t *flags_,
                               int             *fExact_)
{
  uint32_t y;

  DSP.flags.req.raw    = DSP.INSTTRAS[insn_].req.raw;
  DSP.flags.BS         = DSP.INSTTRAS[insn_].BS;
  DSP.flags.WRITEBACK  = dst_addr_;
  DSP.flags.MULT1      = dsp_read(src1_addr_);
  DSP.flags.MULT2      = dsp_read(src2_addr_);

  y = dsp_fast_multiply_product_value(DSP.flags.MULT1,DSP.flags.MULT2);
  dsp_fast_set_product_flags(y,flags_,fExact_);

  *Y_ = y;
  dsp_write(DSP.flags.WRITEBACK,((int32_t)y) >> 16);
}

static
bool
dsp_fast_multiply(uint32_t        *Y_,
                  dsp_alu_flags_t *flags_,
                  int             *fExact_,
                  uint32_t        *RBSR_,
                  bool            *work_)
{
  uint32_t pc;
  ITAG_t src1;
  ITAG_t src2;
  ITAG_t dst;

  if(DSP.flags.nOP_MASK != 0xFFFF)
    return false;

  pc = DSP.dregs.PC;
  if(!dsp_fast_multiply_match(pc))
    return false;

  src1.raw = DSP.NMem[pc + 1];
  src2.raw = DSP.NMem[pc + 2];
  dst.raw  = DSP.NMem[pc + 3];

  dsp_fast_multiply_write_direct(DSP.NMem[pc],
                                 dst.nrof.OP_ADDR,
                                 src1.nrof.OP_ADDR,
                                 src2.nrof.OP_ADDR,
                                 Y_,flags_,fExact_);

  DSP.dregs.PC = pc + 4;

  return true;
}

static
void
dsp_fast_mixer_channel(uint32_t const   pc_,
                       uint32_t const   off_,
                       uint32_t const   terms_,
                       uint32_t        *Y_,
                       dsp_alu_flags_t *flags_,
                       int             *fExact_)
{
  uint32_t term;

  dsp_fast_multiply_product_to_y(DSP.NMem[pc_ + off_],
                                 DSP.NMem[pc_ + off_ + 1],
                                 DSP.NMem[pc_ + off_ + 2],
                                 Y_,flags_,fExact_);

  for(term = 1; term < terms_; term++)
    dsp_fast_multiply_accumulate_y_clip(DSP.NMem[pc_ + off_ + (term * 3)],
                                        DSP.NMem[pc_ + off_ + (term * 3) + 1],
                                        DSP.NMem[pc_ + off_ + (term * 3) + 2],
                                        Y_,flags_,fExact_);

  dsp_fast_add_y_to_mix_clip(DSP.NMem[pc_ + off_ + (terms_ * 3)],
                             DSP.NMem[pc_ + off_ + (terms_ * 3) + 1],
                             DSP.NMem[pc_ + off_ + (terms_ * 3) + 2],
                             Y_,flags_,fExact_);
}

static
bool
dsp_fast_mixer2x2(uint32_t        *Y_,
                  dsp_alu_flags_t *flags_,
                  int             *fExact_,
                  uint32_t        *RBSR_,
                  bool            *work_)
{
  uint32_t pc;

  if(DSP.flags.nOP_MASK != 0xFFFF)
    return false;

  pc = DSP.dregs.PC;
  if(!dsp_fast_mixer2x2_match(pc))
    return false;

  dsp_fast_mixer_channel(pc,0,2,Y_,flags_,fExact_);
  dsp_fast_mixer_channel(pc,9,2,Y_,flags_,fExact_);

  DSP.dregs.PC = pc + DSP_MIXER2X2_WORDS;

  return true;
}

static
bool
dsp_fast_mixer4x2(uint32_t        *Y_,
                  dsp_alu_flags_t *flags_,
                  int             *fExact_,
                  uint32_t        *RBSR_,
                  bool            *work_)
{
  uint32_t pc;

  if(DSP.flags.nOP_MASK != 0xFFFF)
    return false;

  pc = DSP.dregs.PC;
  if(!dsp_fast_mixer4x2_match(pc))
    return false;

  dsp_fast_mixer_channel(pc,0,4,Y_,flags_,fExact_);
  dsp_fast_mixer_channel(pc,15,4,Y_,flags_,fExact_);

  DSP.dregs.PC = pc + DSP_MIXER4X2_WORDS;

  return true;
}

static
bool
dsp_fast_mixer8x2(uint32_t        *Y_,
                  dsp_alu_flags_t *flags_,
                  int             *fExact_,
                  uint32_t        *RBSR_,
                  bool            *work_)
{
  uint32_t gap;
  uint32_t pc;

  if(DSP.flags.nOP_MASK != 0xFFFF)
    return false;

  pc = DSP.dregs.PC;
  if(dsp_fast_mixer8x2_match_gap(pc,0))
    gap = 0;
  else if(dsp_fast_mixer8x2_match_gap(pc,1))
    gap = 1;
  else
    return false;

  dsp_fast_mixer_channel(pc,0,8,Y_,flags_,fExact_);
  dsp_fast_mixer_channel(pc,27 + gap,8,Y_,flags_,fExact_);

  DSP.dregs.PC = pc + DSP_MIXER8X2_WORDS + gap;

  return true;
}

static
bool
dsp_fast_adpcmduck22s_158(uint32_t        *Y_,
                          dsp_alu_flags_t *flags_,
                          int             *fExact_,
                          uint32_t        *RBSR_,
                          bool            *work_)
{
  uint32_t base;
  uint32_t pc;

  if(DSP.flags.nOP_MASK != 0xFFFF)
    return false;

  pc = DSP.dregs.PC;
  if(!dsp_fast_adpcmduck22s_158_base_for_pc(pc,&base))
    return false;

  return dsp_fast_interpret_block(base,base + DSP_ADPCMDUCK22S_158_WORDS,
                                  Y_,flags_,fExact_,RBSR_,work_);
}

static
bool
dsp_fast_adpcmduck22s_242(uint32_t        *Y_,
                          dsp_alu_flags_t *flags_,
                          int             *fExact_,
                          uint32_t        *RBSR_,
                          bool            *work_)
{
  uint32_t base;
  uint32_t pc;

  if(DSP.flags.nOP_MASK != 0xFFFF)
    return false;

  pc = DSP.dregs.PC;
  if(!dsp_fast_adpcmduck22s_242_base_for_pc(pc,&base))
    return false;

  return dsp_fast_interpret_block(base,base + DSP_ADPCMDUCK22S_242_WORDS,
                                  Y_,flags_,fExact_,RBSR_,work_);
}

static
bool
dsp_fast_adpcmduck22s_250(uint32_t        *Y_,
                          dsp_alu_flags_t *flags_,
                          int             *fExact_,
                          uint32_t        *RBSR_,
                          bool            *work_)
{
  uint32_t base;
  uint32_t pc;

  if(DSP.flags.nOP_MASK != 0xFFFF)
    return false;

  pc = DSP.dregs.PC;
  if(!dsp_fast_adpcmduck22s_250_base_for_pc(pc,&base))
    return false;

  return dsp_fast_interpret_block(base,base + DSP_ADPCMDUCK22S_250_WORDS,
                                  Y_,flags_,fExact_,RBSR_,work_);
}

static
bool
dsp_fast_adpcmhalfmono_71_374(uint32_t        *Y_,
                              dsp_alu_flags_t *flags_,
                              int             *fExact_,
                              uint32_t        *RBSR_,
                              bool            *work_)
{
  uint32_t base;
  uint32_t pc;

  if(DSP.flags.nOP_MASK != 0xFFFF)
    return false;

  pc = DSP.dregs.PC;
  if(!dsp_fast_adpcmhalfmono_71_374_base_for_pc(pc,&base))
    return false;

  return dsp_fast_interpret_block(base,base + DSP_ADPCMHALFMONO_71_WORDS,
                                  Y_,flags_,fExact_,RBSR_,work_);
}

static
bool
dsp_fast_adpcmhalfmono_71_3cd(uint32_t        *Y_,
                              dsp_alu_flags_t *flags_,
                              int             *fExact_,
                              uint32_t        *RBSR_,
                              bool            *work_)
{
  uint32_t base;
  uint32_t pc;

  if(DSP.flags.nOP_MASK != 0xFFFF)
    return false;

  pc = DSP.dregs.PC;
  if(!dsp_fast_adpcmhalfmono_71_3cd_base_for_pc(pc,&base))
    return false;

  return dsp_fast_interpret_block(base,base + DSP_ADPCMHALFMONO_71_WORDS,
                                  Y_,flags_,fExact_,RBSR_,work_);
}

static
bool
dsp_fast_adpcmmono_51(uint32_t        *Y_,
                      dsp_alu_flags_t *flags_,
                      int             *fExact_,
                      uint32_t        *RBSR_,
                      bool            *work_)
{
  uint32_t base;
  uint32_t pc;

  if(DSP.flags.nOP_MASK != 0xFFFF)
    return false;

  pc = DSP.dregs.PC;
  if(!dsp_fast_adpcmmono_51_base_for_pc(pc,&base))
    return false;

  return dsp_fast_interpret_block(base,base + DSP_ADPCMMONO_51_WORDS,
                                  Y_,flags_,fExact_,RBSR_,work_);
}

static
bool
dsp_fast_adpcmvarmono_70(uint32_t        *Y_,
                         dsp_alu_flags_t *flags_,
                         int             *fExact_,
                         uint32_t        *RBSR_,
                         bool            *work_)
{
  uint32_t base;
  uint32_t pc;

  if(DSP.flags.nOP_MASK != 0xFFFF)
    return false;

  pc = DSP.dregs.PC;
  if(!dsp_fast_adpcmvarmono_70_base_for_pc(pc,&base))
    return false;

  return dsp_fast_interpret_block(base,base + DSP_ADPCMVARMONO_70_WORDS,
                                  Y_,flags_,fExact_,RBSR_,work_);
}

static
bool
dsp_fast_adpcmvarmono_block_92(uint32_t        *Y_,
                               dsp_alu_flags_t *flags_,
                               int             *fExact_,
                               uint32_t        *RBSR_,
                               bool            *work_)
{
  uint32_t base;
  uint32_t pc;

  if(DSP.flags.nOP_MASK != 0xFFFF)
    return false;

  pc = DSP.dregs.PC;
  if(!dsp_fast_adpcmvarmono_block_92_base_for_pc(pc,&base))
    return false;

  return dsp_fast_interpret_block(base,base + DSP_ADPCMVARMONO_BLOCK_92_WORDS,
                                  Y_,flags_,fExact_,RBSR_,work_);
}

static
bool
dsp_fast_adpcmvarstereo_77(uint32_t        *Y_,
                           dsp_alu_flags_t *flags_,
                           int             *fExact_,
                           uint32_t        *RBSR_,
                           bool            *work_)
{
  uint32_t base;
  uint32_t pc;

  if(DSP.flags.nOP_MASK != 0xFFFF)
    return false;

  pc = DSP.dregs.PC;
  if(!dsp_fast_adpcmvarstereo_77_base_for_pc(pc,&base))
    return false;

  return dsp_fast_interpret_block(base,base + DSP_ADPCMVARSTEREO_77_WORDS,
                                  Y_,flags_,fExact_,RBSR_,work_);
}

static
bool
dsp_fast_benchmark_6(uint32_t        *Y_,
                     dsp_alu_flags_t *flags_,
                     int             *fExact_,
                     uint32_t        *RBSR_,
                     bool            *work_)
{
  ITAG_t dst;
  uint32_t pc;
  uint32_t a;
  uint32_t b;
  uint32_t y;

  if(DSP.flags.nOP_MASK != 0xFFFF)
    return false;

  pc = DSP.dregs.PC;
  if(!dsp_fast_benchmark_6_match(pc))
    return false;

  dst.raw = DSP.NMem[pc + 3];

  DSP.flags.req.raw    = DSP.INSTTRAS[DSP_BENCHMARK_6_INSN].req.raw;
  DSP.flags.BS         = DSP.INSTTRAS[DSP_BENCHMARK_6_INSN].BS;

  DSP.flags.WRITEBACK  = 0x104;
  DSP.flags.ALU1       = dsp_read(DSP.flags.WRITEBACK);
  DSP.flags.WRITEBACK  = 0x0EF;
  DSP.flags.ALU2       = dsp_read(DSP.flags.WRITEBACK);
  DSP.flags.WRITEBACK  = dst.nrof.OP_ADDR;
  DSP.dregs.PC         = pc + DSP_BENCHMARK_6_WORDS;
  (void)dsp_read(DSP.flags.WRITEBACK);

  a = ((uint32_t)(uint16_t)DSP.flags.ALU1 << 16);
  b = ((uint32_t)(uint16_t)DSP.flags.ALU2 << 16);
  y = (a - b);

  flags_->carry    = SUB_CFLAG(a,b,y);
  flags_->overflow = SUB_VFLAG(a,b,y);
  flags_->zero     = ((y & 0xFFFF0000) ? 0 : 1);
  flags_->negative = ((y >> 31) ? 1 : 0);
  *fExact_         = ((y & 0x0000F000) ? 0 : 1);

  *Y_ = y;
  if(DSP.flags.WRITEBACK)
    dsp_write(DSP.flags.WRITEBACK,((int32_t)y) >> 16);

  return true;
}

static
bool
dsp_fast_dcsqxdhalfmono_59(uint32_t        *Y_,
                           dsp_alu_flags_t *flags_,
                           int             *fExact_,
                           uint32_t        *RBSR_,
                           bool            *work_)
{
  uint32_t base;
  uint32_t pc;

  if(DSP.flags.nOP_MASK != 0xFFFF)
    return false;

  pc = DSP.dregs.PC;
  if(!dsp_fast_dcsqxdhalfmono_59_base_for_pc(pc,&base))
    return false;

  return dsp_fast_interpret_block(base,base + DSP_DCSQXDHALFMONO_59_WORDS,
                                  Y_,flags_,fExact_,RBSR_,work_);
}

static
bool
dsp_fast_dcsqxdhalfmono_62(uint32_t        *Y_,
                           dsp_alu_flags_t *flags_,
                           int             *fExact_,
                           uint32_t        *RBSR_,
                           bool            *work_)
{
  uint32_t base;
  uint32_t pc;

  if(DSP.flags.nOP_MASK != 0xFFFF)
    return false;

  pc = DSP.dregs.PC;
  if(!dsp_fast_dcsqxdhalfmono_62_base_for_pc(pc,&base))
    return false;

  return dsp_fast_interpret_block(base,base + DSP_DCSQXDHALFMONO_62_WORDS,
                                  Y_,flags_,fExact_,RBSR_,work_);
}

static
bool
dsp_fast_dcsqxdhalfstereo_92(uint32_t        *Y_,
                             dsp_alu_flags_t *flags_,
                             int             *fExact_,
                             uint32_t        *RBSR_,
                             bool            *work_)
{
  uint32_t base;
  uint32_t pc;

  if(DSP.flags.nOP_MASK != 0xFFFF)
    return false;

  pc = DSP.dregs.PC;
  if(!dsp_fast_dcsqxdhalfstereo_92_base_for_pc(pc,&base))
    return false;

  return dsp_fast_interpret_block(base,base + DSP_DCSQXDHALFSTEREO_92_WORDS,
                                  Y_,flags_,fExact_,RBSR_,work_);
}

static
bool
dsp_fast_dcsqxdmono_42(uint32_t        *Y_,
                       dsp_alu_flags_t *flags_,
                       int             *fExact_,
                       uint32_t        *RBSR_,
                       bool            *work_)
{
  uint32_t base;
  uint32_t pc;

  if(DSP.flags.nOP_MASK != 0xFFFF)
    return false;

  pc = DSP.dregs.PC;
  if(!dsp_fast_dcsqxdmono_42_base_for_pc(pc,&base))
    return false;

  return dsp_fast_interpret_block(base,base + DSP_DCSQXDMONO_42_WORDS,
                                  Y_,flags_,fExact_,RBSR_,work_);
}

static
bool
dsp_fast_dcsqxdstereo_61(uint32_t        *Y_,
                         dsp_alu_flags_t *flags_,
                         int             *fExact_,
                         uint32_t        *RBSR_,
                         bool            *work_)
{
  uint32_t base;
  uint32_t pc;

  if(DSP.flags.nOP_MASK != 0xFFFF)
    return false;

  pc = DSP.dregs.PC;
  if(!dsp_fast_dcsqxdstereo_61_base_for_pc(pc,&base))
    return false;

  return dsp_fast_interpret_block(base,base + DSP_DCSQXDSTEREO_61_WORDS,
                                  Y_,flags_,fExact_,RBSR_,work_);
}

static
bool
dsp_fast_dcsqxdstereo_64(uint32_t        *Y_,
                         dsp_alu_flags_t *flags_,
                         int             *fExact_,
                         uint32_t        *RBSR_,
                         bool            *work_)
{
  uint32_t base;
  uint32_t pc;

  if(DSP.flags.nOP_MASK != 0xFFFF)
    return false;

  pc = DSP.dregs.PC;
  if(!dsp_fast_dcsqxdstereo_64_base_for_pc(pc,&base))
    return false;

  return dsp_fast_interpret_block(base,base + DSP_DCSQXDSTEREO_64_WORDS,
                                  Y_,flags_,fExact_,RBSR_,work_);
}

static
bool
dsp_fast_dcsqxdvarmono_56(uint32_t        *Y_,
                          dsp_alu_flags_t *flags_,
                          int             *fExact_,
                          uint32_t        *RBSR_,
                          bool            *work_)
{
  uint32_t base;
  uint32_t pc;

  if(DSP.flags.nOP_MASK != 0xFFFF)
    return false;

  pc = DSP.dregs.PC;
  if(!dsp_fast_dcsqxdvarmono_56_base_for_pc(pc,&base))
    return false;

  return dsp_fast_interpret_block(base,base + DSP_DCSQXDVARMONO_56_WORDS,
                                  Y_,flags_,fExact_,RBSR_,work_);
}

static
bool
dsp_fast_dsppdhdr(uint32_t        *Y_,
                  dsp_alu_flags_t *flags_,
                  int             *fExact_,
                  uint32_t        *RBSR_,
                  bool            *work_)
{
  uint32_t base;
  uint32_t pc;

  if(DSP.flags.nOP_MASK != 0xFFFF)
    return false;

  pc = DSP.dregs.PC;
  if(!dsp_fast_dsppdhdr_base_for_pc(pc,&base))
    return false;

  return dsp_fast_interpret_block(base,base + DSP_DSPPDHDR_WORDS,
                                  Y_,flags_,fExact_,RBSR_,work_);
}

static
bool
dsp_fast_directout(uint32_t        *Y_,
                   dsp_alu_flags_t *flags_,
                   int             *fExact_,
                   uint32_t        *RBSR_,
                   bool            *work_)
{
  uint32_t pc;

  if(DSP.flags.nOP_MASK != 0xFFFF)
    return false;

  pc = DSP.dregs.PC;
  if(!dsp_fast_directout_match(pc))
    return false;

  dsp_fast_add_clip_write(DSP.NMem[pc + 1],DSP.NMem[pc + 2],Y_,flags_,fExact_);
  dsp_fast_add_clip_write(DSP.NMem[pc + 4],DSP.NMem[pc + 5],Y_,flags_,fExact_);

  DSP.dregs.PC = pc + 6;

  return true;
}

static
bool
dsp_fast_execute(uint32_t        *Y_,
                 dsp_alu_flags_t *flags_,
                 int             *fExact_,
                 uint32_t        *RBSR_,
                 bool            *work_)
{
  dsp_fast_handler_t handler;

  if(DSP_FAST_DIRTY)
    dsp_fast_rebuild();

  if(!dsp_fast_enabled() || (DSP.dregs.PC >= DSP_NMEM_EXEC_WORDS))
    return false;

  handler = DSP_FAST_TABLE[DSP.dregs.PC];
  if(handler == NULL)
    return false;

  if(handler(Y_,flags_,fExact_,RBSR_,work_))
    return true;

  return false;
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
    dsp_fast_invalidate();

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
  dsp_fast_invalidate();

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
    DSP.flags.BS = ops[idx++];

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

  /* ?? 8-CPU last, 4-DSP last, 2-CPU ACK, 1 DSP ACK ?? */
  DSP.dregs.Sema4Status = 0;

  for(i = 0; i < sizeof(DSP.NMem)/sizeof(DSP.NMem[0]); i++)
    DSP.NMem[i] = 0x8380; /* sleep */

  for(i = 0; i < 16; i++)
    DSP.CPUSupply[i] = 0;

  dsp_fast_invalidate();
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

static
void
dsp_interpret_step(uint32_t        *Y_,
                   dsp_alu_flags_t *flags_,
                   int             *fExact_,
                   uint32_t        *RBSR_,
                   bool            *work_)
{
  uint32_t AOP = 0;
  uint32_t BOP = 0;
  ITAG_t inst;

  inst.raw = DSP.NMem[DSP.dregs.PC++];
  if(inst.aif.PAD)
    { // Control instruction
      switch((inst.raw >> 7) & 0xFF)
        {
        case 0:         /* NOP TODO */
          break;
        case 1:         /* branch accum */
          DSP.dregs.PC = ((*Y_ >> 16) & 0x3FF);
          break;
        case 2:         /* set rbase */
          DSP.RBASEx4 = ((inst.cif.BCH_ADDR & 0x3F) << 2);
          break;
        case 3:         /* set rmap */
          DSP.REGi = (inst.cif.BCH_ADDR & 7);
          break;
        case 4:         /* RTS */
          DSP.dregs.PC = *RBSR_;
          break;
        case 5:         /* set op_mask */
          DSP.flags.nOP_MASK = ~(inst.cif.BCH_ADDR & 0x1F);
          break;
        case 6:         /* -not used2- ins */
          break;
        case 7:         /* sleep */
          *work_ = false;
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
          DSP.dregs.PC = inst.cif.BCH_ADDR;
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
          *RBSR_       = DSP.dregs.PC;
          DSP.dregs.PC = inst.cif.BCH_ADDR;
          break;
        case 24:
        case 25:
        case 26:
        case 27:
        case 28:
        case 29:
        case 30:
        case 31:
          /* branch only if was branched */
          DSP.dregs.PC = inst.cif.BCH_ADDR;
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

            op   = dsp_operand_load1();
            addr = DSP.REGCONV[DSP.REGi][inst.r2of.R1] ^ DSP.RBASEx4;
            if(inst.r2of.R1_DI)
              addr = dsp_read(addr);
            dsp_write(addr,op);
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

            op   = dsp_operand_load1();
            addr = inst.cif.BCH_ADDR;
            if(inst.nrof.DI)
              addr = dsp_read(addr);
            dsp_write(addr,op);
          }
          break;
        default: /* condition branch */
          if(1 & DSP.BRCONDTAB[inst.br.bits][*fExact_+((flags_->raw*0x10080402)>>24)])
            DSP.dregs.PC = inst.cif.BCH_ADDR;
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
                AOP = (flags_->carry ? ((int)DSP.flags.MULT1<<16) & ALUSIZEMASK : 0);
              else
                AOP = (((int)DSP.flags.MULT1 * (((int32_t)*Y_ >> 15) & ~1)) & ALUSIZEMASK);
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
          AOP = *Y_;
          break;
        case 2:
          AOP = (DSP.flags.ALU2 << 16);
          break;
        }

      /* ACSBU signal */
      if((inst.aif.ALU == 3) || (inst.aif.ALU == 5))
        {
          BOP = (flags_->carry << 16);
        }
      else
        {
          switch(inst.aif.MUXB)
            {
            case 0:
              BOP = *Y_;
              break;
            case 1:
              BOP = (DSP.flags.ALU1 << 16);
              break;
            case 2:
              BOP = (DSP.flags.ALU2 << 16);
              break;
            case 3:
              if(inst.aif.M2SEL == 0) // ACSBU == 0 here always
                BOP = (((int)DSP.flags.MULT1 * (((int32_t)*Y_ >> 15)) & ~1) & ALUSIZEMASK);
              else
                BOP = (((int)DSP.flags.MULT1 * (int)DSP.flags.MULT2 * 2) & ALUSIZEMASK);
              break;
            }
        }

      /* Any ALU op. change overflow and possible carry */
      flags_->carry    = 0;
      flags_->overflow = 0;
      switch(inst.aif.ALU)
        {
        case 0:
          *Y_ = AOP;
          break;
          //*
        case 1:
          *Y_ = (0 - BOP);
          flags_->carry    = SUB_CFLAG(0,BOP,*Y_);
          flags_->overflow = SUB_VFLAG(0,BOP,*Y_);
          break;
        case 2:
        case 3:
          *Y_ = (AOP + BOP);
          flags_->carry    = ADD_CFLAG(AOP,BOP,*Y_);
          flags_->overflow = ADD_VFLAG(AOP,BOP,*Y_);
          break;
        case 4:
        case 5:
          *Y_ = (AOP - BOP);
          flags_->carry    = SUB_CFLAG(AOP,BOP,*Y_);
          flags_->overflow = SUB_VFLAG(AOP,BOP,*Y_);
          break;
        case 6:
          *Y_ = (AOP + 0x1000);
          flags_->carry    = ADD_CFLAG(AOP,0x1000,*Y_);
          flags_->overflow = ADD_VFLAG(AOP,0x1000,*Y_);
          break;
        case 7:
          *Y_ = (AOP - 0x1000);
          flags_->carry    = SUB_CFLAG(AOP,0x1000,*Y_);
          flags_->overflow = SUB_VFLAG(AOP,0x1000,*Y_);
          break;
        case 8:
          *Y_ = AOP;
          break;
        case 9:
          *Y_ = (AOP ^ ALUSIZEMASK);
          break;
        case 10:
          *Y_ = (AOP & BOP);
          break;
        case 11:
          *Y_ = ((AOP & BOP) ^ ALUSIZEMASK);
          break;
        case 12:
          *Y_= (AOP | BOP);
          break;
        case 13:
          *Y_ = ((AOP | BOP) ^ ALUSIZEMASK);
          break;
        case 14:
          *Y_ = (AOP ^ BOP);
          break;
        case 15:
          *Y_ = (AOP ^ BOP ^ ALUSIZEMASK);
          break;
        }

      flags_->zero     = ((*Y_ & 0xFFFF0000) ? 0 : 1);
      flags_->negative = ((*Y_ >> 31) ? 1 : 0);
      *fExact_         = ((*Y_ & 0x0000F000) ? 0 : 1);

      //and BarrelShifter
      switch(DSP.flags.BS)
        {
        case 1:
        case 17:
          *Y_ = *Y_ << 1;
          break;
        case 2:
        case 18:
          *Y_ = *Y_ << 2;
          break;
        case 3:
        case 19:
          *Y_ = *Y_ << 3;
          break;
        case 4:
        case 20:
          *Y_ = *Y_ << 4;
          break;
        case 5:
        case 21:
          *Y_ = *Y_ << 5;
          break;
        case 6:
        case 22:
          *Y_ = *Y_ << 8;
          break;

          //arithmetic shifts
        case 9:
          *Y_  = ((int32_t)*Y_ >> 16);
          *Y_ &= ALUSIZEMASK;
          break;
        case 10:
          *Y_  = ((int32_t)*Y_ >> 8);
          *Y_ &= ALUSIZEMASK;
          break;
        case 11:
          *Y_  = ((int32_t)*Y_ >> 5);
          *Y_ &= ALUSIZEMASK;
          break;
        case 12:
          *Y_  = ((int32_t)*Y_ >> 4);
          *Y_ &= ALUSIZEMASK;
          break;
        case 13:
          *Y_  = ((int32_t)*Y_ >> 3);
          *Y_ &= ALUSIZEMASK;
          break;
        case 14:
          *Y_  = ((int32_t)*Y_ >> 2);
          *Y_ &= ALUSIZEMASK;
          break;
        case 15:
          *Y_  = ((int32_t)*Y_ >> 1);
          *Y_ &= ALUSIZEMASK;
          break;

          // logocal shift
        case 7:         // CLIP ari
        case 23:        // CLIP log
          if(1 & flags_->overflow)
            {
              if(1 & flags_->negative)
                *Y_ = 0x7FFFF000;
              else
                *Y_ = 0x80000000;
            }
          break;
        case 8:         // Load operand load sameself again (ari)
        case 24:        // same, but logicalshift
          {
            //int temp=flags.carry;
            flags_->carry = ((signed)*Y_ < 0); // shift out bit to Carry
            //Y=Y<<1;
            //Y|=temp<<16;
            *Y_ = (((*Y_ << 1) & 0xFFFE0000)   |
                   (flags_->carry ? 1<<16 : 0) |
                   (*Y_ & 0xF000));
          }
          break;
        case 25:
          *Y_  = ((uint32_t)*Y_ >> 16);
          *Y_ &= ALUSIZEMASK;
          break;
        case 26:
          *Y_  = ((uint32_t)*Y_ >> 8);
          *Y_ &= ALUSIZEMASK;
          break;
        case 27:
          *Y_  = ((uint32_t)*Y_ >> 5);
          *Y_ &= ALUSIZEMASK;
          break;
        case 28:
          *Y_  = ((uint32_t)*Y_ >> 4);
          *Y_ &= ALUSIZEMASK;
          break;
        case 29:
          *Y_  = ((uint32_t)*Y_ >> 3);
          *Y_ &= ALUSIZEMASK;
          break;
        case 30:
          *Y_  = ((uint32_t)*Y_ >> 2);
          *Y_ &= ALUSIZEMASK;
          break;
        case 31:
          *Y_  = ((uint32_t)*Y_ >> 1);
          *Y_ &= ALUSIZEMASK;
          break;
        }

      if(DSP.flags.WRITEBACK)
        dsp_write(DSP.flags.WRITEBACK,((int32_t)*Y_) >> 16);
    }
}

static
bool
dsp_fast_interpret_block(uint32_t         start_,
                         uint32_t         end_,
                         uint32_t        *Y_,
                         dsp_alu_flags_t *flags_,
                         int             *fExact_,
                         uint32_t        *RBSR_,
                         bool            *work_)
{
  if((DSP.dregs.PC < start_) || (DSP.dregs.PC >= end_))
    return false;

  do
    dsp_interpret_step(Y_,flags_,fExact_,RBSR_,work_);
  while(*work_ && (DSP.dregs.PC >= start_) && (DSP.dregs.PC < end_));

  return true;
}

uint32_t
opera_dsp_loop(void)
{
  uint32_t Y;	/* accumulator */
  uint32_t BOP; /* 1st & 2nd operand */
  dsp_alu_flags_t flags;

  if(DSP.flags.Running)
    {
      uint32_t AOP    = 0;      /* 1st operand */
      uint32_t RBSR   = 0;	/* return address */
      int      fExact = 0;
      bool     work   = true;

      opera_dsp_reset();

      Y   = 0;
      BOP = 0;
      flags.raw = 0;

      do
        {
          ITAG_t inst;

          if(dsp_fast_execute(&Y,&flags,&fExact,&RBSR,&work))
            continue;

          inst.raw = DSP.NMem[DSP.dregs.PC++];
          if(inst.aif.PAD)
            { // Control instruction
              switch((inst.raw >> 7) & 0xFF)
                {
                case 0:         /* NOP TODO */
                  break;
                case 1:         /* branch accum */
                  DSP.dregs.PC = ((Y >> 16) & 0x3FF);
                  break;
                case 2:         /* set rbase */
                  DSP.RBASEx4 = ((inst.cif.BCH_ADDR & 0x3F) << 2);
                  break;
                case 3:         /* set rmap */
                  DSP.REGi = (inst.cif.BCH_ADDR & 7);
                  break;
                case 4:         /* RTS */
                  DSP.dregs.PC = RBSR;
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
                  DSP.dregs.PC = inst.cif.BCH_ADDR;
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
                  RBSR         = DSP.dregs.PC;
                  DSP.dregs.PC = inst.cif.BCH_ADDR;
                  break;
                case 24:
                case 25:
                case 26:
                case 27:
                case 28:
                case 29:
                case 30:
                case 31:
                  /* branch only if was branched */
                  DSP.dregs.PC = inst.cif.BCH_ADDR;
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

                    op   = dsp_operand_load1();
                    addr = DSP.REGCONV[DSP.REGi][inst.r2of.R1] ^ DSP.RBASEx4;
                    if(inst.r2of.R1_DI)
                      addr = dsp_read(addr);
                    dsp_write(addr,op);
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

                    op   = dsp_operand_load1();
                    addr = inst.cif.BCH_ADDR;
                    if(inst.nrof.DI)
                      addr = dsp_read(addr);
                    dsp_write(addr,op);
                  }
                  break;
                default: /* condition branch */
                  if(1 & DSP.BRCONDTAB[inst.br.bits][fExact+((flags.raw*0x10080402)>>24)])
                    DSP.dregs.PC = inst.cif.BCH_ADDR;
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
                dsp_write(DSP.flags.WRITEBACK,((int32_t)Y) >> 16);
            }

        } while(work);


      if(1 & DSP.flags.GenFIQ)
        {
          DSP.flags.GenFIQ = false;
          opera_clio_fiq_generate(0x800,0); /* AudioFIQ */
        }

      DSP.dregs.DSPPCNT -= SYSTEM_TICKS;
      if(DSP.dregs.DSPPCNT <= 0)
        DSP.dregs.DSPPCNT += DSP.dregs.DSPPRLD;
    }

  return ((DSP.IMem[0x3FF] << 16) | DSP.IMem[0x3FE]);
}

/* CPU writes NMEM of DSP */
void
opera_dsp_mem_write(uint16_t addr_,
                     uint16_t val_)
{
  //mwriteh(addr,val);
  addr_ &= 0x3FF;
  if(DSP.NMem[addr_] != val_)
    {
      DSP.NMem[addr_] = val_;
      dsp_fast_invalidate();
    }
}

void
opera_dsp_set_running(int val_)
{
  DSP.flags.Running = (val_ & 1);
}

void
opera_dsp_set_fast(int val_)
{
  val_ = (val_ & 1);
  if(DSP_FAST_ENABLED == val_)
    return;

  DSP_FAST_ENABLED = val_;
  dsp_fast_invalidate();
}

/* CPU writes to EI,I of DSP */
void
opera_dsp_imem_write(uint16_t addr_,
                      uint16_t val_)
{
  if((addr_ >= 0x70) && (addr_ <= 0x7C))
    {
      DSP.CPUSupply[addr_ - 0x70] = 1;
      DSP.IMem[addr_ & 0x7F]      = val_;
    }
  else if(!(addr_ & 0x80))
    {
      DSP.IMem[addr_ & 0x7F] = val_;
    }
}

void
opera_dsp_arm_semaphore_write(uint32_t val_)
{
  // How about Sema4ACK? Now don't think about it
  // ARM write to Sema4Data low 16 bits
  // ARM be last
  DSP.dregs.Sema4Data   = (val_ & 0xFFFF);
  DSP.dregs.Sema4Status = 0x8;
}

/* CPU reads from EO,I of DSP */
uint16_t
opera_dsp_imem_read(uint16_t addr_)
{
  switch(addr_)
    {
    case 0x3EB:
      return DSP.dregs.AudioOutStatus;
    case 0x3EC:
      return DSP.dregs.Sema4Status;
    case 0x3ED:
      return DSP.dregs.Sema4Data;
    case 0x3EE:
      return DSP.dregs.INT;
    case 0x3EF:
      return DSP.dregs.DSPPRLD;
    default:
      break;
    }

  return DSP.IMem[addr_];
}

uint32_t
opera_dsp_arm_semaphore_read(void)
{
  return ((DSP.dregs.Sema4Status << 16) | DSP.dregs.Sema4Data);
}
