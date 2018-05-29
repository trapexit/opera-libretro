#include "sign_extend.h"

#include <stdint.h>

#define CONCAT(X, Y) X##Y
#define DISPATCH() \
  if(cycle_cnt > cycles_) goto EXIT;            \
  opcode = *((uint32_t*)&mem8[PC]);             \
  switch((opcode & 0x0FF00000) >> 20) { OPCODE(DISPATCH_CASE) }
#define DISPATCH_CASE(OP) case OP: goto CONCAT(label_,OP);
#define OPCODE(X)                               \
  X(OP_B_0)                                     \
  X(OP_B_1)                                     \
  X(OP_B_2)                                     \
  X(OP_B_3)                                     \
  X(OP_B_4)                                     \
  X(OP_B_5)                                     \
  X(OP_B_6)                                     \
  X(OP_B_7)                                     \
  X(OP_B_8)                                     \
  X(OP_B_9)                                     \
  X(OP_B_A)                                     \
  X(OP_B_B)                                     \
  X(OP_B_C)                                     \
  X(OP_B_D)                                     \
  X(OP_B_E)                                     \
  X(OP_B_F)                                     \
  X(OP_BL_0)                                    \
  X(OP_BL_1)                                    \
  X(OP_BL_2)                                    \
  X(OP_BL_3)                                    \
  X(OP_BL_4)                                    \
  X(OP_BL_5)                                    \
  X(OP_BL_6)                                    \
  X(OP_BL_7)                                    \
  X(OP_BL_8)                                    \
  X(OP_BL_9)                                    \
  X(OP_BL_A)                                    \
  X(OP_BL_B)                                    \
  X(OP_BL_C)                                    \
  X(OP_BL_D)                                    \
  X(OP_BL_E)                                    \
  X(OP_BL_F)                                    \
  X(OP_AND_REG_DAC)                             \
  X(OP_AND_REG_SCC)                             \
  X(OP_AND_IMM_DAC)                             \
  X(OP_AND_IMM_SCC)                             \
  X(OP_EOR_REG_DAC)                             \
  X(OP_EOR_REG_SCC)                             \
  X(OP_EOR_IMM_DAC)                             \
  X(OP_EOR_IMM_SCC)                             \
  X(OP_SUB_REG_DAC)                             \
  X(OP_SUB_REG_SCC)                             \
  X(OP_SUB_IMM_DAC)                             \
  X(OP_SUB_IMM_SCC)                             \
  X(OP_RSB_REG_DAC)                             \
  X(OP_RSB_REG_SCC)                             \
  X(OP_RSB_IMM_DAC)                             \
  X(OP_RSB_IMM_SCC)                             \
  X(OP_ADD_REG_DAC)                             \
  X(OP_ADD_REG_SCC)                             \
  X(OP_ADD_IMM_DAC)                             \
  X(OP_ADD_IMM_SCC)                             \
  X(OP_ADC_REG_DAC)                             \
  X(OP_ADC_REG_SCC)                             \
  X(OP_ADC_IMM_DAC)                             \
  X(OP_ADC_IMM_SCC)                             \
  X(OP_SBC_REG_DAC)                             \
  X(OP_SBC_REG_SCC)                             \
  X(OP_SBC_IMM_DAC)                             \
  X(OP_SBC_IMM_SCC)                             \
  X(OP_RSC_REG_DAC)                             \
  X(OP_RSC_REG_SCC)                             \
  X(OP_RSC_IMM_DAC)                             \
  X(OP_RSC_IMM_SCC)                             \
  X(OP_TST_REG_SCC)                             \
  X(OP_TST_IMM_SCC)                             \
  X(OP_TEQ_REG_SCC)                             \
  X(OP_TEQ_IMM_SCC)                             \
  X(OP_CMP_REG_SCC)                             \
  X(OP_CMP_IMM_SCC)                             \
  X(OP_CMN_REG_SCC)                             \
  X(OP_CMN_IMM_SCC)                             \
  X(OP_ORR_REG_DAC)                             \
  X(OP_ORR_REG_SCC)                             \
  X(OP_ORR_IMM_DAC)                             \
  X(OP_ORR_IMM_SCC)                             \
  X(OP_MOV_REG_DAC)                             \
  X(OP_MOV_REG_SCC)                             \
  X(OP_MOV_IMM_DAC)                             \
  X(OP_MOV_IMM_SCC)                             \
  X(OP_BIC_REG_DAC)                             \
  X(OP_BIC_REG_SCC)                             \
  X(OP_BIC_IMM_DAC)                             \
  X(OP_BIC_IMM_SCC)                             \
  X(OP_MVN_REG_DAC)                             \
  X(OP_MVN_REG_SCC)                             \
  X(OP_MVN_IMM_DAC)                             \
  X(OP_MVN_IMM_SCC)                             \
  X(OP_MRS_CPSR)                                \
  X(OP_MRS_SPSR)                                \
  X(OP_MSR_IMM_CPSR)                            \
  X(OP_MSR_IMM_SPSR)                            \
  X(OP_MSR_REG_CPSR)                            \
  X(OP_MSR_REG_SPSR)                            \
  X(OP_SWI_0)                                   \
  X(OP_SWI_1)                                   \
  X(OP_SWI_2)                                   \
  X(OP_SWI_3)                                   \
  X(OP_SWI_4)                                   \
  X(OP_SWI_5)                                   \
  X(OP_SWI_6)                                   \
  X(OP_SWI_7)                                   \
  X(OP_SWI_8)                                   \
  X(OP_SWI_9)                                   \
  X(OP_SWI_A)                                   \
  X(OP_SWI_B)                                   \
  X(OP_SWI_C)                                   \
  X(OP_SWI_D)                                   \
  X(OP_SWI_E)                                   \
  X(OP_SWI_F)                                   \
  X(OP_CDP_0)                                   \
  X(OP_CDP_1)                                   \
  X(OP_CDP_2)                                   \
  X(OP_CDP_3)                                   \
  X(OP_CDP_4)                                   \
  X(OP_CDP_5)                                   \
  X(OP_CDP_6)                                   \
  X(OP_CDP_7)                                   \
  X(OP_CDP_8)                                   \
  X(OP_CDP_9)                                   \
  X(OP_CDP_A)                                   \
  X(OP_CDP_B)                                   \
  X(OP_CDP_C)                                   \
  X(OP_CDP_D)                                   \
  X(OP_CDP_E)                                   \
  X(OP_CDP_F)

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
    OP_B_0 = 0xA0,
    OP_B_1 = 0xA1,
    OP_B_2 = 0xA2,
    OP_B_3 = 0xA3,
    OP_B_4 = 0xA4,
    OP_B_5 = 0xA5,
    OP_B_6 = 0xA6,
    OP_B_7 = 0xA7,
    OP_B_8 = 0xA8,
    OP_B_9 = 0xA9,
    OP_B_A = 0xAA,
    OP_B_B = 0xAB,
    OP_B_C = 0xAC,
    OP_B_D = 0xAD,
    OP_B_E = 0xAE,
    OP_B_F = 0xAF,

    OP_BL_0 = 0xB0,
    OP_BL_1 = 0xB1,
    OP_BL_2 = 0xB2,
    OP_BL_3 = 0xB3,
    OP_BL_4 = 0xB4,
    OP_BL_5 = 0xB5,
    OP_BL_6 = 0xB6,
    OP_BL_7 = 0xB7,
    OP_BL_8 = 0xB8,
    OP_BL_9 = 0xB9,
    OP_BL_A = 0xBA,
    OP_BL_B = 0xBB,
    OP_BL_C = 0xBC,
    OP_BL_D = 0xBD,
    OP_BL_E = 0xBE,
    OP_BL_F = 0xBF,

    OP_AND_REG_DAC = 0x00,
    OP_AND_REG_SCC = 0x01,
    OP_AND_IMM_DAC = 0x20,
    OP_AND_IMM_SCC = 0x21,

    OP_EOR_REG_DAC = 0x02,
    OP_EOR_REG_SCC = 0x03,
    OP_EOR_IMM_DAC = 0x22,
    OP_EOR_IMM_SCC = 0x23,

    OP_SUB_REG_DAC = 0x04,
    OP_SUB_REG_SCC = 0x05,
    OP_SUB_IMM_DAC = 0x24,
    OP_SUB_IMM_SCC = 0x25,

    OP_RSB_REG_DAC = 0x06,
    OP_RSB_REG_SCC = 0x07,
    OP_RSB_IMM_DAC = 0x26,
    OP_RSB_IMM_SCC = 0x27,

    OP_ADD_REG_DAC = 0x08,
    OP_ADD_REG_SCC = 0x09,
    OP_ADD_IMM_DAC = 0x28,
    OP_ADD_IMM_SCC = 0x29,

    OP_ADC_REG_DAC = 0x0A,
    OP_ADC_REG_SCC = 0x0B,
    OP_ADC_IMM_DAC = 0x2A,
    OP_ADC_IMM_SCC = 0x2B,

    OP_SBC_REG_DAC = 0x0C,
    OP_SBC_REG_SCC = 0x0D,
    OP_SBC_IMM_DAC = 0x2C,
    OP_SBC_IMM_SCC = 0x2D,

    OP_RSC_REG_DAC = 0x0E,
    OP_RSC_REG_SCC = 0x0F,
    OP_RSC_IMM_DAC = 0x2E,
    OP_RSC_IMM_SCC = 0x2F,

    //    OP_TST_REG_DAC = 0x10,
    OP_TST_REG_SCC = 0x11,
    //    OP_TST_IMM_DAC = 0x30,
    OP_TST_IMM_SCC = 0x31,

    //    OP_TEQ_REG_DAC = 0x12,
    OP_TEQ_REG_SCC = 0x13,
    //    OP_TEQ_IMM_DAC = 0x32,
    OP_TEQ_IMM_SCC = 0x33,

    //    OP_CMP_REG_DAC = 0x14,
    OP_CMP_REG_SCC = 0x15,
    //    OP_CMP_IMM_DAC = 0x34,
    OP_CMP_IMM_SCC = 0x35,

    //    OP_CMN_REG_DAC = 0x16,
    OP_CMN_REG_SCC = 0x17,
    //    OP_CMN_IMM_DAC = 0x36,
    OP_CMN_IMM_SCC = 0x37,

    OP_ORR_REG_DAC = 0x18,
    OP_ORR_REG_SCC = 0x19,
    OP_ORR_IMM_DAC = 0x38,
    OP_ORR_IMM_SCC = 0x39,

    OP_MOV_REG_DAC = 0x1A,
    OP_MOV_REG_SCC = 0x1B,
    OP_MOV_IMM_DAC = 0x3A,
    OP_MOV_IMM_SCC = 0x3B,

    OP_BIC_REG_DAC = 0x1C,
    OP_BIC_REG_SCC = 0x1D,
    OP_BIC_IMM_DAC = 0x3C,
    OP_BIC_IMM_SCC = 0x3D,

    OP_MVN_REG_DAC = 0x1E,
    OP_MVN_REG_SCC = 0x1F,
    OP_MVN_IMM_DAC = 0x3E,
    OP_MVN_IMM_SCC = 0x3F,

    OP_MRS_CPSR     = 0x10,
    OP_MRS_SPSR     = 0x14,

    OP_MSR_IMM_CPSR = 0x12,
    OP_MSR_IMM_SPSR = 0x16,
    OP_MSR_REG_CPSR = 0x32,
    OP_MSR_REG_SPSR = 0x36,

    OP_MUL_DAC = 0x00,
    OP_MUL_SCC = 0x01,
    OP_MLA_DAC = 0x02,
    OP_MLA_SCC = 0x03,

    OP_SWI_0 = 0xF0,
    OP_SWI_1 = 0xF1,
    OP_SWI_2 = 0xF2,
    OP_SWI_3 = 0xF3,
    OP_SWI_4 = 0xF4,
    OP_SWI_5 = 0xF5,
    OP_SWI_6 = 0xF6,
    OP_SWI_7 = 0xF7,
    OP_SWI_8 = 0xF8,
    OP_SWI_9 = 0xF9,
    OP_SWI_A = 0xFA,
    OP_SWI_B = 0xFB,
    OP_SWI_C = 0xFC,
    OP_SWI_D = 0xFD,
    OP_SWI_E = 0xFE,
    OP_SWI_F = 0xFF,

    OP_CDP_0 = 0xE0,
    OP_CDP_1 = 0xE1,
    OP_CDP_2 = 0xE2,
    OP_CDP_3 = 0xE3,
    OP_CDP_4 = 0xE4,
    OP_CDP_5 = 0xE5,
    OP_CDP_6 = 0xE6,
    OP_CDP_7 = 0xE7,
    OP_CDP_8 = 0xE8,
    OP_CDP_9 = 0xE9,
    OP_CDP_A = 0xEA,
    OP_CDP_B = 0xEB,
    OP_CDP_C = 0xEC,
    OP_CDP_D = 0xED,
    OP_CDP_E = 0xEE,
    OP_CDP_F = 0xEF,

    OP_LDC_PRE_DOWN_ST_NW  = 0xD1,
    OP_LDC_PRE_DOWN_ST_WB  = 0xD3,
    OP_LDC_PRE_DOWN_LT_NW  = 0xD5,
    OP_LDC_PRE_DOWN_LT_WB  = 0xD7,
    OP_LDC_PRE_UP_ST_NW    = 0xD9,
    OP_LDC_PRE_UP_ST_WB    = 0xDB,
    OP_LDC_PRE_UP_LT_NW    = 0xDD,
    OP_LDC_PRE_UP_LT_WB    = 0xDF,
    OP_LDC_POST_DOWN_ST_NW = 0xC1,
    OP_LDC_POST_DOWN_ST_WB = 0xC3,
    OP_LDC_POST_DOWN_LT_NW = 0xC5,
    OP_LDC_POST_DOWN_LT_WB = 0xC7,
    OP_LDC_POST_UP_ST_NW   = 0xC9,
    OP_LDC_POST_UP_ST_WB   = 0xCB,
    OP_LDC_POST_UP_LT_NW   = 0xCD,
    OP_LDC_POST_UP_LT_WB   = 0xCF,

    OP_STC_PRE_DOWN_ST_NW  = 0xD0,
    OP_STC_PRE_DOWN_ST_WB  = 0xD2,
    OP_STC_PRE_DOWN_LT_NW  = 0xD4,
    OP_STC_PRE_DOWN_LT_WB  = 0xD6,
    OP_STC_PRE_UP_ST_NW    = 0xD8,
    OP_STC_PRE_UP_ST_WB    = 0xDA,
    OP_STC_PRE_UP_LT_NW    = 0xDC,
    OP_STC_PRE_UP_LT_WB    = 0xDE,
    OP_STC_POST_DOWN_ST_NW = 0xC0,
    OP_STC_POST_DOWN_ST_WB = 0xC2,
    OP_STC_POST_DOWN_LT_NW = 0xC4,
    OP_STC_POST_DOWN_LT_WB = 0xC6,
    OP_STC_POST_UP_ST_NW   = 0xC8,
    OP_STC_POST_UP_ST_WB   = 0xCA,
    OP_STC_POST_UP_LT_NW   = 0xCC,
    OP_STC_POST_UP_LT_WB   = 0xCE
  };

uint32_t
freedo_arm2_loop(const uint32_t cycles_)
{
  uint32_t opcode;
  uint32_t PC;
  uint32_t cycle_cnt;
  uint8_t *mem8;

  cycle_cnt = 0;
  DISPATCH();

 label_OP_B_0:
 label_OP_B_1:
 label_OP_B_2:
 label_OP_B_3:
 label_OP_B_4:
 label_OP_B_5:
 label_OP_B_6:
 label_OP_B_7:
 label_OP_B_8:
 label_OP_B_9:
 label_OP_B_A:
 label_OP_B_B:
 label_OP_B_C:
 label_OP_B_D:
 label_OP_B_E:
 label_OP_B_F:
  {
    // R15 += sign_extend_26_32((opcode & 0x00FFFFFF) << 2);
    // cycle_cnt += 2S + 1N
  }
  DISPATCH();

 label_OP_BL_0:
 label_OP_BL_1:
 label_OP_BL_2:
 label_OP_BL_3:
 label_OP_BL_4:
 label_OP_BL_5:
 label_OP_BL_6:
 label_OP_BL_7:
 label_OP_BL_8:
 label_OP_BL_9:
 label_OP_BL_A:
 label_OP_BL_B:
 label_OP_BL_C:
 label_OP_BL_D:
 label_OP_BL_E:
 label_OP_BL_F:
  {
    // R14 = PC + 4
    // R15 += sign_extend_26_32((opcode & 0x00FFFFFF) << 2);
    // cycle_cnt += 2S + 1N
  }
  DISPATCH();

 label_OP_AND_REG_DAC:
  DISPATCH();

 label_OP_AND_REG_SCC:
  DISPATCH();

 label_OP_AND_IMM_DAC:
  DISPATCH();
 label_OP_AND_IMM_SCC:
  DISPATCH();

 label_OP_EOR_REG_DAC:
  DISPATCH();
 label_OP_EOR_REG_SCC:
  DISPATCH();
 label_OP_EOR_IMM_DAC:
  DISPATCH();
 label_OP_EOR_IMM_SCC:
  DISPATCH();

 label_OP_SUB_REG_DAC:
  DISPATCH();
 label_OP_SUB_REG_SCC:
  DISPATCH();
 label_OP_SUB_IMM_DAC:
  DISPATCH();
 label_OP_SUB_IMM_SCC:
  DISPATCH();

 label_OP_RSB_REG_DAC:
  DISPATCH();
 label_OP_RSB_REG_SCC:
  DISPATCH();
 label_OP_RSB_IMM_DAC:
  DISPATCH();
 label_OP_RSB_IMM_SCC:
  DISPATCH();

 label_OP_ADD_REG_DAC:
  DISPATCH();
 label_OP_ADD_REG_SCC:
  DISPATCH();
 label_OP_ADD_IMM_DAC:
  DISPATCH();
 label_OP_ADD_IMM_SCC:
  DISPATCH();

 label_OP_ADC_REG_DAC:
  DISPATCH();
 label_OP_ADC_REG_SCC:
  DISPATCH();
 label_OP_ADC_IMM_DAC:
  DISPATCH();
 label_OP_ADC_IMM_SCC:
  DISPATCH();

 label_OP_SBC_REG_DAC:
  DISPATCH();
 label_OP_SBC_REG_SCC:
  DISPATCH();
 label_OP_SBC_IMM_DAC:
  DISPATCH();
 label_OP_SBC_IMM_SCC:
  DISPATCH();

 label_OP_RSC_REG_DAC:
  DISPATCH();
 label_OP_RSC_REG_SCC:
  DISPATCH();
 label_OP_RSC_IMM_DAC:
  DISPATCH();
 label_OP_RSC_IMM_SCC:
  DISPATCH();

 label_OP_TST_REG_SCC:
  DISPATCH();
 label_OP_TST_IMM_SCC:
  DISPATCH();

 label_OP_TEQ_REG_SCC:
  DISPATCH();
 label_OP_TEQ_IMM_SCC:
  DISPATCH();

 label_OP_CMP_REG_SCC:
  DISPATCH();
 label_OP_CMP_IMM_SCC:
  DISPATCH();

 label_OP_CMN_REG_SCC:
  DISPATCH();
 label_OP_CMN_IMM_SCC:
  DISPATCH();

 label_OP_ORR_REG_DAC:
  DISPATCH();
 label_OP_ORR_REG_SCC:
  DISPATCH();
 label_OP_ORR_IMM_DAC:
  DISPATCH();
 label_OP_ORR_IMM_SCC:
  DISPATCH();

 label_OP_MOV_REG_DAC:
  DISPATCH();
 label_OP_MOV_REG_SCC:
  DISPATCH();
 label_OP_MOV_IMM_DAC:
  DISPATCH();
 label_OP_MOV_IMM_SCC:
  DISPATCH();

 label_OP_BIC_REG_DAC:
  DISPATCH();
 label_OP_BIC_REG_SCC:
  DISPATCH();
 label_OP_BIC_IMM_DAC:
  DISPATCH();
 label_OP_BIC_IMM_SCC:
  DISPATCH();

 label_OP_MVN_REG_DAC:
  DISPATCH();
 label_OP_MVN_REG_SCC:
  DISPATCH();
 label_OP_MVN_IMM_DAC:
  DISPATCH();
 label_OP_MVN_IMM_SCC:
  DISPATCH();

 label_OP_MRS_CPSR:
 label_OP_MRS_SPSR:
 label_OP_MSR_IMM_CPSR:
 label_OP_MSR_IMM_SPSR:
 label_OP_MSR_REG_CPSR:
 label_OP_MSR_REG_SPSR:
  DISPATCH();

 label_OP_MUL_DAC:
 label_OP_MUL_SCC:
 label_OP_MLA_DAC:
 label_OP_MLA_SCC:
  DISPATCH();

 label_OP_SWI_0:
 label_OP_SWI_1:
 label_OP_SWI_2:
 label_OP_SWI_3:
 label_OP_SWI_4:
 label_OP_SWI_5:
 label_OP_SWI_6:
 label_OP_SWI_7:
 label_OP_SWI_8:
 label_OP_SWI_9:
 label_OP_SWI_A:
 label_OP_SWI_B:
 label_OP_SWI_C:
 label_OP_SWI_D:
 label_OP_SWI_E:
 label_OP_SWI_F:
  {
    // MODE = MODE_SVC
    // R14_svc = PC + 4
    PC = EX_ADDR_SOFTWARE_INTERRUPT;
    // cycle_cnt += 2S + 1N
  }
  DISPATCH();

 label_OP_CDP_0:
 label_OP_CDP_1:
 label_OP_CDP_2:
 label_OP_CDP_3:
 label_OP_CDP_4:
 label_OP_CDP_5:
 label_OP_CDP_6:
 label_OP_CDP_7:
 label_OP_CDP_8:
 label_OP_CDP_9:
 label_OP_CDP_A:
 label_OP_CDP_B:
 label_OP_CDP_C:
 label_OP_CDP_D:
 label_OP_CDP_E:
 label_OP_CDP_F:
  {
    const uint32_t CRm = (opcode & 0xF);
    const uint32_t CP  = ((opcode & 0x000E0) >>  4);
    const uint32_t CPN = ((opcode & 0x00F00) >>  8);
    const uint32_t CRd = ((opcode & 0x0F000) >> 12);
    const uint32_t CRn = ((opcode & 0xF0000) >> 16);

    // cycle_cnt += 1S + bI
  }
  DISPATCH();

 EXIT:

  return cycle_cnt;
}
