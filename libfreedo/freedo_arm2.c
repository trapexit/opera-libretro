
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

enum
  {
    /* 0000 = EQ - Z set (equal) */
    COND_EQ = 0x0,
    /* 0001 = NE - Z clear (not equal) */
    COND_NE = 0x1,
    /* 0010 = CS - C set (unsigned higher or same) */
    COND_CS = 0x2,
    /* 0011 = CC - C clear (unsigned lower) */
    COND_CC = 0x3,
    /* 0100 = MI - N set (negative) */
    COND_MI = 0x4,
    /* 0101 = PL - N clear (positive or zero) */
    COND_PL = 0x5,
    /* 0110 = VS - V set (overflow) */
    COND_VS = 0x6,
    /* 0111 = VC - V clear (no overflow) */
    COND_VC = 0x7,
    /* 1000 = HI - C set and Z clear (unsigned higher) */
    COND_HI = 0x8,
    /* 1001 = LS - C clear or Z set (unsigned lower or same) */
    COND_LS = 0x9,
    /* 1010 = GE - N set and V set, or N clear and V clear (greater or equal) */
    COND_GE = 0xA,
    /* 1011 = LT - N set and V clear, or N clear and V set (less than) */
    COND_LT = 0xB,
    /* 1100 = GT - Z clear, and either N set and V set, or N clear
       and V clear (greater than) */
    COND_GT = 0xC,
    /* 1101 = LE - Z set, or N set and V clear, or N clear
       and V set (less than or equal) */
    COND_LE = 0xD,
    /* 1110 = AL - always */
    COND_AL = 0xE,
    /* 1111 = NV - never */
    COND_NV = 0xF
  };

uint32_t
freedo_arm2_dispatch(void)
{
  uint32_t opcode;

  switch(opcode)
    {

    }
}
