#include "sign_extend.h"

#include <stdint.h>

#define CONCAT(X, Y) X##Y
#define DISPATCH() \
  if(cycle_cnt > cycles_) goto EXIT;            \
  opcode = *((uint32_t*)&mem8[PC]);             \
  switch((opcode & 0x0F00000) >> 24) { OPCODE(DISPATCH_CASE) }
#define DISPATCH_CASE(OP) case OP: goto CONCAT(label_,OP);
#define OPCODE(X)                               \
  X(OP_0)                                       \
  X(OP_1)                                       \
  X(OP_2)                                       \
  X(OP_3)                                       \
  X(OP_4)                                       \
  X(OP_5)                                       \
  X(OP_6)                                       \
  X(OP_7)                                       \
  X(OP_8)                                       \
  X(OP_9)                                       \
  X(OP_A)                                       \
  X(OP_B)                                       \
  X(OP_C)                                       \
  X(OP_D)                                       \
  X(OP_E)                                       \
  X(OP_F)

#define CPSR_NZ_MASK   0x3FFFFFFFUL
#define CPSR_CV_MASK   0xCFFFFFFFUL
#define CPSR_NZCV_MASK 0x0FFFFFFFUL
#define CPSR_N_FLAG    0x80000000UL
#define CPSR_Z_FLAG    0x40000000UL
#define CPSR_C_FLAG    0x20000000UL
#define CPSR_V_FLAG    0x10000000UL

struct REGS_s
{
  uint32_t R[16];
  uint32_t CPSR;
  uint32_t SPSR;
};

typedef struct REGS_s REGS_t;

struct CPU_s
{
  uint32_t  MODE;
  REGS_t   *REG_BANKS[6];
  REGS_t   *REGS;
};

typedef struct CPU_s CPU_t;

static CPU_t CPU;

/*
  ARM60 supports six modes of operation:
  * User mode (usr): the normal program execution state
  * FIQ mode (fiq): designed to support a data transfer or channel process
  * IRQ mode (irq): used for general purpose interrupt handling
  * Supervisor mode (svc): a protected mode for the operating system
  * Abort mode (abt): entered after a data or instruction prefetch abort
  * Undefined mode (und): entered when an undefined instruction is executed
*/

enum
  {
    MODE_USR = 0,
    MODE_FIQ = 1,
    MODE_IRQ = 2,
    MODE_SVC = 3,
    MODE_ABT = 4,
    MODE_UND = 5
  };

/*
  Exceptions arise whenever there is a need for the normal flow of
  program execution to be broken, so that (for example) the processor
  can be diverted to handle an interrupt from a peripheral. The
  processor state just prior to handling the exception must be
  preserved so that the original program can be resumed when the
  exception routine has completed. Many exceptions may arise at the
  same time.

  ARM60 handles exceptions by making use of the banked registers to
  save state. The old PC and CPSR contents are copied into the
  appropriate R14 and SPSR and the PC and mode bits in the CPSR bits
  are forced to a value which depends on the exception. Interrupt
  disable flags are set where required to prevent otherwise
  unmanageable nestings of exceptions. In the case of a re-entrant
  interrupt handler, R14 and the SPSR should be saved onto a stack in
  main memory before re-enabling the interrupt; when transferring the
  SPSR register to and from a stack, it is important to transfer the
  whole 32 bit value, and not just the flag or control fields. When
  multiple exceptions arise simultaneously, a fixed priority
  determines the order in which they are handled.
*/

enum
  {
    EX_ADDR_RESET                 = 0x00000000UL,
    EX_ADDR_UNDEFINED_INSTRUCTION = 0x00000004UL,
    EX_ADDR_SOFTWARE_INTERRUPT    = 0x00000008UL,
    EX_ADDR_ABORT_PREFETCH        = 0x0000000CUL,
    EX_ADDR_RESERVED              = 0x00000014UL,
    EX_ADDR_IRQ                   = 0x00000018UL,
    EX_ADDR_FIQ                   = 0x0000001CUL
  };

/*
  All ARM60 instructions are conditionally executed, which means that
  their execution may or may not take place depending on the values of
  the N, Z, C and V flags in the CPSR.

  If the always (AL) condition is specified, the instruction will be
  executed irrespective of the flags. The never (NV) class of condition
  codes shall not be used as they will be redefined in future variants
  of the ARM architecture. If a NOP is required it is suggested that MOV
  R0,R0 be used. The assembler treats the absence of a condition code as
  though always had been specified. The other condition codes have
  meanings as detailed in Figure 6: Condition Codes , for instance code
  0000 (EQual) causes the instruction to be executed only if the Z flag
  is set. This would correspond to the case here a compare (CMP)
  instruction had found the two operands to be equal. If the two
  operands were different, the compare instruction would have cleared
  the Z flag and the instruction will not be executed.
*/

/*
  0000 = EQ - Z set (equal)
  0001 = NE - Z clear (not equal)
  0010 = CS - C set (unsigned higher or same)
  0011 = CC - C clear (unsigned lower)
  0100 = MI - N set (negative)
  0101 = PL - N clear (positive or zero)
  0110 = VS - V set (overflow)
  0111 = VC - V clear (no overflow)
  1000 = HI - C set and Z clear (unsigned higher)
  1001 = LS - C clear or Z set (unsigned lower or same)
  1010 = GE - N set and V set, or N clear and V clear (greater or equal)
  1011 = LT - N set and V clear, or N clear and V set (less than)
  1100 = GT - Z clear, and either N set and V set, or N clear and V clear (greater than)
  1101 = LE - Z set, or N set and V clear, or N clear and V set (less than or equal)
  1110 = AL - always
  1111 = NV - never
*/
enum
  {
    COND_EQ = 0x0,
    COND_NE = 0x1,
    COND_CS = 0x2,
    COND_CC = 0x3,
    COND_MI = 0x4,
    COND_PL = 0x5,
    COND_VS = 0x6,
    COND_VC = 0x7,
    COND_HI = 0x8,
    COND_LS = 0x9,
    COND_GE = 0xA,
    COND_LT = 0xB,
    COND_GT = 0xC,
    COND_LE = 0xD,
    COND_AL = 0xE,
    COND_NV = 0xF
  };

enum
  {
    R0  = 0x0,
    R1  = 0x1,
    R2  = 0x2,
    R3  = 0x3,
    R4  = 0x4,
    R5  = 0x5,
    R6  = 0x6,
    R7  = 0x7,
    R8  = 0x8,
    R9  = 0x9,
    R10 = 0xA,
    R11 = 0xB,
    R12 = 0xC,
    R13 = 0xD,
    R14 = 0xE,
    R15 = 0xF
  };

enum
  {
    OP_0 = 0x0,
    OP_1 = 0x1,
    OP_2 = 0x2,
    OP_3 = 0x3,
    OP_4 = 0x4,
    OP_5 = 0x5,
    OP_6 = 0x6,
    OP_7 = 0x7,
    OP_8 = 0x8,
    OP_9 = 0x9,
    OP_A = 0xA,
    OP_B = 0xB,
    OP_C = 0xC,
    OP_D = 0xD,
    OP_E = 0xE,
    OP_F = 0xF
  };

static
INLINE
uint32_t
ROR(const uint32_t n_,
    const uint32_t s_)
{
  return ((n_ >> s_) | (n_ << (32 - s_)));
}

static
INLINE
uint32_t
calculate_CPSR_NZ(const uint32_t val_)
{
  return ((CPU.REGS->CPSR & CPSR_NZ_MASK) |
          ((val_ == 0) ?
           (CPSR_Z_FLAG) :
           (val_ & CPSR_N_FLAG)));
}

static
INLINE
uint32_t
calculate_CPSR_CV_SUB(const uint32_t val_,
                      const uint32_t op1_,
                      const uint32_t op2_)
{
  return ((CPU.REGS->CPSR & CPSR_CV_MASK) |
          ((((op1_ & op2_) | (~val_ & (op1_ | op2_))) & 0x80000000) >> 2) |
          ((((op1_ & op2_ & ~val_) | (~op1_ & ~op2_ & val_)) & 0x80000000) >> 3));
}

static
INLINE
uint32_t
handle_IMM_AND_DAC(const uint32_t Rn_,
                   const uint32_t Rd_,
                   const uint32_t rot_imm_)
{
  CPU.REGS->R[Rd_] = (CPU.REGS->R[Rn_] & rot_imm_);

  return 0;
}

static
INLINE
uint32_t
handle_IMM_AND_SCC(const uint32_t Rn_,
                   const uint32_t Rd_,
                   const uint32_t rot_imm_)
{
  const uint32_t val = (CPU.REGS->R[Rn_] & rot_imm_);

  CPU.REGS->R[Rd_] = val;
  CPU.REGS->CPSR   = calculate_CPSR_NZ(val);

  return 0;
}

static
INLINE
uint32_t
handle_IMM_EOR_DAC(const uint32_t Rn_,
                   const uint32_t Rd_,
                   const uint32_t rot_imm_)
{
  CPU.REGS->R[Rd_] = (CPU.REGS->R[Rn_] ^ rot_imm_);

  return 0;
}

static
INLINE
uint32_t
handle_IMM_EOR_SCC(const uint32_t Rn_,
                   const uint32_t Rd_,
                   const uint32_t rot_imm_)
{
  const uint32_t val = (CPU.REGS->R[Rn_] ^ rot_imm_);

  CPU.REGS->R[Rd_] = val;
  CPU.REGS->CPSR   = calculate_CPSR_NZ(val);

  return 0;
}

static
INLINE
uint32_t
handle_IMM_SUB_DAC(const uint32_t Rn_,
                   const uint32_t Rd_,
                   const uint32_t rot_imm_)
{
  CPU.REGS->R[Rd_] = (CPU.REGS->R[Rn_] - rot_imm_);

  return 0;
}

static
INLINE
uint32_t
handle_IMM_SUB_SCC(const uint32_t Rn_,
                   const uint32_t Rd_,
                   const uint32_t rot_imm_)
{
  const uint32_t val = (CPU.REGS->R[Rn_] - rot_imm_);

  CPU.REGS->R[Rd_] = val;
  CPU.REGS->CPSR   = calculate_CPSR_NZ(val);
  CPU.REGS->CPSR   = calculate_CPSR_CV_SUB(val,CPU.REGS->R[Rn_],rot_imm_);
  
  return 0;
}

static
INLINE
uint32_t
handle_IMM_RSB_DAC(const uint32_t Rn_,
                   const uint32_t Rd_,
                   const uint32_t rot_imm_)
{
  CPU.REGS->R[Rd_] = (rot_imm_ - CPU.REGS->R[Rn_]);

  return 0;
}

static
INLINE
uint32_t
handle_IMM_RSB_SCC(const uint32_t Rn_,
                   const uint32_t Rd_,
                   const uint32_t rot_imm_)
{
  CPU.REGS->R[Rd_] = (rot_imm_ - CPU.REGS->R[Rn_]);

  return 0;
}

static
INLINE
uint32_t
handle_IMM_ADD_DAC(const uint32_t Rn_,
                   const uint32_t Rd_,
                   const uint32_t rot_imm_)
{
  CPU.REGS->R[Rd_] = (CPU.REGS->R[Rn_] + rot_imm_);

  return 0;
}

static
INLINE
uint32_t
handle_IMM_ADD_SCC(const uint32_t Rn_,
                   const uint32_t Rd_,
                   const uint32_t rot_imm_)
{
  CPU.REGS->R[Rd_] = (CPU.REGS->R[Rn_] + rot_imm_);

  return 0;
}

static
INLINE
uint32_t
handle_IMM_ADC_DAC(const uint32_t Rn_,
                   const uint32_t Rd_,
                   const uint32_t rot_imm_)
{
  CPU.REGS->R[Rd_] = (CPU.REGS->R[Rn_] +
                      rot_imm_ +
                      ((CPU.REGS->CPSR >> 29) & 1));

  return 0;
}

static
INLINE
uint32_t
handle_IMM_ADC_SCC(const uint32_t Rn_,
                   const uint32_t Rd_,
                   const uint32_t rot_imm_)
{
  CPU.REGS->R[Rd_] = (CPU.REGS->R[Rn_] +
                      rot_imm_ +
                      ((CPU.REGS->CPSR >> 29) & 1));

  return 0;
}

static
INLINE
uint32_t
handle_IMM_SBC_DAC(const uint32_t Rn_,
                   const uint32_t Rd_,
                   const uint32_t rot_imm_)
{
  CPU.REGS->R[Rd_] = (CPU.REGS->R[Rn_] -
                      rot_imm_ +
                      ((CPU.REGS->CPSR >> 29) & 1) -
                      1);

  return 0;
}

static
INLINE
uint32_t
handle_IMM_SBC_SCC(const uint32_t Rn_,
                   const uint32_t Rd_,
                   const uint32_t rot_imm_)
{
  CPU.REGS->R[Rd_] = (CPU.REGS->R[Rn_] -
                      rot_imm_ +
                      ((CPU.REGS->CPSR >> 29) & 1) -
                      1);
  return 0;
}

static
INLINE
uint32_t
handle_IMM_RSC_DAC(const uint32_t Rn_,
                   const uint32_t Rd_,
                   const uint32_t rot_imm_)
{
  CPU.REGS->R[Rd_] = (rot_imm_ -
                      CPU.REGS->R[Rn_] +
                      ((CPU.REGS->CPSR >> 29) & 1) -
                      1);

  return 0;
}

static
INLINE
uint32_t
handle_IMM_RSC_SCC(const uint32_t Rn_,
                   const uint32_t Rd_,
                   const uint32_t rot_imm_)
{
  CPU.REGS->R[Rd_] = (rot_imm_ -
                      CPU.REGS->R[Rn_] +
                      ((CPU.REGS->CPSR >> 29) & 1) -
                      1);
  return 0;
}

static
INLINE
uint32_t
handle_REG_AND_DAC(const uint32_t Rn_,
                   const uint32_t Rd_,
                   const uint32_t rot_imm_)
{
  CPU.REGS->R[Rd_] = (CPU.REGS->R[Rn_] & rot_imm_);

  return 0;
}

static
INLINE
uint32_t
handle_REG_AND_SCC(const uint32_t Rn_,
                   const uint32_t Rd_,
                   const uint32_t rot_imm_)
{
  CPU.REGS->R[Rd_] = (CPU.REGS->R[Rn_] & rot_imm_);

  return 0;
}

static
INLINE
uint32_t
handle_REG_EOR_DAC(const uint32_t Rn_,
                   const uint32_t Rd_,
                   const uint32_t rot_imm_)
{
  CPU.REGS->R[Rd_] = (CPU.REGS->R[Rn_] ^ rot_imm_);

  return 0;
}

static
INLINE
uint32_t
handle_REG_EOR_SCC(const uint32_t Rn_,
                   const uint32_t Rd_,
                   const uint32_t rot_imm_)
{
  CPU.REGS->R[Rd_] = (CPU.REGS->R[Rn_] ^ rot_imm_);

  return 0;
}

static
INLINE
uint32_t
handle_REG_SUB_DAC(const uint32_t Rn_,
                   const uint32_t Rd_,
                   const uint32_t rot_imm_)
{
  CPU.REGS->R[Rd_] = (CPU.REGS->R[Rn_] - rot_imm_);

  return 0;
}

static
INLINE
uint32_t
handle_REG_SUB_SCC(const uint32_t Rn_,
                   const uint32_t Rd_,
                   const uint32_t rot_imm_)
{
  CPU.REGS->R[Rd_] = (CPU.REGS->R[Rn_] - rot_imm_);

  return 0;
}

static
INLINE
uint32_t
handle_REG_RSB_DAC(const uint32_t Rn_,
                   const uint32_t Rd_,
                   const uint32_t rot_imm_)
{
  CPU.REGS->R[Rd_] = (rot_imm_ - CPU.REGS->R[Rn_]);

  return 0;
}

static
INLINE
uint32_t
handle_REG_RSB_SCC(const uint32_t Rn_,
                   const uint32_t Rd_,
                   const uint32_t rot_imm_)
{
  CPU.REGS->R[Rd_] = (rot_imm_ - CPU.REGS->R[Rn_]);

  return 0;
}

static
INLINE
uint32_t
handle_REG_ADD_DAC(const uint32_t Rn_,
                   const uint32_t Rd_,
                   const uint32_t rot_imm_)
{
  CPU.REGS->R[Rd_] = (CPU.REGS->R[Rn_] + rot_imm_);

  return 0;
}

static
INLINE
uint32_t
handle_REG_ADD_SCC(const uint32_t Rn_,
                   const uint32_t Rd_,
                   const uint32_t rot_imm_)
{
  CPU.REGS->R[Rd_] = (CPU.REGS->R[Rn_] + rot_imm_);

  return 0;
}

static
INLINE
uint32_t
handle_REG_ADC_DAC(const uint32_t Rn_,
                   const uint32_t Rd_,
                   const uint32_t rot_imm_)
{
  CPU.REGS->R[Rd_] = (CPU.REGS->R[Rn_] +
                      rot_imm_ +
                      ((CPU.REGS->CPSR >> 29) & 1));

  return 0;
}

static
INLINE
uint32_t
handle_REG_ADC_SCC(const uint32_t Rn_,
                   const uint32_t Rd_,
                   const uint32_t rot_imm_)
{
  CPU.REGS->R[Rd_] = (CPU.REGS->R[Rn_] +
                      rot_imm_ +
                      ((CPU.REGS->CPSR >> 29) & 1));

  return 0;
}

static
INLINE
uint32_t
handle_REG_SBC_DAC(const uint32_t Rn_,
                   const uint32_t Rd_,
                   const uint32_t rot_imm_)
{
  CPU.REGS->R[Rd_] = (CPU.REGS->R[Rn_] -
                      rot_imm_ +
                      ((CPU.REGS->CPSR >> 29) & 1) -
                      1);

  return 0;
}

static
INLINE
uint32_t
handle_REG_SBC_SCC(const uint32_t Rn_,
                   const uint32_t Rd_,
                   const uint32_t rot_imm_)
{
  CPU.REGS->R[Rd_] = (CPU.REGS->R[Rn_] -
                      rot_imm_ +
                      ((CPU.REGS->CPSR >> 29) & 1) -
                      1);
  return 0;
}

static
INLINE
uint32_t
handle_REG_RSC_DAC(const uint32_t Rn_,
                   const uint32_t Rd_,
                   const uint32_t rot_imm_)
{
  CPU.REGS->R[Rd_] = (rot_imm_ -
                      CPU.REGS->R[Rn_] +
                      ((CPU.REGS->CPSR >> 29) & 1) -
                      1);

  return 0;
}

static
INLINE
uint32_t
handle_REG_RSC_SCC(const uint32_t Rn_,
                   const uint32_t Rd_,
                   const uint32_t rot_imm_)
{
  CPU.REGS->R[Rd_] = (rot_imm_ -
                      CPU.REGS->R[Rn_] +
                      ((CPU.REGS->CPSR >> 29) & 1) -
                      1);
  return 0;
}

static
INLINE
uint32_t
handle_IMM_AND_EOR_SUB_RSB_ADD_ADC_SDC_RSC(const uint32_t opcode_)
{
  uint32_t rot_imm;
  const uint32_t opcode = ((opcode_ & 0x00F00000) >> 20);
  const uint32_t Rn     = ((opcode_ & 0x000F0000) >> 16);
  const uint32_t Rd     = ((opcode_ & 0x0000F000) >> 12);
  const uint32_t rotate = ((opcode_ & 0x00000F00) >>  7);
  const uint32_t imm    = ((opcode_ & 0x000000FF) >>  0);

  rot_imm = ROR(imm,rotate);
  switch(opcode)
    {
      /* AND, do not alter condition codes */
    case 0x0:
      return handle_IMM_AND_DAC(Rn,Rd,rot_imm);
      /* AND, set condition codes */
    case 0x1:
      return handle_IMM_AND_SCC(Rn,Rd,rot_imm);
      /* EOR, do not alter condition codes */
    case 0x2:
      return handle_IMM_EOR_DAC(Rn,Rd,rot_imm);
      /* EOR, set condition codes */
    case 0x3:
      return handle_IMM_EOR_SCC(Rn,Rd,rot_imm);
      /* SUB, do not alter condition codes */
    case 0x4:
      return handle_IMM_SUB_DAC(Rn,Rd,rot_imm);
      /* SUB, set condition codes */
    case 0x5:
      return handle_IMM_SUB_SCC(Rn,Rd,rot_imm);
      /* RSB, do not alter condition codes */
    case 0x6:
      return handle_IMM_RSB_DAC(Rn,Rd,rot_imm);
      /* RSB, set condition codes */
    case 0x7:
      return handle_IMM_RSB_SCC(Rn,Rd,rot_imm);
      /* ADD, do not alter condition codes */
    case 0x8:
      return handle_IMM_ADD_DAC(Rn,Rd,rot_imm);
      /* ADD, set condition codes */
    case 0x9:
      return handle_IMM_ADD_SCC(Rn,Rd,rot_imm);
      /* ADC, do not alter condition codes */
    case 0xA:
      return handle_IMM_ADC_DAC(Rn,Rd,rot_imm);
      /* ADC, set condition codes */
    case 0xB:
      return handle_IMM_ADC_SCC(Rn,Rd,rot_imm);
      /* SBC, do not alter condition codes */
    case 0xC:
      return handle_IMM_SBC_DAC(Rn,Rd,rot_imm);
      /* SBC, set condition codes */
    case 0xD:
      return handle_IMM_SBC_SCC(Rn,Rd,rot_imm);
      /* RSC, do not alter condition codes */
    case 0xE:
      return handle_IMM_RSC_DAC(Rn,Rd,rot_imm);
      /* RSC, set condition codes */
    case 0xF:
      return handle_IMM_RSC_SCC(Rn,Rd,rot_imm);
      break;
    }

  return 0;
}

/*
  See section 4.4.2 of ARM60 data sheet
  s_   = 8bit shift field from opcode
  val_ = Rm

  bit 0: 0 = imm, 1 = Rs
  bit 1,2: 00 = logical left
           01 = logical right
           10 = arithmetic right
           11 = rotate right
  bit 5-7: 5bit unsigned integer
  or
  bit 4-7: Rs
*/

static
INLINE
uint32_t
SHIFT_DAC(const uint32_t s_,
          const uint32_t val_)
{
  switch(s_ & 0x7)
    {
      /* IMM, logical left */
    case 0x0:
      return (val_ << (s_ >> 3));
      /* REG, logical left */
    case 0x1:
      return (val_ << (CPU.REGS->R[s_ >> 4] & 0xFF));
      /* IMM, logical right */
    case 0x2:
      return (val_ >> (s_ >> 3));
      /* REG, logical right */
    case 0x3:
      return (val_ >> (CPU.REGS->R[s_ >> 4] & 0xFF));
      /* IMM, arithmetic right */
      /* WARNING: signed right shifts are often but not always
         arithmetic. It might be required to use a more thorough
         computation. */
    case 0x4:
      {
        const uint32_t s = (s_ >> 3);

        return ((int32_t)val_ >> ((s == 0) ? 32 : s));
      }
      /* REG, arithmetic right */
    case 0x5:
      {
        const uint32_t s = (CPU.REGS->R[s_ >> 4] & 0xFF);

        return ((int32_t)val_ >> ((s == 0) ? 32 : s));
      }
      /* IMM, rotate right */
    case 0x6:
      return ROR(val_,(s_ >> 3));
      /* REG, rotate right */
    case 0x7:
      return ROR(val_,(CPU.REGS->R[s_ >> 4] & 0xFF));
    }
}

static
INLINE
uint32_t
SHIFT_SCC(const uint32_t s_,
          const uint32_t val_)
{
  switch(s_ & 0x7)
    {
      /* IMM, logical left */
    case 0x0:
      return (val_ << (s_ >> 3));
      /* REG, logical left */
    case 0x1:
      return (val_ << (CPU.REGS->R[s_ >> 4] & 0xFF));
      /* IMM, logical right */
    case 0x2:
      return (val_ >> (s_ >> 3));
      /* REG, logical right */
    case 0x3:
      return (val_ >> (CPU.REGS->R[s_ >> 4] & 0xFF));
      /* IMM, arithmetic right */
      /* WARNING: signed right shifts are often but not always
         arithmetic. It might be required to use a more thorough
         computation. */
    case 0x4:
      {
        const uint32_t s = (s_ >> 3);

        return ((int32_t)val_ >> ((s == 0) ? 32 : s));
      }
      /* REG, arithmetic right */
    case 0x5:
      {
        const uint32_t s = (CPU.REGS->R[s_ >> 4] & 0xFF);

        return ((int32_t)val_ >> ((s == 0) ? 32 : s));
      }
      /* IMM, rotate right */
    case 0x6:
      return ROR(val_,(s_ >> 3));
      /* REG, rotate right */
    case 0x7:
      return ROR(val_,(CPU.REGS->R[s_ >> 4] & 0xFF));
    }
}

static
INLINE
uint32_t
handle_REG_AND_EOR_SUB_RSB_ADD_ADC_SDC_RSC(const uint32_t opcode_)
{
  const uint32_t opcode = ((opcode_ & 0x00F00000) >> 20);
  const uint32_t Rn     = ((opcode_ & 0x000F0000) >> 16);
  const uint32_t Rd     = ((opcode_ & 0x0000F000) >> 12);
  const uint32_t shift  = ((opcode_ & 0x00000FF0) >>  4);
  const uint32_t Rm     = ((opcode_ & 0x0000000F) >>  0);

  switch(opcode)
    {
      /* AND, do not alter condition codes */
    case 0x0:
      /* AND, set condition codes */
    case 0x1:
      /* EOR, do not alter condition codes */
    case 0x2:
      /* EOR, set condition codes */
    case 0x3:
      /* SUB, do not alter condition codes */
    case 0x4:
      /* SUB, set condition codes */
    case 0x5:
      /* RSB, do not alter condition codes */
    case 0x6:
      /* RSB, set condition codes */
    case 0x7:
      /* ADD, do not alter condition codes */
    case 0x8:
      /* ADD, set condition codes */
    case 0x9:
      /* ADC, do not alter condition codes */
    case 0xA:
      /* ADC, set condition codes */
    case 0xB:
      /* SBC, do not alter condition codes */
    case 0xC:
      /* SBC, set condition codes */
    case 0xD:
      /* RSC, do not alter condition codes */
    case 0xE:
      /* RSC, set condition codes */
    case 0xF:
      break;
    }

  return 0;
}

static
INLINE
uint32_t
handle_IMM_TST_TEQ_CMP_CMN_ORR_MOV_BIC_MVN(const uint32_t opcode_)
{
  return 0;
}

static
INLINE
uint32_t
handle_REG_TST_TEQ_CMP_CMN_ORR_MOV_BIC_MVN(const uint32_t opcode_)
{
  return 0;
}

static
INLINE
uint32_t
handle_IMM_POST_LDR_STR(const uint32_t opcode_)
{
  return 0;
}

static
INLINE
uint32_t
handle_IMM_PRE_LDR_STR(const uint32_t opcode_)
{
  return 0;
}

static
INLINE
uint32_t
handle_REG_POST_LDR_STR(const uint32_t opcode_)
{
  return 0;
}

static
INLINE
uint32_t
handle_REG_PRE_LDR_STR(const uint32_t opcode_)
{
  return 0;
}

static
INLINE
uint32_t
handle_POST_LDM_STM(const uint32_t opcode_)
{
  return 0;
}

static
INLINE
uint32_t
handle_PRE_LDM_STM(const uint32_t opcode_)
{
  return 0;
}

static
INLINE
uint32_t
handle_Branch(const uint32_t opcode_)
{
  const uint32_t offset = (opcode_ & 0x00FFFFFF);

  CPU.REGS->R[R15] += sign_extend_26_32(offset << 2);

  return (0); // 2S + 1N
}

static
INLINE
uint32_t
handle_Branch_with_Link(const uint32_t opcode_)
{
  const uint32_t offset = (opcode_ & 0x00FFFFFF);

  CPU.REGS->R[R14] = (CPU.REGS->R[R15] + 4);
  CPU.REGS->R[R15] += sign_extend_26_32(offset << 2);

  return (0); // 2S + 1N
}

static
INLINE
uint32_t
handle_POST_LDC_STC(const uint32_t opcode_)
{
  return 0;
}

static
INLINE
uint32_t
handle_PRE_LDC_STC(const uint32_t opcode_)
{
  return 0;
}

static
INLINE
uint32_t
handle_coproc_data_operation(const uint32_t opcode_)
{
  return 0;
}

static
INLINE
uint32_t
handle_coproc_register_transfer(const uint32_t opcode_)
{
  return 0;
}

static
INLINE
uint32_t
handle_software_interrupt(const uint32_t opcode_)
{
  return 0;
}

static
INLINE
uint32_t
handle_single_data_swap(const uint32_t opcode_)
{
  return 0;
}

static
INLINE
uint32_t
handle_multiply(const uint32_t opcode_)
{
  return 0;
}

static
INLINE
uint32_t
handle_undefined(const uint32_t opcode_)
{
  return 0;
}

static
INLINE
uint32_t
handle_OP_0(const uint32_t opcode_)
{
  if((opcode_ & 0x00000090) == 0x00000090)
    return handle_multiply(opcode_);
  return handle_REG_AND_EOR_SUB_RSB_ADD_ADC_SDC_RSC(opcode_);
}

static
INLINE
uint32_t
handle_OP_1(const uint32_t opcode_)
{
  if((opcode_ & 0x01000090) == 0x01000090)
    return handle_single_data_swap(opcode_);
  return handle_IMM_TST_TEQ_CMP_CMN_ORR_MOV_BIC_MVN(opcode_);
}

static
INLINE
uint32_t
handle_OP_2(const uint32_t opcode_)
{
  return handle_IMM_AND_EOR_SUB_RSB_ADD_ADC_SDC_RSC(opcode_);
}

static
INLINE
uint32_t
handle_OP_3(const uint32_t opcode_)
{
  return handle_IMM_TST_TEQ_CMP_CMN_ORR_MOV_BIC_MVN(opcode_);
}

static
INLINE
uint32_t
handle_OP_4(const uint32_t opcode_)
{
  return handle_IMM_POST_LDR_STR(opcode_);
  return 0;
}

static
INLINE
uint32_t
handle_OP_5(const uint32_t opcode_)
{
  return handle_IMM_PRE_LDR_STR(opcode_);
  return 0;
}

static
INLINE
uint32_t
handle_OP_6(const uint32_t opcode_)
{
  if((opcode_ & 0x06000010) == 0x06000010)
    return handle_undefined(opcode_);
  return handle_REG_POST_LDR_STR(opcode_);
}

static
INLINE
uint32_t
handle_OP_7(const uint32_t opcode_)
{
  if((opcode_ & 0x06000010) == 0x06000010)
    return handle_undefined(opcode_);
  return handle_REG_PRE_LDR_STR(opcode_);
}

static
INLINE
uint32_t
handle_OP_8(const uint32_t opcode_)
{
  return handle_POST_LDM_STM(opcode_);
}

static
INLINE
uint32_t
handle_OP_9(const uint32_t opcode_)
{
  return handle_PRE_LDM_STM(opcode_);
}

static
INLINE
uint32_t
handle_OP_A(const uint32_t opcode_)
{
  return handle_Branch(opcode_);
}

static
INLINE
uint32_t
handle_OP_B(const uint32_t opcode_)
{
  return handle_Branch_with_Link(opcode_);
}

static
INLINE
uint32_t
handle_OP_C(const uint32_t opcode_)
{
  return handle_POST_LDC_STC(opcode_);
}

static
INLINE
uint32_t
handle_OP_D(const uint32_t opcode_)
{
  return handle_PRE_LDC_STC(opcode_);
}

static
INLINE
uint32_t
handle_OP_E(const uint32_t opcode_)
{
  if((opcode_ & 0x0E000000) == 0x0E000000)
    return handle_coproc_data_operation(opcode_);
  return handle_coproc_register_transfer(opcode_);
}

static
INLINE
uint32_t
handle_OP_F(const uint32_t opcode_)
{
  return handle_software_interrupt(opcode_);
}

uint32_t
freedo_arm2_loop(const uint32_t cycles_)
{
  uint32_t opcode;
  uint32_t PC;
  uint32_t cycle_cnt;
  uint8_t *mem8;

  cycle_cnt = 0;
  DISPATCH();

 label_OP_0:
  handle_OP_0(opcode);
  DISPATCH();
 label_OP_1:
  handle_OP_1(opcode);
  DISPATCH();
 label_OP_2:
  handle_OP_2(opcode);
  DISPATCH();
 label_OP_3:
  handle_OP_3(opcode);
  DISPATCH();
 label_OP_4:
  handle_OP_4(opcode);
  DISPATCH();
 label_OP_5:
  handle_OP_5(opcode);
  DISPATCH();
 label_OP_6:
  handle_OP_6(opcode);
  DISPATCH();
 label_OP_7:
  handle_OP_7(opcode);
  DISPATCH();
 label_OP_8:
  handle_OP_8(opcode);
  DISPATCH();
 label_OP_9:
  handle_OP_9(opcode);
  DISPATCH();
 label_OP_A:
  handle_OP_A(opcode);
  DISPATCH();
 label_OP_B:
  handle_OP_B(opcode);
  DISPATCH();
 label_OP_C:
  handle_OP_C(opcode);
  DISPATCH();
 label_OP_D:
  handle_OP_D(opcode);
  DISPATCH();
 label_OP_E:
  handle_OP_E(opcode);
  DISPATCH();
 label_OP_F:
  handle_OP_F(opcode);
  DISPATCH();

 EXIT:

  return cycle_cnt;
}
