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
    MODE_USR = 1,
    MODE_FIQ = 2,
    MODE_IRQ = 3,
    MODE_SVC = 4,
    MODE_ABT = 5,
    MODE_UND = 6
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
handle_IMM_AND_EOR_SUB_RSB_ADD_ADC_SDC_RSC(const uint32_t opcode_)
{
  return 0;
}

static
INLINE
uint32_t
handle_REG_AND_EOR_SUB_RSB_ADD_ADC_SDC_RSC(const uint32_t opcode_)
{
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
  return handle_IMM_AND_EOR_SUB_RSB_ADD_ADC_SDC_RSC(opcode_);
  return handle_multiply(opcode_);
}

static
INLINE
uint32_t
handle_OP_1(const uint32_t opcode_)
{
  return handle_IMM_TST_TEQ_CMP_CMN_ORR_MOV_BIC_MVN(opcode_);
  return handle_single_data_swap(opcode_);
}

static
INLINE
uint32_t
handle_OP_2(const uint32_t opcode_)
{
  return handle_REG_AND_EOR_SUB_RSB_ADD_ADC_SDC_RSC(opcode_);
}

static
INLINE
uint32_t
handle_OP_3(const uint32_t opcode_)
{
  return handle_REG_TST_TEQ_CMP_CMN_ORR_MOV_BIC_MVN(opcode_);
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
  return handle_REG_PRE_LDR_STR(opcode_);
}

static
INLINE
uint32_t
handle_OP_8(const uint32_t opcode_)
{
  return 0;
}

static
INLINE
uint32_t
handle_OP_9(const uint32_t opcode_)
{
  return 0;
}

static
INLINE
uint32_t
handle_OP_A(const uint32_t opcode_)
{
  return 0;
}

static
INLINE
uint32_t
handle_OP_B(const uint32_t opcode_)
{
  return 0;
}

static
INLINE
uint32_t
handle_OP_C(const uint32_t opcode_)
{
  return 0;
}

static
INLINE
uint32_t
handle_OP_D(const uint32_t opcode_)
{
  return 0;
}

static
INLINE
uint32_t
handle_OP_E(const uint32_t opcode_)
{
  return 0;
}

static
INLINE
uint32_t
handle_OP_F(const uint32_t opcode_)
{
  return 0;
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
