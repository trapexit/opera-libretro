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
#include "endianness.h"
#include "inline.h"

#include "opera_arm.h"
#include "opera_arm_core.h"
#include "opera_cdrom.h"
#include "opera_clio.h"
#include "opera_core.h"
#include "opera_diag_port.h"
#include "opera_fixedpoint_math.h"
#include "opera_3do.h"
#include "opera_dsp.h"
#include "opera_madam.h"
#include "opera_mem.h"
#include "opera_xbus.h"
#include "opera_sport.h"
#include "opera_state.h"
#include "opera_swi_hle_0x5XXXX.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
/* Optional block-compiling dynarec for the ARM60 core (engine id 3,
 * selected via the opera_arm_engine core option).  The implementation is
 * #included at the bottom of this file so it can share the file-static CPU
 * core, handlers, classifier and poll pointers.
 *
 * ENCODING BACKEND MATRIX: which (arch, OS) pairs get a jit at all is
 * decided in opera_arm_jit_backend.h (per-backend gates + the master
 * kill-switch OPERA_JIT_BACKENDS=0).  The matrix covers: x86-64 SysV
 * (shipped, gate-proven), x86-64 Win64, AArch64, Armv7.  When the
 * matrix selects nothing, the jit compiles out and the engine falls
 * back to cache/interp exactly like an unsupported host. */
#include "opera_arm_jit_backend.h"
#if defined(OPERA_JIT_HAVE_BACKEND)
#define OPERA_ARM_JIT_ENABLED 1
#endif

#ifdef OPERA_ARM_JIT_ENABLED
static int32_t arm_jit_exec_slice(int32_t budget_);
#endif
void opera_arm_jit_destroy(void);   /* real impl or no-op stub at EOF */


/*
  HACK
  This is not accurate. Real ARM60 would start at 0x00000000. The
  ROM is mapped to the bottom of the address space till any write
  happens and then it is swapped out for the DRAM. Instead of having
  that overhead ever memory access we just start in ROM.
*/
#define ARM_INITIAL_PC  0x03000000
#define ARM_ARRAY_COUNT(A_) ((uint32_t)(sizeof(A_) / sizeof((A_)[0])))

#define ARM_MUL_MASK    0x0fc000f0
#define ARM_MUL_SIGN    0x00000090
#define ARM_SDS_MASK    0x0fb00ff0
#define ARM_SDS_SIGN    0x01000090
#define ARM_UND_MASK    0x0e000010
#define ARM_UND_SIGN    0x06000010
#define ARM_MRS_MASK    0x0fbf0fff
#define ARM_MRS_SIGN    0x010f0000
#define ARM_MSR_MASK    0x0fbffff0
#define ARM_MSR_SIGN    0x0129f000
#define ARM_MSRF_MASK   0x0dbff000
#define ARM_MSRF_SIGN   0x0128f000

#define ARM_DP_IMM_MASK             0xFF
#define ARM_DP_IMM_ROT_AMOUNT_SHIFT 7
#define ARM_DP_IMM_ROT_AMOUNT_MASK  0x1E
#define ARM_SHIFT_TYPE_ROR          3

#define ARM_MODE_USER   0
#define ARM_MODE_FIQ    1
#define ARM_MODE_IRQ    2
#define ARM_MODE_SVC    3
#define ARM_MODE_ABT    4
#define ARM_MODE_UND    5
#define ARM_MODE_UNK    0xff

const static uint8_t arm_mode_table[]=
  {
    ARM_MODE_UNK,     ARM_MODE_UNK,     ARM_MODE_UNK,     ARM_MODE_UNK,
    ARM_MODE_UNK,     ARM_MODE_UNK,     ARM_MODE_UNK,     ARM_MODE_UNK,
    ARM_MODE_UNK,     ARM_MODE_UNK,     ARM_MODE_UNK,     ARM_MODE_UNK,
    ARM_MODE_UNK,     ARM_MODE_UNK,     ARM_MODE_UNK,     ARM_MODE_UNK,
    ARM_MODE_USER,    ARM_MODE_FIQ,     ARM_MODE_IRQ,     ARM_MODE_SVC,
    ARM_MODE_UNK,     ARM_MODE_UNK,     ARM_MODE_UNK,     ARM_MODE_ABT,
    ARM_MODE_UNK,     ARM_MODE_UNK,     ARM_MODE_UNK,     ARM_MODE_UND,
    ARM_MODE_UNK,     ARM_MODE_UNK,     ARM_MODE_UNK,     ARM_MODE_UNK
  };

/*
 * ARM60 instruction timings are expressed in N/S/I/C cycle classes rather
 * than fixed cycle counts.  The ARM60 datasheet defines the cycle classes and
 * notes that extra N-cycle length is a memory-system property, not an ARM60
 * requirement.  The ARM toolkit docs bundled with the 3DO SDK use N=2S in an
 * uncached ARM example, and 3DO hardware is documented as an uncached ARM60.
 *
 * References:
 * - https://github.com/trapexit/3do-devkit/blob/master/docs/cpu/arm60_datasheet_-_zarlink_semiconductor.pdf
 * - https://github.com/trapexit/3do-devkit/blob/master/docs/3dosdk/tktfldr/atsfldr/4atsd.html#L71-L78
 * - https://github.com/trapexit/portfolio_os/blob/master/src/audio/audiofolio/dspp_instr.c#L220-L245
 */
#define NCYCLE 2
#define SCYCLE 1
#define ICYCLE 1

//--------------------------Conditions-------------------------------------------
// flags - N Z C V  -  31...28
const static uint16_t cond_flags_cross[]=
  {
    0xf0f0, //EQ - Z set (equal)
    0x0f0f, //NE - Z clear (not equal)
    0xcccc, //CS - C set (unsigned higher or same)
    0x3333, //CC - C clear (unsigned lower)
    0xff00, //N set (negative)
    0x00ff, //N clear (positive or zero)
    0xaaaa, //V set (overflow)
    0x5555, //V clear (no overflow)
    0x0c0c, //C set and Z clear (unsigned higher)
    0xf3f3, //C clear or Z set (unsigned lower or same)
    0xaa55, //N set and V set, or N clear and V clear (greater or equal)
    0x55aa, //N set and V clear, or N clear and V set (less than)
    0x0a05, //Z clear, and either N set and V set, or N clear and V clear (greater than)
    0xf5fa, //Z set, or N set and V clear, or N clear and V set (less than or equal)
    0xffff, //always
    0x0000  //never
  };

static int        g_SWI_HLE;
static arm_core_t CPU;
static int        CYCLES;	//cycle counter
static bool       g_SOFT_RESET_PENDING = false;
static uint32_t   carry_out = 0;
static int        g_arm_engine = 1;
/* ARM engine selection: 0 = interpreter, 1 = cached, 3 = JIT dynarec
 * (2 is the internal cached-alloc-failed fallback marker) */
/* libretro core-option selection: -1 = unset (default cache), else 0/1/3 */
static int        g_arm_engine_opt = -1;

void
opera_arm_engine_opt_set(int engine_)
{
  g_arm_engine_opt = engine_;
}

static uint32_t readusr(uint32_t const rn);
static void     loadusr(uint32_t const rn, uint32_t const val);
static uint32_t mreadb(uint32_t const addr);
static void     mwriteb(uint32_t const addr, uint8_t const val);
static uint32_t mreadw(uint32_t const addr);
static void     mwritew(uint32_t const addr,uint32_t const val);
static int32_t arm_execute_interp(void);

/* one-time bound pointers for call-free per-instruction polling */
static const uint32_t *g_clio_regs;
static const uint32_t *g_clio_fiqpend;
static const uint32_t *g_madam_fsm_poll;
static const bool     *g_cdrom_restart_poll;

#define arm_fiq_pending_()  (*g_clio_fiqpend)
#define arm_madam_inprocess_()  (*g_madam_fsm_poll == FSM_INPROCESS)
#define arm_cdrom_restart_()    (*g_cdrom_restart_poll != false)

static
bool
clio_xbus_access_aborts(uint32_t const index_)
{
  if((index_ < 0x540) || (index_ >= 0x600))
    return false;

  if(!opera_xbus_selected_device_absent())
    return false;

  return true;
}

uint32_t
opera_arm_state_size_v1(void)
{
  return opera_state_save_size(sizeof(CPU));
}

static
bool
opera_arm_state_write_payload(opera_state_writer_t *writer_,
                              arm_core_t const     *state_)
{
  return (opera_state_write_u32_array(writer_,state_->USER,ARM_ARRAY_COUNT(state_->USER)) &&
          opera_state_write_u32_array(writer_,state_->CASH,ARM_ARRAY_COUNT(state_->CASH)) &&
          opera_state_write_u32_array(writer_,state_->SVC,ARM_ARRAY_COUNT(state_->SVC)) &&
          opera_state_write_u32_array(writer_,state_->ABT,ARM_ARRAY_COUNT(state_->ABT)) &&
          opera_state_write_u32_array(writer_,state_->FIQ,ARM_ARRAY_COUNT(state_->FIQ)) &&
          opera_state_write_u32_array(writer_,state_->IRQ,ARM_ARRAY_COUNT(state_->IRQ)) &&
          opera_state_write_u32_array(writer_,state_->UND,ARM_ARRAY_COUNT(state_->UND)) &&
          opera_state_write_u32_array(writer_,state_->SPSR,ARM_ARRAY_COUNT(state_->SPSR)) &&
          opera_state_write_u32(writer_,state_->CPSR) &&
          opera_state_write_u8(writer_,state_->nFIQ) &&
          opera_state_write_u8(writer_,state_->MAS_Access_Exept) &&
          opera_state_write_u8(writer_,g_SOFT_RESET_PENDING));
}

static
uint32_t
opera_arm_state_payload_size(void)
{
  opera_state_writer_t writer;

  opera_state_writer_init(&writer,NULL,UINT32_MAX);
  opera_arm_state_write_payload(&writer,&CPU);

  return opera_state_writer_used(&writer);
}

uint32_t
opera_arm_state_size(void)
{
  return opera_state_chunk_size(opera_arm_state_payload_size());
}

uint32_t
opera_arm_state_save(void *data_)
{
  uint32_t payload_size;
  opera_state_writer_t writer;

  payload_size = opera_arm_state_payload_size();
  opera_state_writer_init(&writer,data_,opera_state_chunk_size(payload_size));
  opera_state_write_chunk_header(&writer,"ARM",payload_size);
  opera_arm_state_write_payload(&writer,&CPU);

  return opera_state_writer_ok(&writer) ? opera_state_writer_used(&writer) : 0;
}

uint32_t
opera_arm_state_load_v1(void const     *data_,
                        uint32_t const  size_)
{
  uint32_t rv;

  rv = opera_state_load_sized(&CPU,"ARM",data_,size_,sizeof(CPU));
  if(rv != 0)
    g_SOFT_RESET_PENDING = false;

  return rv;
}

static
bool
opera_arm_state_read_payload(opera_state_reader_t *reader_,
                             arm_core_t           *state_,
                             bool                 *soft_reset_pending_)
{
  uint8_t soft_reset_pending;

  soft_reset_pending = false;

  return (opera_state_read_u32_array(reader_,state_->USER,ARM_ARRAY_COUNT(state_->USER)) &&
          opera_state_read_u32_array(reader_,state_->CASH,ARM_ARRAY_COUNT(state_->CASH)) &&
          opera_state_read_u32_array(reader_,state_->SVC,ARM_ARRAY_COUNT(state_->SVC)) &&
          opera_state_read_u32_array(reader_,state_->ABT,ARM_ARRAY_COUNT(state_->ABT)) &&
          opera_state_read_u32_array(reader_,state_->FIQ,ARM_ARRAY_COUNT(state_->FIQ)) &&
          opera_state_read_u32_array(reader_,state_->IRQ,ARM_ARRAY_COUNT(state_->IRQ)) &&
          opera_state_read_u32_array(reader_,state_->UND,ARM_ARRAY_COUNT(state_->UND)) &&
          opera_state_read_u32_array(reader_,state_->SPSR,ARM_ARRAY_COUNT(state_->SPSR)) &&
          opera_state_read_u32(reader_,&state_->CPSR) &&
          opera_state_read_u8(reader_,&state_->nFIQ) &&
          opera_state_read_u8(reader_,&state_->MAS_Access_Exept) &&
          opera_state_read_u8(reader_,&soft_reset_pending) &&
          ((*soft_reset_pending_ = (soft_reset_pending != 0)), true));
}

uint32_t
opera_arm_state_load(void const     *data_,
                     uint32_t const  size_)
{
  arm_core_t state;
  bool soft_reset_pending;
  opera_state_reader_t reader;
  opera_state_reader_t payload;

  opera_state_reader_init(&reader,data_,size_);
  if(!opera_state_read_chunk(&reader,"ARM",&payload) ||
     !opera_arm_state_read_payload(&payload,&state,&soft_reset_pending) ||
     !opera_state_reader_finished(&payload))
    return 0;

  CPU = state;
  g_SOFT_RESET_PENDING = soft_reset_pending;

  return opera_state_reader_used(&reader);
}

static
void
ARM_RestUserRONS(void)
{
  switch(arm_mode_table[CPU.CPSR & 0x1F])
    {
    case ARM_MODE_USER:
      break;
    case ARM_MODE_FIQ:
      memcpy(CPU.FIQ,&CPU.USER[8],7<<2);
      memcpy(&CPU.USER[8],CPU.CASH,7<<2);
      break;
    case ARM_MODE_IRQ:
      CPU.IRQ[0]   = CPU.USER[13];
      CPU.IRQ[1]   = CPU.USER[14];
      CPU.USER[13] = CPU.CASH[5];
      CPU.USER[14] = CPU.CASH[6];
      break;
    case ARM_MODE_SVC:
      CPU.SVC[0]   = CPU.USER[13];
      CPU.SVC[1]   = CPU.USER[14];
      CPU.USER[13] = CPU.CASH[5];
      CPU.USER[14] = CPU.CASH[6];
      break;
    case ARM_MODE_ABT:
      CPU.ABT[0]   = CPU.USER[13];
      CPU.ABT[1]   = CPU.USER[14];
      CPU.USER[13] = CPU.CASH[5];
      CPU.USER[14] = CPU.CASH[6];
      break;
    case ARM_MODE_UND:
      CPU.UND[0]   = CPU.USER[13];
      CPU.UND[1]   = CPU.USER[14];
      CPU.USER[13] = CPU.CASH[5];
      CPU.USER[14] = CPU.CASH[6];
      break;
    }
}

static
void
ARM_RestFiqRONS(void)
{
  switch(arm_mode_table[CPU.CPSR & 0x1F])
    {
    case ARM_MODE_USER:
      memcpy(CPU.CASH,&CPU.USER[8],7<<2);
      memcpy(&CPU.USER[8],CPU.FIQ,7<<2);
      break;
    case ARM_MODE_FIQ:
      break;
    case ARM_MODE_IRQ:
      memcpy(CPU.CASH,&CPU.USER[8],5<<2);
      CPU.IRQ[0] = CPU.USER[13];
      CPU.IRQ[1] = CPU.USER[14];
      memcpy(&CPU.USER[8],CPU.FIQ,7<<2);
      break;
    case ARM_MODE_SVC:
      memcpy(CPU.CASH,&CPU.USER[8],5<<2);
      CPU.SVC[0] = CPU.USER[13];
      CPU.SVC[1] = CPU.USER[14];
      memcpy(&CPU.USER[8],CPU.FIQ,7<<2);
      break;
    case ARM_MODE_ABT:
      memcpy(CPU.CASH,&CPU.USER[8],5<<2);
      CPU.ABT[0] = CPU.USER[13];
      CPU.ABT[1] = CPU.USER[14];
      memcpy(&CPU.USER[8],CPU.FIQ,7<<2);
      break;
    case ARM_MODE_UND:
      memcpy(CPU.CASH,&CPU.USER[8],5<<2);
      CPU.UND[0] = CPU.USER[13];
      CPU.UND[1] = CPU.USER[14];
      memcpy(&CPU.USER[8],CPU.FIQ,7<<2);
      break;
    }
}

static
void
ARM_RestIrqRONS(void)
{
  switch(arm_mode_table[CPU.CPSR & 0x1F])
    {
    case ARM_MODE_USER:
      CPU.CASH[5]  = CPU.USER[13];
      CPU.CASH[6]  = CPU.USER[14];
      CPU.USER[13] = CPU.IRQ[0];
      CPU.USER[14] = CPU.IRQ[1];
      break;
    case ARM_MODE_FIQ:
      memcpy(CPU.FIQ,&CPU.USER[8],7<<2);
      memcpy(&CPU.USER[8],CPU.CASH,5<<2);
      CPU.USER[13] = CPU.IRQ[0];
      CPU.USER[14] = CPU.IRQ[1];
      break;
    case ARM_MODE_IRQ:
      break;
    case ARM_MODE_SVC:
      CPU.SVC[0]   = CPU.USER[13];
      CPU.SVC[1]   = CPU.USER[14];
      CPU.USER[13] = CPU.IRQ[0];
      CPU.USER[14] = CPU.IRQ[1];
      break;
    case ARM_MODE_ABT:
      CPU.ABT[0]   = CPU.USER[13];
      CPU.ABT[1]   = CPU.USER[14];
      CPU.USER[13] = CPU.IRQ[0];
      CPU.USER[14] = CPU.IRQ[1];
      break;
    case ARM_MODE_UND:
      CPU.UND[0]   = CPU.USER[13];
      CPU.UND[1]   = CPU.USER[14];
      CPU.USER[13] = CPU.IRQ[0];
      CPU.USER[14] = CPU.IRQ[1];
      break;
    }
}

static
void
ARM_RestSvcRONS(void)
{
  switch(arm_mode_table[CPU.CPSR & 0x1F])
    {
    case ARM_MODE_USER:
      CPU.CASH[5]  = CPU.USER[13];
      CPU.CASH[6]  = CPU.USER[14];
      CPU.USER[13] = CPU.SVC[0];
      CPU.USER[14] = CPU.SVC[1];
      break;
    case ARM_MODE_FIQ:
      memcpy(CPU.FIQ,&CPU.USER[8],7<<2);
      memcpy(&CPU.USER[8],CPU.CASH,5<<2);
      CPU.USER[13] = CPU.SVC[0];
      CPU.USER[14] = CPU.SVC[1];
      break;
    case ARM_MODE_IRQ:
      CPU.IRQ[0]   = CPU.USER[13];
      CPU.IRQ[1]   = CPU.USER[14];
      CPU.USER[13] = CPU.SVC[0];
      CPU.USER[14] = CPU.SVC[1];
      break;
    case ARM_MODE_SVC:
      break;
    case ARM_MODE_ABT:
      CPU.ABT[0]   = CPU.USER[13];
      CPU.ABT[1]   = CPU.USER[14];
      CPU.USER[13] = CPU.SVC[0];
      CPU.USER[14] = CPU.SVC[1];
      break;
    case ARM_MODE_UND:
      CPU.UND[0]   = CPU.USER[13];
      CPU.UND[1]   = CPU.USER[14];
      CPU.USER[13] = CPU.SVC[0];
      CPU.USER[14] = CPU.SVC[1];
      break;
    }
}

static
void
ARM_RestAbtRONS(void)
{
  switch(arm_mode_table[CPU.CPSR & 0x1F])
    {
    case ARM_MODE_USER:
      CPU.CASH[5]  = CPU.USER[13];
      CPU.CASH[6]  = CPU.USER[14];
      CPU.USER[13] = CPU.ABT[0];
      CPU.USER[14] = CPU.ABT[1];
      break;
    case ARM_MODE_FIQ:
      memcpy(CPU.FIQ,&CPU.USER[8],7<<2);
      memcpy(&CPU.USER[8],CPU.CASH,5<<2);
      CPU.USER[13] = CPU.ABT[0];
      CPU.USER[14] = CPU.ABT[1];
      break;
    case ARM_MODE_IRQ:
      CPU.IRQ[0]   = CPU.USER[13];
      CPU.IRQ[1]   = CPU.USER[14];
      CPU.USER[13] = CPU.ABT[0];
      CPU.USER[14] = CPU.ABT[1];
      break;
    case ARM_MODE_SVC:
      CPU.SVC[0]   = CPU.USER[13];
      CPU.SVC[1]   = CPU.USER[14];
      CPU.USER[13] = CPU.ABT[0];
      CPU.USER[14] = CPU.ABT[1];
      break;
    case ARM_MODE_ABT:
      break;
    case ARM_MODE_UND:
      CPU.UND[0]   = CPU.USER[13];
      CPU.UND[1]   = CPU.USER[14];
      CPU.USER[13] = CPU.ABT[0];
      CPU.USER[14] = CPU.ABT[1];
      break;
    }
}

static
void
ARM_RestUndRONS(void)
{
  switch(arm_mode_table[CPU.CPSR & 0x1F])
    {
    case ARM_MODE_USER:
      CPU.CASH[5]  = CPU.USER[13];
      CPU.CASH[6]  = CPU.USER[14];
      CPU.USER[13] = CPU.UND[0];
      CPU.USER[14] = CPU.UND[1];
      break;
    case ARM_MODE_FIQ:
      memcpy(CPU.FIQ,&CPU.USER[8],7<<2);
      memcpy(&CPU.USER[8],CPU.CASH,5<<2);
      CPU.USER[13] = CPU.UND[0];
      CPU.USER[14] = CPU.UND[1];
      break;
    case ARM_MODE_IRQ:
      CPU.IRQ[0]   = CPU.USER[13];
      CPU.IRQ[1]   = CPU.USER[14];
      CPU.USER[13] = CPU.UND[0];
      CPU.USER[14] = CPU.UND[1];
      break;
    case ARM_MODE_SVC:
      CPU.SVC[0]   = CPU.USER[13];
      CPU.SVC[1]   = CPU.USER[14];
      CPU.USER[13] = CPU.UND[0];
      CPU.USER[14] = CPU.UND[1];
      break;
    case ARM_MODE_ABT:
      CPU.ABT[0]   = CPU.USER[13];
      CPU.ABT[1]   = CPU.USER[14];
      CPU.USER[13] = CPU.UND[0];
      CPU.USER[14] = CPU.UND[1];
      break;
    case ARM_MODE_UND:
      break;
    }
}

static
void
ARM_Change_ModeSafe(uint32_t mode_)
{
  switch(arm_mode_table[mode_ & 0x1F])
    {
    case ARM_MODE_USER:
      ARM_RestUserRONS();
      break;
    case ARM_MODE_FIQ:
      ARM_RestFiqRONS();
      break;
    case ARM_MODE_IRQ:
      ARM_RestIrqRONS();
      break;
    case ARM_MODE_SVC:
      ARM_RestSvcRONS();
      break;
    case ARM_MODE_ABT:
      ARM_RestAbtRONS();
      break;
    case ARM_MODE_UND:
      ARM_RestUndRONS();
      break;
    }
}

static
INLINE
void
arm_cpsr_set(uint32_t a_)
{
  a_ |= 0x10;
  ARM_Change_ModeSafe(a_);
  CPU.CPSR = (a_ & 0xf00000df);
}


static
INLINE
void
SETM(uint32_t a_)
{
  a_ |= 0x10;
  ARM_Change_ModeSafe(a_);
  CPU.CPSR = ((CPU.CPSR & 0xffffffe0) | (a_ & 0x1F));
}

static
INLINE
void
SETN(int a_)
{
  CPU.CPSR = (CPU.CPSR & 0x7fffffff) | ((a_ ? 1 << 31 : 0));
}

static
INLINE
void
SETZ(int a_)
{
  CPU.CPSR = (CPU.CPSR & 0xbfffffff) | ((a_ ? 1 << 30 : 0));
}

static
INLINE
void
SETC(int a_)
{
  CPU.CPSR = (CPU.CPSR & 0xdfffffff) | ((a_ ? 1 << 29 : 0));
}

static
INLINE
void
SETV(int a_)
{
  CPU.CPSR = (CPU.CPSR & 0xefffffff) | ((a_ ? 1 << 28 : 0));
}

static
INLINE
void
SETI(int a_)
{
  CPU.CPSR = (CPU.CPSR & 0xffffff7f) | ((a_ ? 1 << 7 : 0));
}

static
INLINE
void
SETF(int a_)
{
  CPU.CPSR = (CPU.CPSR & 0xffffffbf) | ((a_ ? 1 << 6 : 0));
}

static
void
arm_data_abort(void)
{
  CPU.SPSR[arm_mode_table[0x17]] = CPU.CPSR;
  SETI(1);
  SETM(0x17);
  CPU.USER[14] = (CPU.USER[15] + 4);
  CPU.USER[15] = 0x00000010;
  CYCLES -= (SCYCLE + NCYCLE);
  CPU.MAS_Access_Exept = false;
}

#define MODE ((CPU.CPSR & 0x1F))
#define ISN  ((CPU.CPSR >> 31) & 1)
#define ISZ  ((CPU.CPSR >> 30) & 1)
#define ISC  ((CPU.CPSR >> 29) & 1)
#define ISV  ((CPU.CPSR >> 28) & 1)
#define ISI  ((CPU.CPSR >>  7) & 1)
#define ISF  ((CPU.CPSR >>  6) & 1)

static
INLINE
uint32_t
ROTR(const uint32_t val_,
     const uint32_t shift_)
{
  return (shift_ ?
          ((val_ >> shift_) | (val_ << (32 - shift_))) :
          val_);
}

static void arm_cache_alloc(void);
static void arm_cache_free(void);

void
opera_arm_init(void)
{
  int i;

  g_arm_engine = (g_arm_engine_opt >= 0) ? g_arm_engine_opt : 1;
#ifndef OPERA_ARM_JIT_ENABLED
  if(g_arm_engine == 3)
    g_arm_engine = 1;
#endif

  if(g_arm_engine != 3)
    arm_cache_alloc();

  g_clio_regs          = opera_clio_regs_ptr();
  g_clio_fiqpend       = opera_clio_fiqpend_ptr();
  g_madam_fsm_poll     = opera_madam_fsm_ptr();
  g_cdrom_restart_poll = opera_cdrom_ode_restart_ptr();

  opera_arm_fetch_window_flush();

  g_SWI_HLE = 0;
  carry_out = 0;

  CYCLES = 0;
  for(i = 0; i < 16; i++)
    CPU.USER[i] = 0;

  for(i = 0; i < 2; i++)
    {
      CPU.SVC[i] = 0;
      CPU.ABT[i] = 0;
      CPU.IRQ[i] = 0;
      CPU.UND[i] = 0;
    }

  for(i = 0;i < 7; i++)
    CPU.CASH[i] = CPU.FIQ[i] = 0;

  opera_mem_seed_low_boot_word();

  CPU.nFIQ = false;
  CPU.MAS_Access_Exept = false;

  CPU.USER[15] = ARM_INITIAL_PC;
  arm_cpsr_set(0x13);
}

void
opera_arm_destroy(void)
{
  arm_cache_free();
  opera_arm_jit_destroy();
}

void
opera_arm_reset(void)
{
  int i;

  CYCLES = 0;
  g_SOFT_RESET_PENDING = false;
  opera_mem_rom_select(ROM1);
  opera_mem_seed_low_boot_word();

  for(i = 0; i < 16; i++)
    CPU.USER[i] = 0;

  for(i = 0; i < 2; i++)
    {
      CPU.SVC[i] = 0;
      CPU.ABT[i] = 0;
      CPU.IRQ[i] = 0;
      CPU.UND[i] = 0;
    }

  for(i = 0; i < 7; i++)
    CPU.CASH[i] = CPU.FIQ[i] = 0;

  CPU.MAS_Access_Exept = false;

  CPU.nFIQ = false;

  CPU.USER[15] = ARM_INITIAL_PC;
  arm_cpsr_set(0x13);

}

void
opera_arm_swi_hle_set(const int hle_)
{
  g_SWI_HLE = !!hle_;
}

int
opera_arm_swi_hle_get(void)
{
  return g_SWI_HLE;
}

static
void
ldm_accur(uint32_t opc_,
          uint32_t base_,
          uint32_t rn_ind_)
{
  uint16_t x;
  uint16_t list;
  uint32_t i;
  uint32_t tmp;
  uint32_t base_comp;

  i = 0;
  x    = opc_ & 0xFFFF;
  list = opc_ & 0xFFFF;
  x = ((x & 0x5555) + ((x >> 1) & 0x5555));
  x = ((x & 0x3333) + ((x >> 2) & 0x3333));
  x = ((x & 0x00ff) + (x >> 8));
  x = ((x & 0x000f) + (x >> 4));

  switch((opc_>>23)&3)
    {
    case 0:
      base_     -= (x << 2);
      base_comp  = (base_ + 4);
      break;
    case 1:
      base_comp  = base_;
      base_     += (x << 2);
      break;
    case 2:
      base_comp = base_ = (base_ - (x << 2));
      break;
    case 3:
      base_comp  = (base_ + 4);
      base_     += (x<<2);
      break;
    }

  //base_comp&=~3;

  //if(opc_&(1<<21))CPU.USER[rn_ind_]=base_;

  if((opc_ & (1 << 22)) && !(opc_ & 0x8000))
    {
      if(opc_ & (1 << 21))
        loadusr(rn_ind_,base_);

      while(list)
        {
          if(list & 1)
            {
              tmp = mreadw(base_comp);
              /*
                if(MAS_Access_Exept)
                {
                if(opc_&(1<<21))
                CPU.USER[rn_ind_]=base_;
                break;
                }
              */
              loadusr(i,tmp);
              base_comp += 4;
            }

          i++;
          list >>= 1;
        }
    }
  else
    {
      if(opc_ & (1 << 21))
        CPU.USER[rn_ind_] = base_;

      while(list)
        {
          if(list & 1)
            {
              tmp = mreadw(base_comp);
              CPU.USER[i]  = tmp;
              base_comp   += 4;
            }

          i++;
          list >>= 1;
        }

      if((opc_ & (1 << 22)) && arm_mode_table[MODE] /*&& !MAS_Access_Exept*/)
        arm_cpsr_set(CPU.SPSR[arm_mode_table[MODE]]);
    }

  CYCLES -= ((x-1) * SCYCLE + NCYCLE + ICYCLE);
}

static
void
stm_accur(uint32_t opc_,
          uint32_t base_,
          uint32_t rn_ind_)
{
  uint16_t x;
  uint16_t list;
  uint32_t i;
  uint32_t base_comp;

  i = 0;
  x    = opc_ & 0xFFFF;
  list = opc_ & 0x7FFF;
  x = ((x & 0x5555) + ((x >> 1) & 0x5555));
  x = ((x & 0x3333) + ((x >> 2) & 0x3333));
  x = ((x & 0x00ff) + (x >> 8));
  x = ((x & 0x000f) + (x >> 4));

  switch((opc_ >> 23) & 3)
    {
    case 0:
      base_     -= (x << 2);
      base_comp  = (base_ + 4);
      break;
    case 1:
      base_comp  = base_;
      base_     += (x << 2);
      break;
    case 2:
      base_comp = base_ = (base_ - (x << 2));
      break;
    case 3:
      base_comp  = (base_ + 4);
      base_     += (x << 2);
      break;
    }

  if((opc_ & (1 << 22)))
    {
      if((opc_ & (1 << 21)) && (opc_ & ((1 << rn_ind_) - 1)))
        loadusr(rn_ind_,base_);

      while(list)
        {
          if(list & 1)
            {
              mwritew(base_comp,readusr(i));
              if(g_SOFT_RESET_PENDING)
                return;
              //if(MAS_Access_Exept)break;
              base_comp += 4;
            }

          i++;
          list >>= 1;
        }

      if(opc_ & (1 << 21))
        loadusr(rn_ind_,base_);
    }
  else
    {
      if((opc_ & (1 << 21)) && (opc_ & ((1 << rn_ind_) - 1)))
        CPU.USER[rn_ind_] = base_;

      while(list)
        {
          if(list&1)
            {
              mwritew(base_comp,CPU.USER[i]);
              if(g_SOFT_RESET_PENDING)
                return;
              base_comp += 4;
            }

          i++;
          list >>= 1;
        }

      if(opc_ & (1 << 21))
        CPU.USER[rn_ind_] = base_;
    }

  if((opc_ & 0x8000) /*&& !MAS_Access_Exept*/)
    {
      mwritew(base_comp,CPU.USER[15]+8);
      if(g_SOFT_RESET_PENDING)
        return;
    }

  CYCLES -= ((x - 2) * SCYCLE + NCYCLE + NCYCLE);
}

static
void
bdt_core(uint32_t opc_)
{
  uint32_t base;
  uint32_t rn_ind = ((opc_ >> 16) & 0xF);

  if(rn_ind == 0xF)
    base = CPU.USER[rn_ind] + 8;
  else
    base = CPU.USER[rn_ind];

  if(opc_ & (1 << 20)) /* memory or register? */
    {
      if(opc_ & 0x8000)
        CYCLES -= (SCYCLE + NCYCLE);
      ldm_accur(opc_,base,rn_ind);
    }
  else
    {
      stm_accur(opc_,base,rn_ind);
    }
}

typedef struct TagArg
{
  uint32_t Type;
  uint32_t Arg;
} TagItem;


static
void
decode_swi_lle(void)
{
  CPU.SPSR[arm_mode_table[0x13]] = CPU.CPSR;

  SETI(1);
  SETM(0x13);

  CPU.USER[14] = CPU.USER[15];
  CPU.USER[15] = 0x00000008;
}

static void decode_swi_hle(const uint32_t op_)
{
  uint32_t r0_;
  uint32_t cnt_;

  switch(op_ & 0x000FFFFF)
    {
    case 0x50000:
      r0_ = CPU.USER[0];
      opera_swi_hle_0x50000(DRAM,CPU.USER[0],CPU.USER[1],CPU.USER[2]);
      opera_mem_jit_touch_range(r0_,12u);   /* dest vec3f16 */
      return;
    case 0x50001:
      r0_ = CPU.USER[0];
      opera_swi_hle_0x50001(DRAM,CPU.USER[0],CPU.USER[1],CPU.USER[2]);
      opera_mem_jit_touch_range(r0_,36u);   /* dest mat33f16 */
      return;
    case 0x50002:
      r0_ = CPU.USER[0];
      cnt_ = CPU.USER[3];
      opera_swi_hle_0x50002(DRAM,CPU.USER[0],CPU.USER[1],CPU.USER[2],CPU.USER[3]);
      opera_mem_jit_touch_range(r0_,(cnt_ * 12u));   /* dest vec3[] */
      return;
    case 0x50003:
      break;
    case 0x50004:
      break;
    case 0x50005:
      r0_ = CPU.USER[0];
      cnt_ = CPU.USER[3];
      opera_swi_hle_0x50005(DRAM,CPU.USER[0],CPU.USER[1],CPU.USER[2],CPU.USER[3]);
      opera_mem_jit_touch_range(r0_,(cnt_ * 4u));    /* dest frac16[] */
      return;
    case 0x50006:
      r0_ = CPU.USER[0];
      cnt_ = CPU.USER[3];
      opera_swi_hle_0x50006(DRAM,CPU.USER[0],CPU.USER[1],CPU.USER[2],CPU.USER[3]);
      opera_mem_jit_touch_range(r0_,(cnt_ * 4u));    /* dest frac16[] */
      return;
    case 0x50007:
      r0_ = CPU.USER[0];
      opera_swi_hle_0x50007(DRAM,CPU.USER[0],CPU.USER[1],CPU.USER[2]);
      opera_mem_jit_touch_range(r0_,16u);   /* dest vec4f16 */
      return;
    case 0x50008:
      r0_ = CPU.USER[0];
      opera_swi_hle_0x50008(DRAM,CPU.USER[0],CPU.USER[1],CPU.USER[2]);
      opera_mem_jit_touch_range(r0_,64u);   /* dest mat44f16 */
      return;
    case 0x50009:
      r0_ = CPU.USER[0];
      cnt_ = CPU.USER[3];
      opera_swi_hle_0x50009(DRAM,CPU.USER[0],CPU.USER[1],CPU.USER[2],CPU.USER[3]);
      opera_mem_jit_touch_range(r0_,(cnt_ * 16u));   /* dest vec4[] */
      return;
    case 0x5000A:
      break;
    case 0x5000B:
      break;
    case 0x5000C:
      CPU.USER[0] = opera_swi_hle_0x5000C(DRAM,CPU.USER[0],CPU.USER[1]);
      return;
    case 0x5000E:
      r0_ = CPU.USER[0];
      opera_swi_hle_0x5000E(DRAM,CPU.USER[0],CPU.USER[1],CPU.USER[2]);
      opera_mem_jit_touch_range(r0_,12u);   /* dest vec3f16 */
      return;
    case 0x5000F:
      CPU.USER[0] = opera_swi_hle_0x5000F(DRAM,CPU.USER[0]);
      return;
    case 0x50010:
      CPU.USER[0] = opera_swi_hle_0x50010(DRAM,CPU.USER[0]);
      return;
    case 0x50011:
      r0_ = CPU.USER[0];
      opera_swi_hle_0x50011(DRAM,CPU.USER[0],CPU.USER[1],CPU.USER[2],CPU.USER[3]);
      opera_mem_jit_touch_range(r0_,12u);   /* dest vec3f16 */
      return;
    case 0x50012:
      {
        /* mmv3m33d: the destination pointer itself lives at [r0+0] */
        r0_ = *(uint32_t*)&DRAM[CPU.USER[0]];
        cnt_ = *(uint32_t*)&DRAM[CPU.USER[0] + 0x10];
        opera_swi_hle_0x50012(DRAM,CPU.USER[0]);
        opera_mem_jit_touch_range(r0_,(cnt_ * 12u));   /* dest vec3[] */
        return;
      }
    }

  decode_swi_lle();
}

static void decode_swi(const uint32_t op_)
{
  CYCLES -= (SCYCLE + NCYCLE);  // +2S+1N

  if(g_SWI_HLE)
  {
    decode_swi_hle(op_);
    return;
  }

  decode_swi_lle();
}


static
INLINE
void
ARM_SET_C(const uint32_t x_)
{
  CPU.CPSR = ((CPU.CPSR & 0xdfffffff) | ((x_ & 1) << 29));
}

static
INLINE
void
ARM_SET_Z(const uint32_t x_)
{
  CPU.CPSR = ((CPU.CPSR & 0xbfffffff) | (x_ == 0 ? 0x40000000 : 0));
}

static
INLINE
void
ARM_SET_N(const uint32_t x_)
{
  CPU.CPSR = ((CPU.CPSR & 0x7fffffff) | (x_ & 0x80000000));
}

static
INLINE
uint32_t
ARM_GET_C(void)
{
  return ((CPU.CPSR >> 29) & 1);
}

static
INLINE
void
ARM_SET_ZN(const uint32_t val_)
{
  if(val_)
    CPU.CPSR = ((CPU.CPSR & 0x3fffffff) | (val_ & 0x80000000));
  else
    CPU.CPSR = ((CPU.CPSR & 0x3fffffff) | 0x40000000);
}

static
INLINE
void
ARM_SET_CV(const uint32_t rd_,
           const uint32_t op1_,
           const uint32_t op2_)
{
  CPU.CPSR = ((CPU.CPSR & 0xcfffffff) |
              ((((op1_ & op2_) | ((~rd_) & (op1_ | op2_))) & 0x80000000) >> 2) |
              (((((op1_ & (op2_ & (~rd_))) | ((~op1_) & (~op2_) & rd_))) & 0x80000000) >> 3));
}

static
INLINE
void
ARM_SET_CV_sub(uint32_t rd_,
               uint32_t op1_,
               uint32_t op2_)
{
  CPU.CPSR = ((CPU.CPSR & 0xcfffffff) |
              ((((op1_ & (~op2_)) | ((~rd_) & (op1_ | (~op2_)))) & 0x80000000) >> 2) |
              (((((op1_ & ((~op2_) & (~rd_))) | ((~op1_) & op2_ & rd_))) & 0x80000000) >> 3));
}

static
OPERA_FORCEINLINE
int
ARM_ALU_Exec(uint32_t  inst_,
             uint8_t   opc_,
             uint32_t  op1_,
             uint32_t  op2_,
             uint32_t *rd_)
{
  switch(opc_)
    {
    case 0:
      *rd_ = op1_ & op2_;
      break;
    case 2:
      *rd_ = op1_ ^ op2_;
      break;
    case 4:
      *rd_ = op1_ - op2_;
      break;
    case 6:
      *rd_ = op2_ - op1_;
      break;
    case 8:
      *rd_ = op1_ + op2_;
      break;
    case 10:
      *rd_ = op1_ + op2_ + ARM_GET_C();
      break;
    case 12:
      *rd_ = op1_ - op2_ - (ARM_GET_C() ^ 1);
      break;
    case 14:
      *rd_ = op2_ - op1_ - (ARM_GET_C() ^ 1);
      break;
    case 16:
    case 20:
      if((inst_ >> 22) & 1)
        CPU.USER[(inst_ >> 12) & 0xF] = CPU.SPSR[arm_mode_table[CPU.CPSR & 0x1F]];
      else
        CPU.USER[(inst_ >> 12) & 0xF] = CPU.CPSR;
      return true;
    case 18:
    case 22:
      if(!((inst_ >> 16) & 0x1) || !(arm_mode_table[MODE]))
        {
          if((inst_ >> 22) & 1)
            CPU.SPSR[arm_mode_table[MODE]] = (CPU.SPSR[arm_mode_table[MODE]] & 0x0fffffff) | (op2_ & 0xf0000000);
          else
            CPU.CPSR = (CPU.CPSR & 0x0fffffff) | (op2_ & 0xf0000000);
        }
      else
        {
          if((inst_ >> 22) & 1)
            CPU.SPSR[arm_mode_table[MODE]] = op2_ & 0xf00000df;
          else
            arm_cpsr_set(op2_);
        }
      return true;
    case 24:
      *rd_ = op1_ | op2_;
      break;
    case 26:
      *rd_ = op2_;
      break;
    case 28:
      *rd_ = op1_ & ~op2_;
      break;
    case 30:
      *rd_ = ~op2_;
      break;
    case 1:
      *rd_ = op1_ & op2_;
      ARM_SET_ZN(*rd_);
      break;
    case 3:
      *rd_ = op1_ ^ op2_;
      ARM_SET_ZN(*rd_);
      break;
    case 5:
      *rd_ = op1_ - op2_;
      ARM_SET_ZN(*rd_);
      ARM_SET_CV_sub(*rd_,op1_,op2_);
      break;
    case 7:
      *rd_ = op2_ - op1_;
      ARM_SET_ZN(*rd_);
      ARM_SET_CV_sub(*rd_,op2_,op1_);
      break;
    case 9:
      *rd_ = op1_ + op2_;
      ARM_SET_ZN(*rd_);
      ARM_SET_CV(*rd_,op1_,op2_);
      break;

    case 11:
      *rd_ = op1_ + op2_ + ARM_GET_C();
      ARM_SET_ZN(*rd_);
      ARM_SET_CV(*rd_,op1_,op2_);
      break;
    case 13:
      *rd_ = op1_ - op2_ - (ARM_GET_C()^1);
      ARM_SET_ZN(*rd_);
      ARM_SET_CV_sub(*rd_,op1_,op2_);
      break;
    case 15:
      *rd_ = op2_ - op1_ - (ARM_GET_C()^1);
      ARM_SET_ZN(*rd_);
      ARM_SET_CV_sub(*rd_,op2_,op1_);
      break;//*/
    case 17:
      op1_ &= op2_;
      ARM_SET_ZN(op1_);
      return true;
    case 19:
      op1_ ^= op2_;
      ARM_SET_ZN(op1_);
      return true;
    case 21:
      ARM_SET_CV_sub(op1_ - op2_,op1_,op2_);
      ARM_SET_ZN(op1_ - op2_);
      return true;
    case 23:
      ARM_SET_CV(op1_ + op2_,op1_,op2_);
      ARM_SET_ZN(op1_ + op2_);
      return true;
    case 25:
      *rd_ = op1_ | op2_;
      ARM_SET_ZN(*rd_);
      break;
    case 27:
      *rd_ = op2_;
      ARM_SET_ZN(*rd_);
      break;
    case 29:
      *rd_ = op1_ & ~op2_;
      ARM_SET_ZN(*rd_);
      break;
    case 31:
      *rd_ = ~op2_;
      ARM_SET_ZN(*rd_);
      break;
    };

  return false;
}

static
OPERA_FORCEINLINE
uint32_t
ARM_SHIFT_NSC(uint32_t value_,
              uint8_t  shift_,
              uint8_t  type_)
{
  switch(type_)
    {
    case 0:
      if(shift_)
        {
          if(shift_ > 32)
            carry_out = 0;
          else
            carry_out = (((value_ << (shift_ - 1)) & 0x80000000) >> 31);
        }
      else
        {
          carry_out = ARM_GET_C();
        }

      if(shift_ == 0)
        return value_;
      if(shift_ > 31)
        return 0;
      return (value_ << shift_);
    case 1:
      if(shift_)
        {
          if(shift_ > 32)
            carry_out = 0;
          else
            carry_out = ((value_ >> (shift_ - 1)) & 1);
        }
      else
        {
          carry_out = ARM_GET_C();
        }

      if(shift_ == 0)
        return value_;
      if(shift_ > 31)
        return 0;
      return (value_ >> shift_);
    case 2:
      if(shift_)
        {
          if(shift_ > 32)
            carry_out = ((((int32_t)value_) >> 31) & 1);
          else
            carry_out = ((((int32_t)value_) >> (shift_ - 1)) & 1);
        }
      else
        {
          carry_out = ARM_GET_C();
        }

      if(shift_ == 0)
        return value_;
      if(shift_ > 31)
        return (((int32_t)value_) >> 31);
      return (((int32_t)value_) >> shift_);
    case 3:
      if(shift_)
        {
          if(shift_&31)
            carry_out = ((value_ >> (shift_ - 1)) & 1);
          else
            carry_out = ((value_ >> 31) & 1);
        }
      else
        {
          carry_out = ARM_GET_C();
        }

      shift_ &= 31;
      if(shift_ == 0)
        return value_;
      return ROTR(value_,shift_);
    case 4:
      carry_out = value_ & 1;
      return ((value_ >> 1) | (ARM_GET_C() << 31));
    }

  return 0;
}

static
uint32_t
ARM_SHIFT_SC(uint32_t value_,
             uint8_t  shift_,
             uint8_t  type_)
{
  uint32_t tmp;

  switch(type_)
    {
    case 0:
      if(shift_)
        {
          if(shift_ > 32)
            ARM_SET_C(0);
          else
            ARM_SET_C(((value_ << (shift_ - 1)) & 0x80000000) >> 31);
        }
      else
        {
          return value_;
        }

      if(shift_ > 31)
        return 0;
      return (value_ << shift_);
    case 1:
      if(shift_)
        {
          if(shift_ > 32)
            ARM_SET_C(0);
          else
            ARM_SET_C((value_ >> (shift_ - 1)) & 1);
        }
      else
        {
          return value_;
        }

      if(shift_ > 31)
        return 0;
      return (value_ >> shift_);
    case 2:
      if(shift_)
        {
          if(shift_ > 32)
            ARM_SET_C((((int32_t)value_) >> 31) & 1);
          else
            ARM_SET_C((((int32_t)value_) >> (shift_ - 1)) & 1);
        }
      else
        {
          return value_;
        }

      if(shift_ > 31)
        return (((int32_t)value_) >> 31);
      return ((int32_t)value_) >> shift_;
    case 3:
      if(shift_)
        {
          shift_ = (shift_ & 31);
          if(shift_)
            ARM_SET_C((value_ >> (shift_ - 1)) & 1);
          else
            ARM_SET_C((value_ >> 31) & 1);
        }
      else
        {
          return value_;
        }

      return ROTR(value_,shift_);
    case 4:
      tmp = ARM_GET_C() << 31;
      ARM_SET_C(value_ & 1);
      return ((value_ >> 1) | (tmp));
    }

  return 0;
}

static
OPERA_FORCEINLINE
void
ARM_SWAP(uint32_t cmd_)
{
  uint32_t tmp;
  uint32_t addr;

  CPU.USER[15] += 4;
  addr          = CPU.USER[(cmd_ >> 16) & 0xF];
  CPU.USER[15] += 4;

  if(cmd_ & (1 << 22))
    {
      tmp = mreadb(addr);
      //	if(MAS_Access_Exept)return true;
      mwriteb(addr,CPU.USER[cmd_ & 0xF]);
      if(g_SOFT_RESET_PENDING)
        return;
      CPU.USER[15] -= 8;
      //	if(MAS_Access_Exept)return true;
      CPU.USER[(cmd_ >> 12) & 0xF] = tmp;
    }
  else
    {
      tmp = mreadw(addr);
      //if(MAS_Access_Exept)return true;
      mwritew(addr,CPU.USER[cmd_ & 0xF]);
      if(g_SOFT_RESET_PENDING)
        return;
      CPU.USER[15] -= 8;
      //if(MAS_Access_Exept)return true;
      if(addr & 3)
        tmp = ((tmp >> ((addr & 3) << 3)) | (tmp << (32 - ((addr & 3) << 3))));
      CPU.USER[(cmd_ >> 12) & 0xF] = tmp;
    }
}

static
INLINE
uint32_t
calcbits(uint32_t num_)
{
  uint32_t rv;

  if(!num_)
    return 1;

  if(num_ >> 16)
    {
      num_ >>= 16;
      rv     = 16;
    }
  else
    {
      rv = 0;
    }

  if(num_ >> 8)
    {
      num_ >>= 8;
      rv    += 8;
    }

  if(num_ >> 4)
    {
      num_ >>= 4;
      rv    += 4;
    }

  if(num_ >> 2)
    {
      num_ >>= 2;
      rv    += 2;
    }

  if(num_ >> 1)
    {
      num_ >>= 1;
      rv    += 2;
    }
  else if(num_)
    {
      rv++;
    }

  return rv;
}

static const int is_logic[] =
  {
    true,true,false,false,
    false,false,false,false,
    true,true,false,false,
    true,true,true,true
  };

/* ---------------------------------------------------------------------------
 * Cached decode engine.
 *
 * Per-word decode cache for the RAM and ROM/ANVIL address windows.  Each
 * entry stores the raw instruction word as a tag plus the handler for its
 * instruction class.  A hit validates the tag against the freshly fetched
 * word, so self-modifying code, save-state loads, ROM swaps, and any other
 * content mutation re-decode automatically.  Fetches outside the cached
 * windows (MADAM/CLIO/SPORT/NVRAM/diag space or unmapped addresses) take
 * the interpreter path verbatim, preserving 0xBADACCE5 and fault behavior.
 *
 * Handler bodies are line-for-line transcriptions of the interpreter case
 * bodies: identical CYCLES arithmetic, identical CPU.USER[15] bump/restore
 * points (pipeline exposure), identical flag update order.  The per-
 * instruction soft-reset and FIQ tails live in arm_execute_slice()'s
 * per-word tails and match the interpreter's sequencing.
 * ------------------------------------------------------------------------- */
enum
  {
    ARM_CLS_DP_IMM,   /* data processing, immediate operand            */
    ARM_CLS_DP_RS,    /* data processing, register, shift-by-register  */
    ARM_CLS_DP_RI,    /* data processing, register, shift-by-immediate */
    ARM_CLS_SDT_IMM,  /* single data transfer, immediate offset        */
    ARM_CLS_SDT_RI,   /* single data transfer, register offset         */
    ARM_CLS_SDT_RS,   /* single data transfer, register-shifted offset */
    ARM_CLS_MUL,      /* multiply / multiply-accumulate                */
    ARM_CLS_SDS,      /* single data swap                              */
    ARM_CLS_BDT,      /* block data transfer                           */
    ARM_CLS_BRANCH,   /* branch / branch-with-link                     */
    ARM_CLS_SWI,      /* SWI (LLE or HLE)                              */
    ARM_CLS_UND,      /* undefined-instruction trap + coprocessor      */
    ARM_CLS_SPEC,     /* 0xE5101810 CPSR-latch special                 */
    ARM_CLS_DP_IMM_NP,/* variants with no r15 operand: the interpreter's */
    ARM_CLS_DP_RS_NP, /* PC bump/restore pairs are provably unobserved   */
    ARM_CLS_DP_RI_NP, /* within these bodies (no operand reads USER[15], */
    ARM_CLS_SDT_IMM_NP,/* no write targets r15), so they are elided     */
    ARM_CLS_SDT_RI_NP,

    /* >= ARM_CLS_TIGHT_MIN: poll-free tight-loop classes.  These forms
     * cannot perform stores, cannot write a control register (MSR forms
     * opc 18/22 excluded at decode), cannot trap, and cannot write the
     * PC - so no FIQ/MADAM/CD-ROM/ROM-bank state they could touch
     * changes while they run, and the per-instruction soft-reset can
     * never fire.  Only cycle accounting, condition evaluation, and the
     * budget check are semantically observable; those stay. */
    ARM_CLS_TIGHT_MIN,
    ARM_CLS_TIGHT_DP_IMM = ARM_CLS_TIGHT_MIN,
    ARM_CLS_TIGHT_DP_RS,
    ARM_CLS_TIGHT_DP_RI,
    ARM_CLS_TIGHT_MUL,
    ARM_CLS_TIGHT_BRANCH,
    ARM_CLS_TIGHT_SDT_LDI, /* poll-free single-data LOAD, imm offset  */
    ARM_CLS_TIGHT_SDT_LDR, /* poll-free single-data LOAD, reg offset  */
    ARM_CLS_TIGHT_SDT_STI, /* poll-free DRAM-safe single-data STORE,imm */
    ARM_CLS_TIGHT_SDT_STR, /* poll-free DRAM-safe single-data STORE,reg */
    ARM_CLS_TIGHT_DP_PC,   /* rd == 15, S = 0: inlineable pc-write DP    */
    ARM_CLS_TIGHT_DP_PCREL, /* rn == 15: op1 = pc_k + 4, a compile-time
                            * constant; every other shape is tight      */
    ARM_CLS_TIGHT_DP_RMPC  /* rm == 15 (rd,rn != 15): op2 = USER[15] read
                            * after the pipeline bump = pc_k + 8, a
                            * compile-time constant; the op2 load
                            * collapses to one imm32 */
  };

typedef struct
{
  uint32_t word;
  uint32_t aux;
  uint32_t cls;
  uint32_t sealed;
} arm_cache_entry_t;

static arm_cache_entry_t *g_arm_cache_ram;
static arm_cache_entry_t *g_arm_cache_rom;
static uint32_t           g_arm_cache_ram_words = 0;
static uint32_t           g_arm_cache_rom_words = 0;

static
void
arm_cache_free(void)
{
  free(g_arm_cache_ram);
  free(g_arm_cache_rom);
  g_arm_cache_ram       = NULL;
  g_arm_cache_rom       = NULL;
  g_arm_cache_ram_words = 0;
  g_arm_cache_rom_words = 0;
}

static
void
arm_cache_alloc(void)
{
  uint32_t const ram_words = (RAM_SIZE >> 2);
  uint32_t const rom_words = ((ROM1_SIZE_MASK + 1) >> 2);

  if(g_arm_cache_ram && (g_arm_cache_ram_words == ram_words))
    return;

  arm_cache_free();

  {
    arm_cache_entry_t *const ram_ = calloc(ram_words,sizeof(arm_cache_entry_t));
    arm_cache_entry_t *const rom_ = calloc(rom_words,sizeof(arm_cache_entry_t));

    if(!ram_ || !rom_)
      {
        /* partial failure: free both so the wrapper's g_arm_cache_ram
         * NULL test degrades the engine to interp cleanly (a NULL rom
         * array with a live ram array used to leave the fallback gate
         * blind and the first ROM fetch dereferencing near-NULL) */
        free(ram_);
        free(rom_);
        return;
      }

    g_arm_cache_ram       = ram_;
    g_arm_cache_rom       = rom_;
    g_arm_cache_ram_words = ram_words;
    g_arm_cache_rom_words = rom_words;
  }

  opera_arm_fetch_window_flush();
}

#define ARM_CACHE_COND_OK(cmd_) \
  (((cmd_) >> 28) == 0xE || \
   ((cond_flags_cross[(cmd_) >> 28] >> (CPU.CPSR >> 28)) & 1))

static
OPERA_FORCEINLINE
void
arm_dp_tail(uint32_t cmd_,
            uint32_t op1_,
            uint32_t op2_,
            uint32_t pc_tmp_,
             int *cycp_)
{
  CPU.USER[15] = pc_tmp_;

  if((cmd_ & (1 << 20)) && is_logic[(cmd_ >> 21) & 0xF])
    ARM_SET_C(carry_out);

  if(ARM_ALU_Exec(cmd_,((cmd_ >> 20) & 0x1F),op1_,op2_,&CPU.USER[(cmd_ >> 12) & 0xF]))
    return;

  if(((cmd_ >> 12) & 0xF) == 0xF)
    {
      if(cmd_ & (1 << 20))
        arm_cpsr_set(CPU.SPSR[arm_mode_table[MODE]]);

      (*cycp_) -= (ICYCLE + NCYCLE);
    }
}

static
OPERA_FORCEINLINE
void
arm_h_dp_imm(uint32_t cmd_,
             uint32_t aux_,
             int *cycp_)
{
  uint32_t op1;
  uint32_t op2;
  uint32_t pc_tmp;

  if(!ARM_CACHE_COND_OK(cmd_))
    return;

  pc_tmp        = CPU.USER[15];
  CPU.USER[15] += 4;

  op2 = ARM_SHIFT_NSC(cmd_ & ARM_DP_IMM_MASK,aux_,ARM_SHIFT_TYPE_ROR);
  op1 = CPU.USER[(cmd_ >> 16) & 0xF];

  arm_dp_tail(cmd_,op1,op2,pc_tmp,cycp_);
}

static
OPERA_FORCEINLINE
void
arm_h_dp_imm_np(uint32_t cmd_,
                uint32_t aux_,
             int *cycp_)
{
  uint32_t op1;
  uint32_t op2;

  if(!ARM_CACHE_COND_OK(cmd_))
    return;

  op2 = ARM_SHIFT_NSC(cmd_ & ARM_DP_IMM_MASK,aux_,ARM_SHIFT_TYPE_ROR);
  op1 = CPU.USER[(cmd_ >> 16) & 0xF];

  if((cmd_ & (1 << 20)) && is_logic[(cmd_ >> 21) & 0xF])
    ARM_SET_C(carry_out);

  ARM_ALU_Exec(cmd_,((cmd_ >> 20) & 0x1F),op1,op2,&CPU.USER[(cmd_ >> 12) & 0xF]);
}

static
OPERA_FORCEINLINE
void
arm_h_dp_rs(uint32_t cmd_,
            uint32_t aux_,
             int *cycp_)
{
  uint8_t  shift;
  uint32_t op1;
  uint32_t op2;
  uint32_t pc_tmp;

  (void)aux_;

  if(!ARM_CACHE_COND_OK(cmd_))
    return;

  pc_tmp        = CPU.USER[15];
  CPU.USER[15] += 4;

  shift         = ((cmd_ >> 8) & 0xF);
  shift         = (CPU.USER[shift] & 0xFF);
  CPU.USER[15] += 4;
  op2           = CPU.USER[cmd_ & 0xF];
  op1           = CPU.USER[(cmd_ >> 16) & 0xF];
  (*cycp_)       -= ICYCLE;

  op2 = ARM_SHIFT_NSC(op2,shift,((cmd_ >> 5) & 0x3));

  arm_dp_tail(cmd_,op1,op2,pc_tmp,cycp_);
}

static
OPERA_FORCEINLINE
void
arm_h_dp_rs_np(uint32_t cmd_,
               uint32_t aux_,
             int *cycp_)
{
  uint8_t  shift;
  uint32_t op1;
  uint32_t op2;

  (void)aux_;

  if(!ARM_CACHE_COND_OK(cmd_))
    return;

  shift  = ((cmd_ >> 8) & 0xF);
  shift  = (CPU.USER[shift] & 0xFF);
  op2    = CPU.USER[cmd_ & 0xF];
  op1    = CPU.USER[(cmd_ >> 16) & 0xF];
  (*cycp_) -= ICYCLE;

  op2 = ARM_SHIFT_NSC(op2,shift,((cmd_ >> 5) & 0x3));

  if((cmd_ & (1 << 20)) && is_logic[(cmd_ >> 21) & 0xF])
    ARM_SET_C(carry_out);

  ARM_ALU_Exec(cmd_,((cmd_ >> 20) & 0x1F),op1,op2,&CPU.USER[(cmd_ >> 12) & 0xF]);
}

static
OPERA_FORCEINLINE
void
arm_h_dp_ri(uint32_t cmd_,
            uint32_t aux_,
             int *cycp_)
{
  uint32_t op1;
  uint32_t op2;
  uint32_t pc_tmp;

  if(!ARM_CACHE_COND_OK(cmd_))
    return;

  pc_tmp        = CPU.USER[15];
  CPU.USER[15] += 4;

  op2 = CPU.USER[cmd_ & 0xF];
  op1 = CPU.USER[(cmd_ >> 16) & 0xF];
  op2 = ARM_SHIFT_NSC(op2,(aux_ & 0x3F),((aux_ >> 8) & 0x7));

  arm_dp_tail(cmd_,op1,op2,pc_tmp,cycp_);
}

static
OPERA_FORCEINLINE
void
arm_sdt_body(uint32_t cmd_,
             uint32_t oper2_,
             uint32_t pc_tmp_,
             int *cycp_)
{
  uint32_t base;
  uint32_t tbas;
  uint32_t val;
  uint32_t rora;

  tbas = base = CPU.USER[((cmd_ >> 16) & 0xF)];

  if(!(cmd_ & (1 << 23)))
    oper2_ = (0 - oper2_);

  if(cmd_ & (1 << 24))
    tbas = base = (base + oper2_);
  else
    base = (base + oper2_);

  if(cmd_ & (1 << 20))
    {
      if(cmd_ & (1 << 22))
        {
          val = mreadb(tbas);
        }
      else
        {
          rora = (tbas & 3);
          val  = mreadw(tbas);

          if(rora && !CPU.MAS_Access_Exept)
            val = ROTR(val,rora*8);
        }

      CPU.USER[15] = pc_tmp_;
      if(CPU.MAS_Access_Exept)
        {
          /* the abort's cycle charge lands in the file-scope CYCLES (the
           * interpreter's accumulator); transfer it through cycp_ like
           * arm_h_bdt does, or the cached engine drops the 3 cycles */
          CYCLES = (*cycp_);
          arm_data_abort();
          (*cycp_) = CYCLES;
          return;
        }

      if(((cmd_ >> 12) & 0xF) == 0xF)
        (*cycp_) -= (SCYCLE + NCYCLE);

      (*cycp_) -= (NCYCLE + ICYCLE);

      if((cmd_ & (1 << 21)) || (!(cmd_ & (1 << 24))))
        CPU.USER[(cmd_ >> 16) & 0xF] = base;

      if((cmd_ & (1 << 21)) && !(cmd_ & (1 << 24)))
        loadusr((cmd_ >> 12) & 0xF,val);
      else
        CPU.USER[(cmd_ >> 12) & 0xF] = val;
    }
  else
    {
      if((cmd_ & (1 << 21)) && !(cmd_ & (1 << 24)))
        val = readusr((cmd_ >> 12) & 0xF);
      else
        val = CPU.USER[(cmd_ >> 12) & 0xF];

      CPU.USER[15]  = pc_tmp_;
      (*cycp_)       -= (-SCYCLE + 2 * NCYCLE);

      if(cmd_ & (1 << 22))
        mwriteb(tbas,val);
      else
        mwritew(tbas,val);

      if(g_SOFT_RESET_PENDING)
        return;

      if(CPU.MAS_Access_Exept)
        {
          CYCLES = (*cycp_);
          arm_data_abort();
          (*cycp_) = CYCLES;
          return;
        }

      if((cmd_ & (1 << 21)) || (!(cmd_ & (1 << 24))))
        CPU.USER[(cmd_ >> 16) & 0xF] = base;
    }
}

static
OPERA_FORCEINLINE
void
arm_h_dp_ri_np(uint32_t cmd_,
               uint32_t aux_,
             int *cycp_)
{
  uint32_t op1;
  uint32_t op2;

  if(!ARM_CACHE_COND_OK(cmd_))
    return;

  op2 = CPU.USER[cmd_ & 0xF];
  op1 = CPU.USER[(cmd_ >> 16) & 0xF];
  op2 = ARM_SHIFT_NSC(op2,(aux_ & 0x3F),((aux_ >> 8) & 0x7));

  if((cmd_ & (1 << 20)) && is_logic[(cmd_ >> 21) & 0xF])
    ARM_SET_C(carry_out);

  ARM_ALU_Exec(cmd_,((cmd_ >> 20) & 0x1F),op1,op2,&CPU.USER[(cmd_ >> 12) & 0xF]);
}

static
OPERA_FORCEINLINE
void
arm_sdt_body_np(uint32_t cmd_,
                uint32_t oper2_,
             int *cycp_)
{
  uint32_t base;
  uint32_t tbas;
  uint32_t val;
  uint32_t rora;

  tbas = base = CPU.USER[((cmd_ >> 16) & 0xF)];

  if(!(cmd_ & (1 << 23)))
    oper2_ = (0 - oper2_);

  if(cmd_ & (1 << 24))
    tbas = base = (base + oper2_);
  else
    base = (base + oper2_);

  if(cmd_ & (1 << 20))
    {
      if(cmd_ & (1 << 22))
        {
          val = mreadb(tbas);
        }
      else
        {
          rora = (tbas & 3);
          val  = mreadw(tbas);

          if(rora && !CPU.MAS_Access_Exept)
            val = ROTR(val,rora*8);
        }

      if(CPU.MAS_Access_Exept)
        {
          CYCLES = (*cycp_);
          arm_data_abort();
          (*cycp_) = CYCLES;
          return;
        }

      (*cycp_) -= (NCYCLE + ICYCLE);

      if((cmd_ & (1 << 21)) || (!(cmd_ & (1 << 24))))
        CPU.USER[(cmd_ >> 16) & 0xF] = base;

      if((cmd_ & (1 << 21)) && !(cmd_ & (1 << 24)))
        loadusr((cmd_ >> 12) & 0xF,val);
      else
        CPU.USER[(cmd_ >> 12) & 0xF] = val;
    }
  else
    {
      if((cmd_ & (1 << 21)) && !(cmd_ & (1 << 24)))
        val = readusr((cmd_ >> 12) & 0xF);
      else
        val = CPU.USER[(cmd_ >> 12) & 0xF];

      (*cycp_) -= (-SCYCLE + 2 * NCYCLE);

      if(cmd_ & (1 << 22))
        mwriteb(tbas,val);
      else
        mwritew(tbas,val);

      if(g_SOFT_RESET_PENDING)
        return;

      if(CPU.MAS_Access_Exept)
        {
          CYCLES = (*cycp_);
          arm_data_abort();
          (*cycp_) = CYCLES;
          return;
        }

      if((cmd_ & (1 << 21)) || (!(cmd_ & (1 << 24))))
        CPU.USER[(cmd_ >> 16) & 0xF] = base;
    }
}

static
OPERA_FORCEINLINE
void
arm_h_sdt_imm(uint32_t cmd_,
              uint32_t aux_,
             int *cycp_)
{
  uint32_t pc_tmp;

  (void)aux_;

  if(!ARM_CACHE_COND_OK(cmd_))
    return;

  pc_tmp        = CPU.USER[15];
  CPU.USER[15] += 4;

  arm_sdt_body(cmd_,(cmd_ & 0x0FFF),pc_tmp,cycp_);
}

static
OPERA_FORCEINLINE
void
arm_h_sdt_imm_np(uint32_t cmd_,
                 uint32_t aux_,
             int *cycp_)
{
  (void)aux_;

  if(!ARM_CACHE_COND_OK(cmd_))
    return;

  arm_sdt_body_np(cmd_,(cmd_ & 0x0FFF),cycp_);
}

static
OPERA_FORCEINLINE
void
arm_h_sdt_ri_np(uint32_t cmd_,
                uint32_t aux_,
             int *cycp_)
{
  uint32_t oper2;

  if(!ARM_CACHE_COND_OK(cmd_))
    return;

  oper2 = ARM_SHIFT_NSC(CPU.USER[cmd_ & 0xF],(aux_ & 0x3F),((aux_ >> 8) & 0x7));

  arm_sdt_body_np(cmd_,oper2,cycp_);
}

/* TIGHT_SDT_STI: store counterpart of arm_h_sdt_ldi_t, mirroring the
 * store half of arm_sdt_body_np exactly.  Fast when the address lands in
 * DRAM without the HIRES fanout (which also performs the invalidation
 * hooks via opera_mem_write8/32); anything else defers wholesale to the
 * classic full body.  Identical cycle accounting: the np store half
 * charges (-SCYCLE + 2*NCYCLE) on top of the caller's -SCYCLE base. */
static
OPERA_FORCEINLINE
void
arm_h_sdt_sti_t(uint32_t cmd_,
                uint32_t aux_,
                int *cycp_,
                int *slow_)
{
  uint32_t base;
  uint32_t tbas;
  uint32_t val;
  int      oper2;

  (void)aux_;

  if(!ARM_CACHE_COND_OK(cmd_))
    return;
  if(CPU.MAS_Access_Exept)
    {
      /* dangling abort flag (SWP-to-XBUS-abort-window can leave one):
       * the interpreter suppresses the load rotation AND takes the
       * data abort on the next SDT; the full handlers implement that
       * contract (rotation guard + arm_data_abort with the cycp_
       * charge), so a flagged state must not take the tight path. */
      arm_h_sdt_imm(cmd_,0,cycp_);
      *slow_ = 1;
      return;
    }

  base  = CPU.USER[((cmd_ >> 16) & 0xF)];
  oper2 = (cmd_ & 0x0FFF);

  if(!(cmd_ & (1 << 23)))
    oper2 = (0 - oper2);

  if(cmd_ & (1 << 24))
    tbas = base = (base + oper2);
  else
    {
      tbas = base;
      base = (base + oper2);
    }

  val = CPU.USER[(cmd_ >> 12) & 0xF];

  if(cmd_ & (1 << 22))
    {
      if((tbas >= (uint32_t)RAM_SIZE) ||
         (HIRESMODE && (tbas >= (uint32_t)DRAM_SIZE)))
        {
          arm_h_sdt_imm(cmd_,0,cycp_);
          *slow_ = 1;
          return;
        }
      opera_mem_write8(tbas,val);
    }
  else
    {
      if(((tbas & ~3u) >= (uint32_t)RAM_SIZE) ||
         (HIRESMODE && ((tbas & ~3u) >= (uint32_t)DRAM_SIZE)))
        {
          arm_h_sdt_imm(cmd_,0,cycp_);
          *slow_ = 1;
          return;
        }
      opera_mem_write32((tbas & ~3u),val);
    }

  (*cycp_) -= (-SCYCLE + 2 * NCYCLE);

  if((cmd_ & (1 << 21)) || (!(cmd_ & (1 << 24))))
    CPU.USER[(cmd_ >> 16) & 0xF] = base;
}

/* register-offset twin of arm_h_sdt_sti_t: oper2 = shifted USER[rm]
 * (arm_h_sdt_ri_np's line, imm12 replaced).  Same DRAM-window slow
 * gate, same store + cycle charge, never touches control state. */
static
OPERA_FORCEINLINE
void
arm_h_sdt_str_t(uint32_t cmd_,
                uint32_t aux_,
                int *cycp_,
                int *slow_)
{
  uint32_t base;
  uint32_t tbas;
  uint32_t val;
  uint32_t oper2;

  if(!ARM_CACHE_COND_OK(cmd_))
    return;
  if(CPU.MAS_Access_Exept)
    {
      /* dangling abort flag (SWP-to-XBUS-abort-window can leave one):
       * the interpreter suppresses the load rotation AND takes the
       * data abort on the next SDT; the full handlers implement that
       * contract (rotation guard + arm_data_abort with the cycp_
       * charge), so a flagged state must not take the tight path. */
      arm_h_sdt_ri_np(cmd_,aux_,cycp_);
      *slow_ = 1;
      return;
    }

  base  = CPU.USER[((cmd_ >> 16) & 0xF)];
  oper2 = ARM_SHIFT_NSC(CPU.USER[cmd_ & 0xF],
                        (aux_ & 0x3F),((aux_ >> 8) & 0x7));

  if(!(cmd_ & (1 << 23)))
    oper2 = (0 - oper2);

  if(cmd_ & (1 << 24))
    tbas = base = (base + oper2);
  else
    {
      tbas = base;
      base = (base + oper2);
    }

  val = CPU.USER[(cmd_ >> 12) & 0xF];

  if(cmd_ & (1 << 22))
    {
      if((tbas >= (uint32_t)RAM_SIZE) ||
         (HIRESMODE && (tbas >= (uint32_t)DRAM_SIZE)))
        {
          arm_h_sdt_ri_np(cmd_,aux_,cycp_);
          *slow_ = 1;
          return;
        }
      opera_mem_write8(tbas,val);
    }
  else
    {
      if(((tbas & ~3u) >= (uint32_t)RAM_SIZE) ||
         (HIRESMODE && ((tbas & ~3u) >= (uint32_t)DRAM_SIZE)))
        {
          arm_h_sdt_ri_np(cmd_,aux_,cycp_);
          *slow_ = 1;
          return;
        }
      opera_mem_write32((tbas & ~3u),val);
    }

  (*cycp_) -= (-SCYCLE + 2 * NCYCLE);

  if((cmd_ & (1 << 21)) || (!(cmd_ & (1 << 24))))
    CPU.USER[(cmd_ >> 16) & 0xF] = base;
}

static
OPERA_FORCEINLINE
void
arm_h_sdt_ldi_t(uint32_t cmd_,
                uint32_t aux_,
                int *cycp_,
                int *slow_)
{
  uint32_t base;
  uint32_t tbas;
  uint32_t val;
  uint32_t rora;
  int      oper2;

  (void)aux_;

  if(!ARM_CACHE_COND_OK(cmd_))
    return;
  if(CPU.MAS_Access_Exept)
    {
      /* dangling abort flag (SWP-to-XBUS-abort-window can leave one):
       * the interpreter suppresses the load rotation AND takes the
       * data abort on the next SDT; the full handlers implement that
       * contract (rotation guard + arm_data_abort with the cycp_
       * charge), so a flagged state must not take the tight path. */
      arm_h_sdt_imm(cmd_,0,cycp_);
      *slow_ = 1;
      return;
    }

  base  = CPU.USER[((cmd_ >> 16) & 0xF)];
  oper2 = (cmd_ & 0x0FFF);

  if(!(cmd_ & (1 << 23)))
    oper2 = (0 - oper2);

  if(cmd_ & (1 << 24))
    tbas = base = (base + oper2);
  else
    {
      tbas = base;
      base = (base + oper2);
    }

  if(cmd_ & (1 << 22))
    {
      if(tbas >= (uint32_t)RAM_SIZE)
        {
          arm_h_sdt_imm(cmd_,0,cycp_);
          *slow_ = 1;
          return;
        }
      val = opera_mem_read8(tbas);
    }
  else
    {
      rora = (tbas & 3);
      if((tbas & ~3u) >= (uint32_t)RAM_SIZE)
        {
          arm_h_sdt_imm(cmd_,0,cycp_);
          *slow_ = 1;
          return;
        }
      val = opera_mem_read32(tbas & ~3u);
      if(rora)
        val = ROTR(val,rora*8);
    }

  (*cycp_) -= (NCYCLE + ICYCLE);

  if((cmd_ & (1 << 21)) || (!(cmd_ & (1 << 24))))
    CPU.USER[(cmd_ >> 16) & 0xF] = base;

  CPU.USER[(cmd_ >> 12) & 0xF] = val;
}

static void arm_h_sdt_ri(uint32_t cmd_,uint32_t aux_,int *cycp_);

static
OPERA_FORCEINLINE
void
arm_h_sdt_ldr_t(uint32_t cmd_,
                uint32_t aux_,
                int *cycp_,
                int *slow_)
{
  uint32_t base;
  uint32_t tbas;
  uint32_t val;
  uint32_t rora;
  int      oper2;

  if(!ARM_CACHE_COND_OK(cmd_))
    return;
  if(CPU.MAS_Access_Exept)
    {
      /* dangling abort flag (SWP-to-XBUS-abort-window can leave one):
       * the interpreter suppresses the load rotation AND takes the
       * data abort on the next SDT; the full handlers implement that
       * contract (rotation guard + arm_data_abort with the cycp_
       * charge), so a flagged state must not take the tight path. */
      arm_h_sdt_ri(cmd_,aux_,cycp_);
      *slow_ = 1;
      return;
    }

  oper2 = ARM_SHIFT_NSC(CPU.USER[cmd_ & 0xF],(aux_ & 0x3F),((aux_ >> 8) & 0x7));

  base  = CPU.USER[((cmd_ >> 16) & 0xF)];

  if(!(cmd_ & (1 << 23)))
    oper2 = (0 - oper2);

  if(cmd_ & (1 << 24))
    tbas = base = (base + oper2);
  else
    {
      tbas = base;
      base = (base + oper2);
    }

  if(cmd_ & (1 << 22))
    {
      if(tbas >= (uint32_t)RAM_SIZE)
        {
          arm_h_sdt_ri(cmd_,aux_,cycp_);
          *slow_ = 1;
          return;
        }
      val = opera_mem_read8(tbas);
    }
  else
    {
      rora = (tbas & 3);
      if((tbas & ~3u) >= (uint32_t)RAM_SIZE)
        {
          arm_h_sdt_ri(cmd_,aux_,cycp_);
          *slow_ = 1;
          return;
        }
      val = opera_mem_read32(tbas & ~3u);
      if(rora)
        val = ROTR(val,rora*8);
    }

  (*cycp_) -= (NCYCLE + ICYCLE);

  if((cmd_ & (1 << 21)) || (!(cmd_ & (1 << 24))))
    CPU.USER[(cmd_ >> 16) & 0xF] = base;

  CPU.USER[(cmd_ >> 12) & 0xF] = val;
}

static
OPERA_FORCEINLINE
void
arm_h_sdt_ri(uint32_t cmd_,
             uint32_t aux_,
             int *cycp_)
{
  uint32_t oper2;
  uint32_t pc_tmp;

  if(!ARM_CACHE_COND_OK(cmd_))
    return;

  pc_tmp        = CPU.USER[15];
  CPU.USER[15] += 4;

  oper2 = ARM_SHIFT_NSC(CPU.USER[cmd_ & 0xF],(aux_ & 0x3F),((aux_ >> 8) & 0x7));

  arm_sdt_body(cmd_,oper2,pc_tmp,cycp_);
}

static
OPERA_FORCEINLINE
void
arm_h_sdt_rs(uint32_t cmd_,
             uint32_t aux_,
             int *cycp_)
{
  uint8_t  shift;
  uint32_t oper2;
  uint32_t pc_tmp;

  (void)aux_;

  if(!ARM_CACHE_COND_OK(cmd_))
    return;

  pc_tmp        = CPU.USER[15];
  CPU.USER[15] += 4;

  shift         = ((cmd_ >> 8) & 0xF);
  shift         = (CPU.USER[shift] & 0xFF);
  CPU.USER[15] += 4;

  oper2 = ARM_SHIFT_NSC(CPU.USER[cmd_ & 0xF],shift,((cmd_ >> 5) & 0x3));

  arm_sdt_body(cmd_,oper2,pc_tmp,cycp_);
}

static
OPERA_FORCEINLINE
void
arm_h_mul(uint32_t cmd_,
          uint32_t aux_,
             int *cycp_)
{
  uint32_t res;

  (void)aux_;

  if(!ARM_CACHE_COND_OK(cmd_))
    return;

  res = ((calcbits(CPU.USER[(cmd_ >> 8) & 0xF]) + 5) >> 1) - 1;
  if(res > 16)
    (*cycp_) -= 16;
  else
    (*cycp_) -= res;

  if(((cmd_ >> 16) & 0xF) == (cmd_ & 0xF))
    {
      if(cmd_ & (1 << 21))
        {
          CPU.USER[15] += 8;
          res           = CPU.USER[(cmd_ >> 12) & 0xF];
          CPU.USER[15] -= 8;
        }
      else
        {
          res = 0;
        }
    }
  else
    {
      if(cmd_ & (1 << 21))
        {
          res           = CPU.USER[cmd_ & 0xF] * CPU.USER[(cmd_ >> 8) & 0xF];
          CPU.USER[15] += 8;
          res          += CPU.USER[(cmd_ >> 12) & 0xF];
          CPU.USER[15] -= 8;
        }
      else
        {
          res = CPU.USER[cmd_ & 0xF] * CPU.USER[(cmd_ >> 8) & 0xF];
        }
    }

  if(cmd_ & (1 << 20))
    ARM_SET_ZN(res);

  CPU.USER[(cmd_ >> 16) & 0xF] = res;
}

static
OPERA_FORCEINLINE
void
arm_h_mul_np(uint32_t cmd_,
             uint32_t aux_,
             int *cycp_)
{
  uint32_t res;

  (void)aux_;

  if(!ARM_CACHE_COND_OK(cmd_))
    return;

  res = ((calcbits(CPU.USER[(cmd_ >> 8) & 0xF]) + 5) >> 1) - 1;
  if(res > 16)
    (*cycp_) -= 16;
  else
    (*cycp_) -= res;

  if(((cmd_ >> 16) & 0xF) == (cmd_ & 0xF))
    {
      if(cmd_ & (1 << 21))
        res = CPU.USER[(cmd_ >> 12) & 0xF];
      else
        res = 0;
    }
  else
    {
      if(cmd_ & (1 << 21))
        res  = CPU.USER[cmd_ & 0xF] * CPU.USER[(cmd_ >> 8) & 0xF]
             + CPU.USER[(cmd_ >> 12) & 0xF];
      else
        res  = CPU.USER[cmd_ & 0xF] * CPU.USER[(cmd_ >> 8) & 0xF];
    }

  if(cmd_ & (1 << 20))
    ARM_SET_ZN(res);

  CPU.USER[(cmd_ >> 16) & 0xF] = res;
}

static
OPERA_FORCEINLINE
void
arm_h_sds(uint32_t cmd_,
          uint32_t aux_,
             int *cycp_)
{
  (void)aux_;

  if(!ARM_CACHE_COND_OK(cmd_))
    return;

  ARM_SWAP(cmd_);
  (*cycp_) -= (2 * NCYCLE + ICYCLE);
}

static
OPERA_FORCEINLINE
void
arm_h_bdt(uint32_t cmd_,
          uint32_t aux_,
             int *cycp_)
{
  (void)aux_;

  if(!ARM_CACHE_COND_OK(cmd_))
    return;

  CYCLES = (*cycp_);
  bdt_core(cmd_);
  (*cycp_) = CYCLES;

  if(CPU.MAS_Access_Exept)
    {
      CYCLES = (*cycp_);
      arm_data_abort();
      (*cycp_) = CYCLES;
    }
}

static
OPERA_FORCEINLINE
void
arm_h_branch(uint32_t cmd_,
             uint32_t aux_,
             int *cycp_)
{
  (void)aux_;

  if(!ARM_CACHE_COND_OK(cmd_))
    return;

  if(cmd_ & (1 << 24))
    CPU.USER[14] = CPU.USER[15];
  CPU.USER[15] += ((((cmd_ & 0x00FFFFFF) |
                      ((cmd_ & 0x00800000) ? 0xFF000000 : 0)) << 2) + 4);

  (*cycp_) -= (SCYCLE + NCYCLE);
}

static
OPERA_FORCEINLINE
void
arm_h_swi(uint32_t cmd_,
          uint32_t aux_,
             int *cycp_)
{
  (void)aux_;

  if(!ARM_CACHE_COND_OK(cmd_))
    return;

  CYCLES = (*cycp_);
  decode_swi(cmd_);
  (*cycp_) = CYCLES;
}

static
OPERA_FORCEINLINE
void
arm_h_und(uint32_t cmd_,
          uint32_t aux_,
             int *cycp_)
{
  (void)aux_;

  if(!ARM_CACHE_COND_OK(cmd_))
    return;

  CPU.SPSR[arm_mode_table[0x1b]] = CPU.CPSR;
  SETI(1);
  SETM(0x1b);
  CPU.USER[14] = CPU.USER[15];
  CPU.USER[15] = 0x00000004;
  (*cycp_)      -= (SCYCLE + NCYCLE);
}

static
OPERA_FORCEINLINE
void
arm_h_spec(uint32_t cmd_,
           uint32_t aux_,
             int *cycp_)
{
  if(CPU.CPSR == 0x80000093)
    return;

  arm_h_sdt_imm(cmd_,aux_,cycp_);
}

/* Resolve the static shift operand exactly like the interpreter's
 * shift-amount-zero promotion (shtype 3 -> 4 i.e. RRX, others -> 32). */
static
INLINE
uint32_t
arm_cache_pack_shift(uint32_t cmd_)
{
  uint8_t shift;
  uint8_t shtype;

  shtype = ((cmd_ >> 5) & 0x3);
  shift  = ((cmd_ >> 7) & 0x1F);

  if(!shift)
    {
      if(shtype)
        {
          if(shtype == 3)
            shtype++;
          else
            shift = 32;
        }
    }

  return (uint32_t)shift | ((uint32_t)shtype << 8);
}

static
void
arm_cache_decode(arm_cache_entry_t *e_,
                 uint32_t           cmd_)
{
  const uint32_t top = ((cmd_ >> 24) & 0xF);
  const uint32_t opc = ((cmd_ >> 20) & 0x1F);
  const int      msr = ((opc == 18) || (opc == 22));
  const int      rd_np = (((cmd_ >> 12) & 0xF) != 0xF);
  const int      rn_np = (((cmd_ >> 16) & 0xF) != 0xF);
  const int      rs_np = (((cmd_ >>  8) & 0xF) != 0xF);
  const int      rm_np = ((cmd_         & 0xF) != 0xF);

  e_->word   = cmd_;
  e_->sealed = 1;
  e_->aux    = 0;
  e_->cls    = 0;

  if(cmd_ == 0xE5101810)
    {
      e_->cls = ARM_CLS_SPEC;
      return;
    }

  switch(top)
    {
    case 0x0:
    case 0x1:
      if((cmd_ & ARM_MUL_MASK) == ARM_MUL_SIGN)
        {
          if(rn_np && rd_np && rs_np && rm_np)
            e_->cls = ARM_CLS_TIGHT_MUL;
          else
            e_->cls = ARM_CLS_MUL;
        }
      else if((cmd_ & ARM_SDS_MASK) == ARM_SDS_SIGN)
        {
          e_->cls = ARM_CLS_SDS;
        }
      else if((cmd_ & 0x2000090) != 0x90)
        {
          if(cmd_ & (1 << 4))
            {
              if(rd_np && rn_np && rs_np && rm_np)
                e_->cls = (msr ? ARM_CLS_DP_RS_NP : ARM_CLS_TIGHT_DP_RS);
              else
                e_->cls = ARM_CLS_DP_RS;
            }
          else
            {
              uint32_t const opc5 = ((cmd_ >> 20) & 0x1F);

              if(rd_np && rn_np && rm_np)
                e_->cls = (msr ? ARM_CLS_DP_RI_NP : ARM_CLS_TIGHT_DP_RI);
              else if(rd_np && rn_np && ((cmd_ & 0xF) == 0xF) &&
                      !(cmd_ & (1 << 20)) &&
                      !((opc5 == 8) || (opc5 == 9) || (opc5 == 10) ||
                        (opc5 == 11) || (opc5 == 16) || (opc5 == 18) ||
                        (opc5 == 20) || (opc5 == 22)))
                e_->cls = ARM_CLS_TIGHT_DP_RMPC;  /* op2 = pc_k + 8:
                                                   * USER[15] after the
                                                   * pipeline bump; the
                                                   * shifter may still
                                                   * rotate the constant
                                                   * but stays constant */
              else if(rd_np && (((cmd_ >> 16) & 0xF) == 0xF) &&
                      ((cmd_ & 0xF) != 0xF) && !msr)
                e_->cls = ARM_CLS_TIGHT_DP_PCREL;   /* op1 = pc+8; rm==15
                                                     * (op2 = pc+8 too)
                                                     * stays full-class */
              else if(rn_np && (((cmd_ >> 12) & 0xF) == 0xF) &&
                      ((cmd_ & 0xF) != 0xF) &&   /* rm==15 reads the
                                                   * pipeline pc as the
                                                   * source value: full */
                      !(cmd_ & (1 << 20)) &&
                      !((opc5 == 8) || (opc5 == 9) || (opc5 == 10) ||
                        (opc5 == 11) || (opc5 == 16) || (opc5 == 18) ||
                        (opc5 == 20) || (opc5 == 22)))
                e_->cls = ARM_CLS_TIGHT_DP_PC;      /* mov pc, rm forms */
              else
                e_->cls = ARM_CLS_DP_RI;
              e_->aux = arm_cache_pack_shift(cmd_);
            }
        }
      else
        {
          /* mul/swap-like words matching neither signature fall through
           * the interpreter's 6/7-UND check (never matches here) into
           * the SDT immediate body */
          e_->cls = ((rd_np && rn_np) ? ARM_CLS_SDT_IMM_NP : ARM_CLS_SDT_IMM);
        }
      break;

    case 0x2:
    case 0x3:
      /* bit25 always set: immediate data processing; the interpreter's
       * (cmd&0x2000090)!=0x90 guard can never fail for these */
      if(rn_np && rd_np)
        e_->cls = (msr ? ARM_CLS_DP_IMM_NP : ARM_CLS_TIGHT_DP_IMM);
      else if(rn_np && (((cmd_ >> 12) & 0xF) == 0xF) &&
              !(cmd_ & (1 << 20)) &&
              !((opc == 8) || (opc == 9) || (opc == 10) || (opc == 11) ||
                (opc == 16) || (opc == 18) || (opc == 20) || (opc == 22)))
        e_->cls = ARM_CLS_TIGHT_DP_PC;   /* real rd==15 data writes only:
                                          * excludes the compare family
                                          * (8-11, no rd write) and the
                                          * MRS/MSR aliases (16/18/20/22) */
      else if((!rn_np) && rd_np && (((cmd_ >> 16) & 0xF) == 0xF) && !msr)
        e_->cls = ARM_CLS_TIGHT_DP_PCREL;  /* ADD rX, pc, #imm etc: the
                                            * full handler reads op1 =
                                            * USER[15] = pc_k + 4 */
      else
        e_->cls = ARM_CLS_DP_IMM;
      e_->aux = ((cmd_ >> ARM_DP_IMM_ROT_AMOUNT_SHIFT) &
                 ARM_DP_IMM_ROT_AMOUNT_MASK);
      break;

    case 0x4:
    case 0x5:
      if(rn_np && rd_np &&
         ((cmd_ & (1 << 24)) || !(cmd_ & (1 << 21))))
        e_->cls = ((cmd_ & (1 << 20)) ? ARM_CLS_TIGHT_SDT_LDI
                                       : ARM_CLS_TIGHT_SDT_STI);
      else
        e_->cls = ((rn_np && rd_np) ? ARM_CLS_SDT_IMM_NP : ARM_CLS_SDT_IMM);
      break;

    case 0x6:
    case 0x7:
      if((cmd_ & ARM_UND_MASK) == ARM_UND_SIGN)
        {
          e_->cls = ARM_CLS_UND;
        }
      else
        {
          if((cmd_ & (1 << 20)) && rn_np && rd_np && rm_np &&
             ((cmd_ & (1 << 24)) || !(cmd_ & (1 << 21))))
            {
              e_->cls = ARM_CLS_TIGHT_SDT_LDR;
              e_->aux = arm_cache_pack_shift(cmd_);
            }
          else if(!(cmd_ & (1 << 20)) && rn_np && rd_np && rm_np &&
                  ((cmd_ & (1 << 24)) || !(cmd_ & (1 << 21))) &&
                  !(((cmd_ >> 5) & 0x3) == 3))  /* RRX (#0): the NSC
                    * shift writes carry_out -- full class only */
            {
              /* register-offset store: same poll-free store shape as
               * TIGHT_SDT_STI, only oper2 comes from a shifted
               * USER[rm] instead of the imm12 field */
              e_->cls = ARM_CLS_TIGHT_SDT_STR;
              e_->aux = arm_cache_pack_shift(cmd_);
            }
          else
            {
              e_->cls = ((rn_np && rd_np && rm_np) ? ARM_CLS_SDT_RI_NP
                                                   : ARM_CLS_SDT_RI);
              e_->aux = arm_cache_pack_shift(cmd_);
            }
        }
      break;

    case 0x8:
    case 0x9:
      e_->cls = ARM_CLS_BDT;
      break;

    case 0xa:
    case 0xb:
      e_->cls = ARM_CLS_TIGHT_BRANCH;
      break;

    case 0xf:
      e_->cls = ARM_CLS_SWI;
      break;

    default:
      e_->cls = ARM_CLS_UND;
      break;
    }
}

/* cached-engine fetch window: instruction fetches are amortized against
 * the last identified region.  Flushed on ROM swaps (rom_select) and at
 * init so a stale ROM pointer is never dereferenced. */
static const uint32_t          *g_fetch_mem;
static const arm_cache_entry_t *g_fetch_cache;
static uint32_t                 g_fetch_lo;
static uint32_t                 g_fetch_span;
static uint32_t                 g_arm_rom_seq   = 0;

void
opera_arm_fetch_window_flush(void)
{
  g_fetch_span = 0;
  g_arm_rom_seq++;

  /* ROM bank swaps change the code under every cached ROM block */
  opera_arm_jit_flush_all();
}

static
void
arm_fetch_window_set(uint32_t const pca_)
{
  uint32_t const idx = (pca_ ^ 0x03000000);

  if(pca_ < RAM_SIZE)
    {
      g_fetch_mem   = (const uint32_t*)DRAM;
      g_fetch_cache = g_arm_cache_ram;
      g_fetch_lo    = 0;
      g_fetch_span  = RAM_SIZE;
    }
  else if(!(idx & ~ROM1_SIZE_MASK))
    {
      g_fetch_mem   = (const uint32_t*)ROM;
      g_fetch_cache = g_arm_cache_rom;
      g_fetch_lo    = 0x03000000;
      g_fetch_span  = (ROM1_SIZE_MASK + 1);
    }
  else if(!((pca_ ^ 0x06000000) & ~ROM1_SIZE_MASK))
    {
      g_fetch_mem   = (const uint32_t*)ROM;
      g_fetch_cache = g_arm_cache_rom;
      g_fetch_lo    = 0x06000000;
      g_fetch_span  = (ROM1_SIZE_MASK + 1);
    }
  else
    {
      g_fetch_span = 0;
    }
}

/*
 * Execute ARM instructions until the accumulated cycle count reaches
 * budget_ (the caller's remaining headroom to the next CLOCK_STEP
 * boundary) or a per-instruction poll requests separation from the next
 * instruction.  Poll cadence and cycle accounting are identical to the
 * interpreter path: FIQ is checked after every instruction in the step
 * tail, and the MADAM-FSM / CD-ODE-restart polls run between instructions
 * exactly like opera_3do_process_frame() does.
 *
 * All hot loop state (fetch window, poll pointers, cycle accumulator) is
 * held in function locals so the compiler keeps it register-resident; the
 * global mirrors are only refreshed on classification misses, on ROM-swap
 * sequence bumps, and around the out-of-line BDT/SWI/interpreter helpers
 * that still write the file-scope CYCLES.
 */

static
OPERA_FORCEINLINE
void
arm_fiq_vector(void)
{
  CPU.nFIQ = false;
  CPU.SPSR[arm_mode_table[0x11]] = CPU.CPSR;
  SETF(1);
  SETI(1);
  SETM(0x11);
  CPU.USER[14] = (CPU.USER[15] + 4);
  CPU.USER[15] = 0x0000001C;
}

static uint64_t g_lane_seq  = 0;   /* slice counter (tracer + dbg) */

static void lane_trace_per_insn(int32_t const ret_,int32_t const budget_);

static
int32_t
arm_execute_slice(int32_t budget_)
{
  int32_t total = 0;
  int     cyc;

  const uint32_t          *mem;
  const arm_cache_entry_t *ec;
  uint32_t                 lo;
  uint32_t                 span;
  const uint32_t          *fpend;
  const uint32_t          *fsm;
  const bool              *rst;
  uint32_t                 seq;

  mem   = g_fetch_mem;
  ec    = g_fetch_cache;
  lo    = g_fetch_lo;
  span  = g_fetch_span;
  fpend = g_clio_fiqpend;
  fsm   = g_madam_fsm_poll;
  rst   = g_cdrom_restart_poll;
  seq   = g_arm_rom_seq;


  for(;;)
    {
      uint32_t          word;
      uint32_t          idx;
      arm_cache_entry_t *e;
      uint32_t const    pc  = CPU.USER[15];
      uint32_t const    pca = (pc & ~3u);

      idx = (pca - lo);
      if(idx >= span)
        {
          uint32_t const wi = (pca ^ 0x03000000);

          if(pca < RAM_SIZE)
            {
              mem  = (const uint32_t*)DRAM;
              ec   = g_arm_cache_ram;
              lo   = 0;
              span = RAM_SIZE;
            }
          else if(!(wi & ~ROM1_SIZE_MASK))
            {
              mem  = (const uint32_t*)ROM;
              ec   = g_arm_cache_rom;
              lo   = 0x03000000;
              span = (ROM1_SIZE_MASK + 1);
            }
          else if(!((pca ^ 0x06000000) & ~ROM1_SIZE_MASK))
            {
              mem  = (const uint32_t*)ROM;
              ec   = g_arm_cache_rom;
              lo   = 0x06000000;
              span = (ROM1_SIZE_MASK + 1);
            }
          else
            {
              int32_t const c = arm_execute_interp();
              total += c;

              if(g_arm_rom_seq != seq)
                {
                  seq  = g_arm_rom_seq;
                  span = 0;   /* force reclassification next fetch */
                }

              if(*rst)
                break;

              if(total >= budget_)
                break;

              if(*fsm == FSM_INPROCESS)
                break;

              continue;
            }

          idx = (pca - lo);
        }

      word = mem[idx >> 2];
      e    = (arm_cache_entry_t*)&ec[idx >> 2];

      CPU.USER[15] = (pc + 4);

      if(!e->sealed || (e->word != word))
        arm_cache_decode(e,word);

      cyc = -SCYCLE;
      switch(e->cls)
        {
        case ARM_CLS_DP_IMM:     arm_h_dp_imm(word,e->aux,&cyc);     break;
        case ARM_CLS_DP_RS:     arm_h_dp_rs(word,e->aux,&cyc);     break;
        case ARM_CLS_DP_RI:     arm_h_dp_ri(word,e->aux,&cyc);     break;
        case ARM_CLS_DP_IMM_NP:     arm_h_dp_imm_np(word,e->aux,&cyc);     break;
        case ARM_CLS_DP_RS_NP:     arm_h_dp_rs_np(word,e->aux,&cyc);     break;
        case ARM_CLS_DP_RI_NP:     arm_h_dp_ri_np(word,e->aux,&cyc);     break;
        case ARM_CLS_SDT_IMM:     arm_h_sdt_imm(word,e->aux,&cyc);     break;
        case ARM_CLS_SDT_IMM_NP:     arm_h_sdt_imm_np(word,e->aux,&cyc);     break;
        case ARM_CLS_SDT_RI:     arm_h_sdt_ri(word,e->aux,&cyc);     break;
        case ARM_CLS_SDT_RI_NP:     arm_h_sdt_ri_np(word,e->aux,&cyc);     break;
        case ARM_CLS_SDT_RS:     arm_h_sdt_rs(word,e->aux,&cyc);     break;
        case ARM_CLS_MUL:     arm_h_mul(word,e->aux,&cyc);     break;
        case ARM_CLS_BRANCH:     arm_h_branch(word,e->aux,&cyc);     break;
        case ARM_CLS_SPEC:     arm_h_spec(word,e->aux,&cyc);     break;
        case ARM_CLS_UND:     arm_h_und(word,e->aux,&cyc);     break;
        case ARM_CLS_SDS:     arm_h_sds(word,e->aux,&cyc);     break;
        case ARM_CLS_BDT:     arm_h_bdt(word,e->aux,&cyc);     break;
        case ARM_CLS_SWI:     arm_h_swi(word,e->aux,&cyc);     break;
        case ARM_CLS_TIGHT_DP_IMM:  arm_h_dp_imm_np(word,e->aux,&cyc); goto tight;
        /* pc-writing DP keeps the full handler + full tail here: the
         * -S writing class only exists for the JIT; the cache run must
         * still pay ICYCLE+NCYCLE and run the full-tail polls.  Both
         * operand forms exist: bit25 picks the handler. */
        case ARM_CLS_TIGHT_DP_PC:
          if(word & (1u << 25))
            arm_h_dp_imm(word,e->aux,&cyc);
          else
            arm_h_dp_ri(word,e->aux,&cyc);
          goto full;
        /* pc-relative op1: R15 as an operand reads instruction+8; the
         * slice loop pre-syncs USER[15] = pc+4, so bump it to the
         * pipeline value for the op1 read, then restore -- the tight
         * tail's FIQ path must observe USER[15] = pc+4 exactly like
         * every other tight word.  bit25 picks the operand decode. */
        case ARM_CLS_TIGHT_DP_PCREL:
          CPU.USER[15] += 4;
          if(word & (1u << 25))
            arm_h_dp_imm_np(word,e->aux,&cyc);
          else
            arm_h_dp_ri_np(word,e->aux,&cyc);
          CPU.USER[15] -= 4;
          goto tight;
        case ARM_CLS_TIGHT_DP_RS:  arm_h_dp_rs_np(word,e->aux,&cyc);  goto tight;
        case ARM_CLS_TIGHT_DP_RI:  arm_h_dp_ri_np(word,e->aux,&cyc);  goto tight;
        case ARM_CLS_TIGHT_DP_RMPC:
          /* op2 = pipeline pc: wrap the _np handler (no bump of its
           * own) so USER[15] reads pc+8 for the operand but stays at
           * pc+4 for the tight tail -- exactly like PCREL above */
          CPU.USER[15] += 4;
          arm_h_dp_ri_np(word,e->aux,&cyc);
          CPU.USER[15] -= 4;
          goto tight;
        case ARM_CLS_TIGHT_MUL:    arm_h_mul_np(word,e->aux,&cyc);    goto tight;
        case ARM_CLS_TIGHT_BRANCH: arm_h_branch(word,e->aux,&cyc);    goto tight;
        case ARM_CLS_TIGHT_SDT_LDI:
          {
            int slow = 0;
            arm_h_sdt_ldi_t(word,e->aux,&cyc,&slow);
            if(!slow)
              goto tight;
          }
          goto full;
        case ARM_CLS_TIGHT_SDT_LDR:
          {
            int slow = 0;
            arm_h_sdt_ldr_t(word,e->aux,&cyc,&slow);
            if(!slow)
              goto tight;
          }
          goto full;
        case ARM_CLS_TIGHT_SDT_STI:
          {
            int slow = 0;
            arm_h_sdt_sti_t(word,e->aux,&cyc,&slow);
            if(!slow)
              goto tight;
          }
          goto full;
        case ARM_CLS_TIGHT_SDT_STR:
          {
            /* register-offset store, same store semantics as STI: the
             * _np handler writes memory and never touches control
             * state; the slow gate mirrors sti_t's MMIO/VRAM check */
            int slow = 0;
            arm_h_sdt_str_t(word,e->aux,&cyc,&slow);
            if(!slow)
              goto tight;
          }
          goto full;
        }

full:
      /* full tail for non-tight classes */
      total -= cyc;


      if(g_SOFT_RESET_PENDING)
        {
          /* the interpreter returns here, before the FIQ check */
          g_SOFT_RESET_PENDING = false;
        }
      else if(!ISF && *fpend)
        arm_fiq_vector();

      lane_trace_per_insn(-cyc, budget_);

      if(*rst)
        break;

      if(total >= budget_)
        break;

      if(*fsm == FSM_INPROCESS)
        break;

      /* only classes capable of poking CLIO (or running SWI/UND/LDM-PC
       * out-of-line helpers) can have swapped ROM banks mid-instruction;
       * pure ALU/branch/mul forms cannot */
      if((e->cls >= ARM_CLS_SDT_IMM) && (g_arm_rom_seq != seq))
        {
          seq  = g_arm_rom_seq;
          span = 0;
        }

      continue;

tight:
      /* tight-run tail: these classes guarantee no store, no CPSR.I/F
       * rewrite, no trap and no strays through shared helpers, so
       * ROM-bank / MADAM-FSM / CD-ROM / soft-reset state is invariant
       * mid-run.  FIQ pending, budget and the two split-worthy polls
       * remain per-instruction; pc is still synced for the vector path
       * because arm_fiq_vector reads USER[15]. */
      total -= cyc;

      if(!ISF && *fpend)
        arm_fiq_vector();

      lane_trace_per_insn(-cyc, budget_);

      if(*rst)
        break;

      if(total >= budget_)
        break;

      if(*fsm == FSM_INPROCESS)
        break;

      continue;
    }

  return total;
}

/* dual-lane state tracer: per-slice CPU hash for engine-vs-engine
 * divergence localization (env OPERA_JIT_TRACE_LANES=<path>) */
static FILE *g_lane_trace   = NULL;

static int      g_lane_on   = 0;

static void
lane_trace_maybe_open(void)
{
  /* one-shot latch: this runs from opera_arm_execute_slice on EVERY slice
   * (~6.5K/frame).  A per-call getenv dominated the profile (~19%) — probe
   * the environment exactly once; absent means absent forever (the tracer
   * is a boot-time debug instrument, never enabled mid-run). */
  static int latched_ = 0;
  char const *p_;

  if(latched_)
    return;
  latched_ = 1;

  p_ = getenv("OPERA_JIT_TRACE_LANES");
  if(p_ && !g_lane_trace)
    {
      g_lane_trace = fopen(p_, "w");
      g_lane_on    = (g_lane_trace != NULL);
    }
}

static int32_t g_lane_budget;

static void
lane_trace_line(int32_t const ret_)
{
  uint64_t h_ = 1469598103934665603ULL;
  unsigned char const *b_ = (unsigned char const *)&CPU;
  uint32_t i_;
  if(!g_lane_on)
    return;
  for(i_ = 0; i_ < sizeof(CPU); i_++)
    {
      h_ ^= b_[i_];
      h_ *= 1099511628211ULL;
    }
  {
    uint32_t const nxt_ = (CPU.USER[15] & ~3u);
    uint32_t const lst_ = (nxt_ >= 4u) ? (nxt_ - 4u) : nxt_;
    fprintf(g_lane_trace, "%llu %d %08X %08X %016llx %08X %08X %d %016llx %016llx %016llx\n",
            (unsigned long long)g_lane_seq++, (int)ret_,
            CPU.USER[15], CPU.CPSR,
            (unsigned long long)h_,
            (nxt_ < RAM_SIZE) ? *(uint32_t *)&DRAM[nxt_] : 0xDEAD0001,
            (lst_ < RAM_SIZE) ? *(uint32_t *)&DRAM[lst_] : 0xDEAD0002,
            g_lane_budget,
            (unsigned long long)opera_clio_state_hash(),
            (unsigned long long)opera_madam_state_hash(),
            (unsigned long long)opera_dsp_state_hash());
  }
}

/* OPERA_TRACE_PER_INSN=1: emit a lane_trace line after EVERY instruction in
 * the cached engine (matches the interpreter's one-word-per-slice
 * granularity), for engine-differential stream comparison. */
static int g_lane_per_insn = -1;

static void
lane_trace_per_insn(int32_t const ret_,int32_t const budget_)
{
  if(!g_lane_on)
    return;
  if(g_lane_per_insn < 0)
    g_lane_per_insn = (getenv("OPERA_TRACE_PER_INSN") != NULL);
  if(!g_lane_per_insn)
    return;
  {
    int32_t const saved_ = g_lane_budget;
    g_lane_budget = budget_;
    lane_trace_line(ret_);
    g_lane_budget = saved_;
  }
}


int32_t
opera_arm_execute_slice(int32_t budget_)
{
  int32_t r_;
  lane_trace_maybe_open();
#ifdef OPERA_ARM_JIT_ENABLED
  if(g_arm_engine == 3)
    {
      r_ = arm_jit_exec_slice(budget_);
      g_lane_budget = budget_;
      lane_trace_line(r_);
      return r_;
    }
#endif

  if(g_arm_engine)
    {
      /* size-aware lazy gate: a state load whose embedded mem_cfg
       * differs from the session's re-sizes RAM_SIZE mid-run
       * (opera_mem_state_load applies it unconditionally); without
       * the staleness check the arrays keep the old word count and
       * the next fetch indexes past them (heap OOB).  arm_cache_alloc
       * no-ops when the size already matches, so the steady-state
       * cost is one compare. */
      if(((g_arm_cache_ram == NULL) ||
          (g_arm_cache_ram_words != (RAM_SIZE >> 2))) &&
         (g_arm_engine != 2))
        arm_cache_alloc();

      if(g_arm_cache_ram != NULL)
        {
          r_ = arm_execute_slice(budget_);
          g_lane_budget = budget_;
          lane_trace_line(r_);
          return r_;
        }

      g_arm_engine = 2;
    }

  r_ = arm_execute_interp();
  g_lane_budget = budget_;
  lane_trace_line(r_);
  return r_;
}

static
int32_t
arm_execute_interp(void)
{
  uint32_t op1;
  uint32_t op2;
  uint8_t shift;
  uint8_t shtype;
  uint32_t cmd;
  uint32_t pc_tmp;
  int isexeption;

  isexeption = false;
  cmd = mreadw(CPU.USER[15]);
  CPU.USER[15] += 4;

  CYCLES = -SCYCLE;
  if((cmd == 0xE5101810) && (CPU.CPSR == 0x80000093))
    isexeption = true;

  if(((cond_flags_cross[cmd >> 28] >> (CPU.CPSR >> 28)) & 1) &&
     (isexeption == false))
    {
      switch((cmd >> 24) & 0xF)
        {
        case 0x0:               //Multiply
          if((cmd & ARM_MUL_MASK) == ARM_MUL_SIGN)
            {
              uint32_t res = ((calcbits(CPU.USER[(cmd>>8)&0xf])+5)>>1)-1;
              if(res > 16)
                CYCLES -= 16;
              else
                CYCLES -= res;

              if(((cmd >> 16) & 0xF) == (cmd & 0xF))
                {
                  if(cmd & (1 << 21))
                    {
                      CPU.USER[15] += 8;
                      res=CPU.USER[(cmd >> 12) & 0xF];
                      CPU.USER[15] -= 8;
                    }
                  else
                    {
                      res = 0;
                    }
                }
              else
                {
                  if(cmd & (1 << 21))
                    {
                      res = CPU.USER[cmd & 0xF] * CPU.USER[(cmd >> 8) & 0xF];
                      CPU.USER[15] += 8;
                      res += CPU.USER[(cmd >> 12) & 0xF];
                      CPU.USER[15] -= 8;
                    }
                  else
                    {
                      res = CPU.USER[cmd & 0xF] * CPU.USER[(cmd >> 8) & 0xF];
                    }
                }

              if(cmd & (1 << 20))
                ARM_SET_ZN(res);

              CPU.USER[(cmd >> 16) & 0xF] = res;
              break;
            }
        case 0x1:               //Single Data Swap
          if((cmd & ARM_SDS_MASK) == ARM_SDS_SIGN)
            {
              ARM_SWAP(cmd);
              //if(MAS_Access_Exept)
              CYCLES -= (2 * NCYCLE + ICYCLE);
              break;
            }
        case 0x2:               //ALU
        case 0x3:
          {
            if((cmd & 0x2000090) != 0x90)
              {
                /* SHIFT */
                pc_tmp = CPU.USER[15];
                CPU.USER[15] += 4;
                if(cmd & (1 << 25))
                  {
                    op2 = cmd & ARM_DP_IMM_MASK;
                    op2 = ARM_SHIFT_NSC(op2,
                                        ((cmd >> ARM_DP_IMM_ROT_AMOUNT_SHIFT) &
                                         ARM_DP_IMM_ROT_AMOUNT_MASK),
                                        ARM_SHIFT_TYPE_ROR);
                    op1 = CPU.USER[(cmd >> 16) & 0xF];
                  }
                else
                  {
                    shtype = ((cmd >> 5) & 0x3);
                    if(cmd & (1 << 4))
                      {
                        shift = ((cmd >> 8) & 0xF);
                        shift = (CPU.USER[shift] & 0xFF);
                        CPU.USER[15] += 4;
                        op2 = CPU.USER[cmd & 0xF];
                        op1 = CPU.USER[(cmd >> 16) & 0xF];
                        CYCLES -= ICYCLE;
                      }
                    else
                      {
                        shift = ((cmd >> 7) & 0x1F);

                        if(!shift)
                          {
                            if(shtype)
                              {
                                if(shtype == 3)
                                  shtype++;
                                else
                                  shift=32;
                              }
                          }

                        op2 = CPU.USER[cmd & 0xF];
                        op1 = CPU.USER[(cmd >> 16) & 0xF];
                      }

                    //if((cmd&(1<<20)) && is_logic[((cmd>>21)&0xf)] ) op2=ARM_SHIFT_SC(op2, shift, shtype);
                    //else
                    op2 = ARM_SHIFT_NSC(op2,shift,shtype);
                  }

                CPU.USER[15] = pc_tmp;

                if((cmd & (1 << 20)) && is_logic[((cmd >> 21) & 0xF)])
                  ARM_SET_C(carry_out);

                if(ARM_ALU_Exec(cmd,((cmd >> 20) & 0x1F),op1,op2,&CPU.USER[(cmd >> 12) & 0xF]))
                  break;

                if(((cmd >> 12) & 0xF) == 0xF) //destination = pc, take care of cpsr
                  {
                    if(cmd & (1 << 20))
                      arm_cpsr_set(CPU.SPSR[arm_mode_table[MODE]]);

                    CYCLES -= (ICYCLE + NCYCLE);
                  }
                break;
              }
          }
        case 0x6:               //Undefined
        case 0x7:
        Undefine:
          if((cmd & ARM_UND_MASK) == ARM_UND_SIGN)
            {
              CPU.SPSR[arm_mode_table[0x1b]] = CPU.CPSR;
              SETI(1);
              SETM(0x1b);
              CPU.USER[14] = CPU.USER[15];
              CPU.USER[15] = 0x00000004;
              CYCLES -= (SCYCLE + NCYCLE); // +2S+1N
              break;
            }
        case 0x4:               //Single Data Transfer
        case 0x5:
          if((cmd & 0x2000090) != 0x2000090)
            {
              uint32_t base;
              uint32_t tbas;
              uint32_t oper2;
              uint32_t val;
              uint32_t rora;

              pc_tmp = CPU.USER[15];
              CPU.USER[15] += 4;
              if(cmd & (1 << 25))
                {
                  shtype = ((cmd >> 5) & 0x3);
                  if(cmd & (1 << 4))
                    {
                      shift = ((cmd >> 8) & 0xF);
                      shift = (CPU.USER[shift] & 0xFF);
                      CPU.USER[15] += 4;
                    }
                  else
                    {
                      shift = ((cmd >> 7) & 0x1F);
                      if(!shift)
                        {
                          if(shtype)
                            {
                              if(shtype == 3)
                                shtype++;
                              else
                                shift = 32;
                            }
                        }
                    }

                  oper2 = ARM_SHIFT_NSC(CPU.USER[cmd & 0xF],shift,shtype);
                }
              else
                {
                  oper2 = (cmd & 0x0FFF);
                }

              tbas = base = CPU.USER[((cmd >> 16) & 0xF)];

              if(!(cmd & (1 << 23)))
                oper2 = (0 - oper2);

              if(cmd & (1 << 24))
                tbas = base = (base + oper2);
              else
                base = (base + oper2);

              if(cmd & (1 << 20)) //load
                {
                  if(cmd & (1 << 22)) //bytes
                    {
                      val = mreadb(tbas);
                    }
                  else //words/halfwords
                    {
                      rora = tbas & 3;
                      val = mreadw(tbas);

                      if(rora && !CPU.MAS_Access_Exept)
                        val = ROTR(val,rora*8);
                    }

                  CPU.USER[15] = pc_tmp;
                  if(CPU.MAS_Access_Exept)
                    {
                      arm_data_abort();
                      break;
                    }

                  if(((cmd >> 12) & 0xF) == 0xF)
                    CYCLES -= (SCYCLE + NCYCLE);   // +1S+1N ifR15 load

                  CYCLES -= (NCYCLE + ICYCLE);  // +1N+1I

                  if((cmd & (1 << 21)) || (!(cmd & (1 << 24))))
                    CPU.USER[(cmd >> 16) & 0xF] = base;

                  if((cmd & (1 << 21)) && !(cmd & (1 << 24)))
                    loadusr((cmd >> 12) & 0xF,val);
                  else
                    CPU.USER[(cmd >> 12) & 0xF] = val;
                }
              else // store
                {
                  if((cmd & (1 << 21)) && !(cmd & (1 << 24)))
                    val = readusr((cmd >> 12) & 0xF);
                  else
                    val = CPU.USER[(cmd >> 12) & 0xF];

                  CPU.USER[15] = pc_tmp;
                  CYCLES -= (-SCYCLE + 2 * NCYCLE);  // 2N

                  if(cmd & (1 << 22)) //bytes/words
                    mwriteb(tbas,val);
                  else //words/halfwords
                    mwritew(tbas,val);

                  if(g_SOFT_RESET_PENDING)
                    break;

                  if(CPU.MAS_Access_Exept)
                    {
                      arm_data_abort();
                      break;
                    }

                  if((cmd & (1 << 21)) || !(cmd & (1 << 24)))
                    CPU.USER[(cmd >> 16) & 0xF] = base;
                }

              break;
            }
          else
            {
              goto Undefine;
            }

        case 0x8:               //Block Data Transfer
        case 0x9:
          bdt_core(cmd);
          if(CPU.MAS_Access_Exept)
            {
              arm_data_abort();
              break;
            }
          break;

        case 0xa:               //BRANCH
        case 0xb:
          if(cmd & (1 << 24))
            CPU.USER[14] = CPU.USER[15];
          CPU.USER[15] += ((((cmd & 0x00FFFFFF) | ((cmd & 0x00800000) ? 0xFF000000 : 0)) << 2) + 4);

          CYCLES -= (SCYCLE + NCYCLE); //2S+1N
          break;

        case 0xf:               //SWI
          decode_swi(cmd);
          break;

        default:                //coprocessor
          CPU.SPSR[arm_mode_table[0x1b]] = CPU.CPSR;
          SETI(1);
          SETM(0x1b);
          CPU.USER[14] = CPU.USER[15];
          CPU.USER[15] = 0x00000004;
          CYCLES -= (SCYCLE + NCYCLE);
          break;
        }
    }

  if(g_SOFT_RESET_PENDING)
    {
      g_SOFT_RESET_PENDING = false;
      return -CYCLES;
    }

  if(!ISF && opera_clio_fiq_needed()/*CPU.nFIQ*/)
    {
      //Set_madam_FSM(FSM_SUSPENDED);
      CPU.nFIQ = false;
      CPU.SPSR[arm_mode_table[0x11]] = CPU.CPSR;
      SETF(1);
      SETI(1);
      SETM(0x11);
      CPU.USER[14] = (CPU.USER[15] + 4);
      CPU.USER[15] = 0x0000001C;
    }

  return -CYCLES;
}

/* Compat one-instruction entry point (no in-tree callers; the hot path is
 * opera_arm_execute_slice).  Routes every engine through the slice loop
 * with a 1-cycle budget: the slice executes at least one full word and
 * checks the budget only after it, so interp/cache run exactly one word.
 * Under the jit the budget-batching lever may let the rest of a block run
 * out -- single-word exactness there requires OPERA_JIT_BUDGET_BATCH=0
 * (the same caveat as every slice).  The previous dedicated cached-step
 * wrapper was removed: its class switch never gained the TIGHT_* cases,
 * so hot words executed as no-ops through it. */
int32_t
opera_arm_execute(void)
{
  return opera_arm_execute_slice(1);
}

static
void
mwritew(uint32_t const addr_,
        uint32_t const val_)
{
   uint32_t index;
   uint32_t const addr = (addr_ & ~3);

   if(addr < RAM_SIZE)
   {
      opera_mem_write32(addr,val_);
      return;
   }

   index = (addr ^ 0x03300000);
   if(!(index & ~0x7FF))
   {
      opera_madam_poke(index,val_);
      return;
   }

   index = (addr ^ 0x03400000);
   if(!(index & ~0xFFFF))
   {
      if(clio_xbus_access_aborts(index))
        {
          CPU.MAS_Access_Exept = true;
          return;
        }

      if(opera_clio_poke(index,val_))
        {
          opera_3do_soft_reset();
          g_SOFT_RESET_PENDING = true;
        }
      return;
   }

   index = (addr ^ 0x03200000);
   if(!(index & ~0xFFFFF))
   {
      opera_sport_write_access(index,val_);
      return;
   }

   index = (addr ^ 0x03100000);
   if(!(index & ~0xFFFFF))
   {
      if(index & 0x80000)
         opera_diag_port_send(val_);
      else if(index & 0x40000)
         NVRAM[(index >> 2) & NVRAM_SIZE_MASK] = (uint8_t)val_;
   }
}

static
uint32_t
mreadw(uint32_t const addr_)
{
  int32_t index;
  uint32_t const addr = (addr_ & ~3);

  if(addr < RAM_SIZE)
    return opera_mem_read32(addr);

  index = (addr ^ 0x03300000);
  if(!(index & ~0xFFFFF))
    return opera_madam_peek(index);

  index = (addr ^ 0x03400000);
  if(!(index & ~0xFFFFF))
    {
      if(clio_xbus_access_aborts(index))
        {
          CPU.MAS_Access_Exept = true;
          return 0;
        }

      return opera_clio_peek(index);
    }

  index = (addr ^ 0x03200000);
  if(!(index & ~0xFFFFF))
    {
      if(!(index & ~0x1FFF))
        return (opera_sport_set_source(index),0);
      return 0xBADACCE5;
    }

  /* Standard ROM */
  index = (addr ^ 0x03000000);
  if(!(index & ~ROM1_SIZE_MASK))
    return *(uint32_t*)&ROM[index];

  /* ANVIL ROM */
  index = (addr ^ 0x06000000);
  if(!(index & ~ROM1_SIZE_MASK))
    return *(uint32_t*)&ROM[index];

  index = (addr ^ 0x03100000);
  if(!(index & ~0xFFFFF))
    {
      if(index & 0x80000)
        return opera_diag_port_get();
      else if(index & 0x40000)
        return NVRAM[(index >> 2) & NVRAM_SIZE_MASK];
    }

  /* MAS_Access_Exept = true; */

  return 0xBADACCE5;
}

static
void
mwriteb(uint32_t const addr_,
        uint8_t  const val_)
{
  int32_t index;

  if(addr_ < RAM_SIZE)
  {
    opera_mem_write8(addr_,val_);
    return;
  }

  index = (addr_ ^ 0x03100003);
  if(!(index & ~0xFFFFF))
  {
     if((index & 0x40000) == 0x40000)
     {
        NVRAM[(index >> 2) & NVRAM_SIZE_MASK] = val_;
        return;
     }
  }
}

static
uint32_t
mreadb(uint32_t const addr_)
{
  int32_t index;

  if(addr_ < RAM_SIZE)
    return opera_mem_read8(addr_);

  /* Standard ROM */
  index = (addr_ ^ 0x03000003);
  if(!(index & ~ROM1_SIZE_MASK))
    return ROM[index];

  /* ANVIL ROM */
  index = (addr_ ^ 0x06000003);
  if(!(index & ~ROM1_SIZE_MASK))
    return ROM[index];

  index = (addr_ ^ 0x03100003);
  if(!(index & ~0xFFFFF))
    {
      if((index & 0x40000) == 0x40000)
        return NVRAM[(index >> 2) & NVRAM_SIZE_MASK];
    }

  /* MAS_Access_Exept = true; */

  return 0xBADACCE5;
}

static
void
loadusr(uint32_t const n_,
        uint32_t const val_)
{
  if(n_ == 15)
    {
      CPU.USER[15] = val_;
      return;
    }

  switch(arm_mode_table[(CPU.CPSR & 0x1F) | 0x10])
    {
    case ARM_MODE_USER:
      CPU.USER[n_] = val_;
      break;
    case ARM_MODE_FIQ:
      if(n_ > 7)
        CPU.CASH[n_ - 8] = val_;
      else
        CPU.USER[n_] = val_;
      break;
    case ARM_MODE_IRQ:
    case ARM_MODE_ABT:
    case ARM_MODE_UND:
    case ARM_MODE_SVC:
      if(n_ > 12)
        CPU.CASH[n_ - 8] = val_;
      else
        CPU.USER[n_] = val_;
      break;
    }
}

static
uint32_t
readusr(uint32_t const n_)
{
  if(n_ == 15)
    return CPU.USER[15];

  switch(arm_mode_table[CPU.CPSR & 0x1F])
    {
    case ARM_MODE_USER:
      return CPU.USER[n_];
    case ARM_MODE_FIQ:
      if(n_ > 7)
        return CPU.CASH[n_ - 8];
      return CPU.USER[n_];
    case ARM_MODE_IRQ:
    case ARM_MODE_ABT:
    case ARM_MODE_UND:
    case ARM_MODE_SVC:
      if(n_ > 12)
        return CPU.CASH[n_ - 8];
      return CPU.USER[n_];
    }

  return 0;
}

void
opera_io_write(uint32_t const addr_,
               uint32_t const val_)
{
  mwritew(addr_,val_);
}

#ifdef OPERA_ARM_JIT_ENABLED
#include "opera_arm_jit.c"
#else
/* no-op JIT hooks so the opera_mem write inlines and flush call sites do not
 * need per-arch ifdefs.  This is also the OPERA_JIT_BACKENDS=0 shape: the
 * jit TU (with its real hooks) is not compiled, so every external symbol
 * the rest of the core references lands here as a no-op. */
int opera_jit_hook_active = 0;

void
opera_arm_jit_touch(uint32_t const addr_)
{
  (void)addr_;
}

void
opera_arm_jit_flush_all(void)
{
}

int
opera_arm_jit_page_hot(uint32_t const addr_)
{
  (void)addr_;
  return 0;
}

void
opera_arm_jit_dsp_thread_refresh(void)
{
}

void
opera_arm_jit_destroy(void)
{
}
#endif
