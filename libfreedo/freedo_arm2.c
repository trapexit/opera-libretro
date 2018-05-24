
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

uint32_t
freedo_arm2_dispatch(void)
{
  uint32_t opcode;
  uint32_t cond;
  uint32_t op;

  cond = (opcode & 0xF0000000);
  op   = (opcode & 0x0FF00000);
  switch(cp)
    {
      /* SWI */
    case 0x0F000000:
    case 0x0F100000:
    case 0x0F200000:
    case 0x0F300000:
    case 0x0F400000:
    case 0x0F500000:
    case 0x0F600000:
    case 0x0F700000:
    case 0x0F800000:
    case 0x0F900000:
    case 0x0FA00000:
    case 0x0FB00000:
    case 0x0FC00000:
    case 0x0FD00000:
    case 0x0FE00000:
    case 0x0FF00000:
      swi(opcode & 0x00FFFFFF);
      break;
    }
}
