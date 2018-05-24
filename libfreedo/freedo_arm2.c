#include <stdint.h>

#define CONCAT(X, Y) X##Y
#define DISPATCH() \
  if(cycle_cnt > cycles_) goto EXIT;            \
  opcode = *((uint32_t*)&mem8[PC]);             \
  switch(opcode & 0x0FF00000) { OPCODE(DISPATCH_CASE) }
#define DISPATCH_CASE(OP) case OP: goto CONCAT(label_,OP);
#define OPCODE(X)                               \
	X(OP_SWI0)                              \
	X(OP_SWI1)                              \
	X(OP_SWI2)                              \
	X(OP_SWI3)                              \
	X(OP_SWI4)                              \
	X(OP_SWI5)                              \
	X(OP_SWI6)                              \
	X(OP_SWI7)                              \
	X(OP_SWI8)                              \
	X(OP_SWI9)                              \
	X(OP_SWIA)                              \
	X(OP_SWIB)                              \
	X(OP_SWIC)                              \
	X(OP_SWID)                              \
	X(OP_SWIE)                              \
	X(OP_SWIF)                              \
	X(OP_JMP)

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
    OP_SWI0 = 0x0F000000,
    OP_SWI1 = 0x0F100000,
    OP_SWI2 = 0x0F200000,
    OP_SWI3 = 0x0F300000,
    OP_SWI4 = 0x0F400000,
    OP_SWI5 = 0x0F500000,
    OP_SWI6 = 0x0F600000,
    OP_SWI7 = 0x0F700000,
    OP_SWI8 = 0x0F800000,
    OP_SWI9 = 0x0F900000,
    OP_SWIA = 0x0FA00000,
    OP_SWIB = 0x0FB00000,
    OP_SWIC = 0x0FC00000,
    OP_SWID = 0x0FD00000,
    OP_SWIE = 0x0FE00000,
    OP_SWIF = 0x0FF00000,

    OS_CDP0 = 0x0E000000,
    OS_CDP1 = 0x0E100000,
    OS_CDP2 = 0x0E200000,
    OS_CDP3 = 0x0E300000,
    OS_CDP4 = 0x0E400000,
    OS_CDP5 = 0x0E500000,
    OS_CDP6 = 0x0E600000,
    OS_CDP7 = 0x0E700000,
    OS_CDP8 = 0x0E800000,
    OS_CDP9 = 0x0E900000,
    OS_CDPA = 0x0EA00000,
    OS_CDPB = 0x0EB00000,
    OS_CDPC = 0x0EC00000,
    OS_CDPD = 0x0ED00000,
    OS_CDPE = 0x0EE00000,
    OS_CDPF = 0x0EF00000,

    OP_JMP  = 0x00000000
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

 label_OP_SWI0:
 label_OP_SWI1:
 label_OP_SWI2:
 label_OP_SWI3:
 label_OP_SWI4:
 label_OP_SWI5:
 label_OP_SWI6:
 label_OP_SWI7:
 label_OP_SWI8:
 label_OP_SWI9:
 label_OP_SWIA:
 label_OP_SWIB:
 label_OP_SWIC:
 label_OP_SWID:
 label_OP_SWIE:
 label_OP_SWIF:
  {
    // MODE = MODE_SVC
    // R14_svc = PC + 4
    PC = EX_ADDR_SOFTWARE_INTERRUPT;
    // cycle_cnt += 2S + 1N
  }
  DISPATCH();

 label_OP_CDP0:
 label_OP_CDP1:
 label_OP_CDP2:
 label_OP_CDP3:
 label_OP_CDP4:
 label_OP_CDP5:
 label_OP_CDP6:
 label_OP_CDP7:
 label_OP_CDP8:
 label_OP_CDP9:
 label_OP_CDPA:
 label_OP_CDPB:
 label_OP_CDPC:
 label_OP_CDPD:
 label_OP_CDPE:
 label_OP_CDPF:
  {
    const uint32_t CRm = (opcode & 0xF);
    const uint32_t CP  = ((opcode & 0x000E0) >>  4);
    const uint32_t CPN = ((opcode & 0x00F00) >>  8);
    const uint32_t CRd = ((opcode & 0x0F000) >> 12);
    const uint32_t CRn = ((opcode & 0xF0000) >> 16);

    
  }
  DISPATCH();

 label_OP_JMP:
  DISPATCH();

 EXIT:

  return cycle_cnt;
}
