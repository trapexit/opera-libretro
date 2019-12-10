#include "freedo_bitmath.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <assert.h>

typedef void (*op_print_func_t)(const uint32_t);

typedef struct arm_opcode_s arm_opcode_t;
struct arm_opcode_s
{
  uint32_t        mask;
  uint32_t        pattern;
  op_print_func_t print;
};

enum
  {
    ARM_DP_AND = 0x0,
    ARM_DP_EOR = 0x1,
    ARM_DP_SUB = 0x2,
    ARM_DP_RSB = 0x3,
    ARM_DP_ADD = 0x4,
    ARM_DP_ADC = 0x5,
    ARM_DP_SBC = 0x6,
    ARM_DP_RSC = 0x7,
    ARM_DP_TST = 0x8,
    ARM_DP_TEQ = 0x9,
    ARM_DP_CMP = 0xA,
    ARM_DP_CMN = 0xB,
    ARM_DP_ORR = 0xC,
    ARM_DP_MOV = 0xD,
    ARM_DP_BIC = 0xE,
    ARM_DP_MVN = 0xF
  };

const
char*
freedo_debug_swi_func(uint32_t swi_)
{
  const char *str;

  switch(swi_ & 0x00FFFFFF)
    {
    case 0x00101:
      return "void Debug(void);";
    case 0x10000:
      return "Item CreateSizedItem(int32 ctype, TagArg *p, int32 size);";
    case 0x10001:
      return "int32 WaitSignal(uint32 sigMask);";
    case 0x10002:
      return "Err SendSignal(Item task, uint32 sigMask);";
    case 0x10003:
      return "Err DeleteItem(Item i);";
    case 0x10004:
      return "Item FindItem(int32 ctype, TagArg *tp);";
    case 0x10005:
      return "Item OpenItem(Item foundItem, void *args);";
    /* case 0x10006:	 */
    /*   return "Err UnlockSemaphore(Item s);"; */
    case 0x10006:
      return "Err UnlockItem(Item s);";
    /* case 0x10007:	 */
    /*   return "int32 LockSemaphore(Item s, uint32 flags);"; */
    case 0x10007:
      return "int32 LockItem(Item s, uint32 flags);";
    case 0x10008:
      return "Err CloseItem(Item i);";
    case 0x10009:
      return "void Yield(void);";
    case 0x1000F:
      return "Item GetThisMsg(Item msg);";
    case 0x10010:
      return "Err SendMsg(Item mp, Item msg, const void *dataptr, int32 datasize);";
    /* case 0x10010:	 */
    /*   return "Err SendSmallMsg(Item mp, Item msg, uint32 val1, uint32 val2);"; */
    case 0x1000A:
      return "int32 SetItemPri(Item i, uint8 newpri);";
    case 0x1000D:
      return "void *AllocMemBlocks(int32 size, uint32 typebits);";
    case 0x1000E:
      return "void kprintf(const char *fmt, …);";
    case 0x10011:
      return "uint32 ReadHardwareRandomNumber(void);";
    case 0x10012:
      return "Err ReplyMsg(Item msg, int32 result, const void *dataptr, int32 datasize);";
    /* case 0x10012:	 */
    /*   return "Err ReplySmallMsg(Item msg, int32 result, uint32 val1, int32 val2);"; */
    case 0x10013:
      return "Item GetMsg(Item mp);";
    case 0x10014:
      return "Err ControlMem(void *p, int32 size, int32 cmd, Item task);";
    case 0x10015:
      return "int32 AllocSignal(uint32 sigMask);";
    case 0x10016:
      return "Err FreeSignal(uint32 sigMask);";
    case 0x10018:
      return "Err SendIO(Item ior, const IOInfo *ioiP);";
    case 0x10019:
      return "Err AbortIO(Item ior);";
    case 0x1001A:
      return "int32 SetItemPri(Item i, uint8 newpri);";
    case 0x1001C:
      return "Err SetItemOwner(Item i, Item newOwner);";
    case 0x1001E:
      return "int MayGetChar(void);";
    case 0x10021:
      return "int32 SystemScavengeMem(void);";
    case 0x10024:
      return "Item FindAndOpenItem(int32 ctype, TagArg *tp);";
    case 0x10025:
      return "Err DoIO(Item ior, const IOInfo *ioiP);";
    case 0x10026:
      return "uint32 SampleSystemTime(void);";
    case 0x10027:
      return "Err SetExitStatus(int32 status);";
    case 0x10028:
      return "Item WaitPort(Item mp, Item msg);";
    case 0x10029:
      return "Err WaitIO(Item ior);";

    case 0x20005:
      return "Err EnableVAVG(Item screenItem);";
    case 0x20006:
      return "Err DisableVAVG(Item screenItem);";
    case 0x20007:
      return "Err EnableHAVG(Item screenItem);";
    case 0x20008:
      return "Err DisableHAVG(Item screenItem);";
    case 0x20011:
      return "Err AddScreenGroup(Item screenGroup, TagArg *targs);";
    case 0x20017:
      return "Err DrawScreenCels(Item screenItem, CCB *ccb);";
    case 0x20027:
      return "Err DrawCels(Item bitmapItem, CCB *ccb);";
    case 0x2002D:
      return "Err DisplayScreen(Item screenItem0, Item screenItem1);";

    case 0x30000:
      return "Item OpenDiskFile(char *path);";
    case 0x30001:
      return "int32 CloseDiskFile(Item fileItem);";
    case 0x30004:
      return "Item MountFileSystem(Item deviceItem, int32 unit, uint32 blockOffset);";
    case 0x30005:
      return "Item OpenDiskFileInDir(Item dirItem, char *path);";
    case 0x30006:
      return "Item MountMacFileSystem(char *path);";
    case 0x30007:
      return "Item ChangeDirectory(char *path);";
    case 0x30008:
      return "Item GetDirectory(char *pathBuf, int pathBufLen);";
    case 0x30009:
      return "Item CreateFile(char *path);";
    case 0x3000A:
      return "Err DeleteFile(char *path);";
    case 0x3000B:
      return "Item CreateAlias(char *aliasPath, char *realPath);";
    case 0x3000D:
      return "Err DismountFileSystem(char *name);";

    case 0x40000:
      return "Err TweakKnob(Item KnobItem, int32 Value);";
    case 0x40001:
      return "Err StartInstrument(Item Instrument, TagArg *TagList);";
    case 0x40002:
      return "Err ReleaseInstrument(Item Instrument, TagArg *TagList);";
    case 0x40003:
      return "Err StopInstrument(Item Instrument, TagArg *TagList);";
    case 0x40004:
      return "Err TuneInsTemplate(Item Instrument, Item Tuning);";
    case 0x40005:
      return "Err TunInstrument(Item Instrument, Item Tuning);";
    case 0x40008:
      return "Err ConnectInstruments(Item SrcIns, char *SrcName, Item DstIns, char *DstName);";
    case 0x40009:
      return "uint32 TraceAudio(int32 Mask);";
    case 0x4000A:
      return "int32 AllocAmplitude(int32 Amplitude);";
    case 0x4000B:
      return "Err FreeAmplitude(int32 Amplitude);";
    case 0x4000C:
      return "Err DisconnectInstruments(Item SrcIns, char *SrcName, Item DstIns, char *DstName);";
    case 0x4000D:
      return "Err SignalAtTime(Item Cue, AudioTime Time);";
    case 0x4000F:
      return "Err SetAudioRate(Item Owner, frac16 Rate);";
    case 0x40010:
      return "Err SetAudioDuration(Item Owner, uint32 Frames);";
    case 0x40011:
      return "Err TweakRawKnob(Item KnobItem, int32 Value);";
    case 0x40012:
      return "Err StartAttachment(Item Attachment, TagArg *tp);";
    case 0x40013:
      return "Err ReleaseAttachment(Item Attachment, TagArg *tp);";
    case 0x40014:
      return "Err StopAttachment(Item Attachment, TagArg *tp);";
    case 0x40015:
      return "Err LinkAttachments(Item At1, Item At2);";
    case 0x40016:
      return "Err MonitorAttachment(Item Attachment, Item Cue, int32 CueAt);";
    case 0x40018:
      return "Err AbandonInstrument(Item Instrument);";
    case 0x40019:
      return "Item AdoptInstrument(Item InsTemplate);";
    case 0x4001A:
      return "Item ScavengeInstrument(Item InsTemplate, uint8 Priority, int32 MaxActivity, int32 IfSystemWide);";
    case 0x4001B:
      return "Err SetAudioItemInfo(Item AnyItem, TagArg *tp);";
    case 0x4001C:
      return "Err PauseInstrument(Item Instrument);";
    case 0x4001D:
      return "Err ResumeInstrument(Item Instrument);";
    case 0x4001E:
      return "int32 WhereAttachment(Item Attachment);";
    case 0x40020:
      return "Err BendInstrumentPitch(Item Instrument, frac16 BendFrac);";
    case 0x40021:
      return "Err AbortTimerCue(Item Cue);";
    case 0x40022:
      return "Err EnableAudioInput(int32 OnOrOff, TagArg *Tags);";
    case 0x40024:
      return "Err ReadProbe(Item Probe, int32 *ValuePtr);";
    case 0x40026:
      return "uint16 GetAudioFrameCount(void);";
    case 0x40027:
      return "int32 GetAudioCyclesUsed(void);";

    case 0x50000:
      return "void MulVec3Mat33_F16(vec3f16 dest, vec3f16 vec, mat33f16 mat);";
    case 0x50001:
      return "void MulMat33Mat33_F16(mat33f16 dest, mat33f16 src1, mat33f16 src2);";
    case 0x50002:
      return "void MulManyVec3Mat33_F16(vec3f16 *dest, vec3f16 *src, mat33f16 mat, int32 count);";
    case 0x50003:
      return "void MulObjectVec3Mat33_F16(void *objectlist[], ObjOffset1 *offsetstruct, int32 count);";
    case 0x50004:
      return "void MulObjectMat33_F16(void *objectlist[], ObjOffset2 *offsetstruct, mat33f16 mat, int32 count);";
    case 0x50005:
      return "void MulManyF16(frac16 *dest, frac16 *src1, frac16 *src2, int32 count);";
    case 0x50006:
      return "void MulScalerF16(frac16 *dest, frac16 *src, frac16 scaler, int32 count);";
    case 0x50007:
      return "void MulVec4Mat44_F16(vec4f16 dest, vec4f16 vec, mat44f16 mat);";
    case 0x50008:
      return "void MulMat44Mat44_F16(mat44f16 dest, mat44f16 src1, mat44f16 src2);";
    case 0x50009:
      return "void MulManyVec4Mat44_F16(vec4f16 *dest, vec4f16 *src, mat44f16 mat, int32 count);";
    case 0x5000A:
      return "void MulObjectVec4Mat44_F16(void *objectlist[], ObjeOffset1 *offsetstruct, int32 count);";
    case 0x5000B:
      return "void MulObjectMat44_F16(void *objectlist[], ObjOffset2 *offsetstruct, mat44f16 mat, int32 count);";
    case 0x5000C:
      return "frac16 Dot3_F16(vec3f16 v1, vec3f16 v2);";
    case 0x5000D:
      return "frac16 Dot4_F16(vec4f16 v1, vec4f16 v2);";
    case 0x5000E:
      return "void Cross3_F16(vec3f16 dest, vec3f16 v1, vec3f16 v2);";
    case 0x5000F:
      return "frac16 AbsVec3_F16(vec3f16 vec);";
    case 0x50010:
      return "frac16 AbsVec4_F16(vec4f16 vec);";
    case 0x50011:
      return "void MulVec3Mat33DivZ_F16(vec3f16 dest, vec3f16 vec, mat33f16 mat, frac16 n);";
    case 0x50012:
      return "void MulManyVec3Mat33DivZ_F16(mmv3m33d *s);";
    default:

      return NULL;
    }
}

static
const
char*
arm_op_dis_condition_mnemonic(const uint32_t op_)
{
  static const char *mnemonics[] = {"EQ","NE","CS","CC",
                                    "MI","PL","VS","VC",
                                    "HI","LS","GE","LT",
                                    "GT","LE","","NV"};
  return mnemonics[(op_ >> 28) & 0xF];
}

static
inline
uint8_t
arm_op_data_processing_opcode(const uint32_t op_)
{
  return ((op_ & 0x01E00000) >> 21);
}

static
const
char*
arm_op_data_processing_opcode_str(const uint32_t op_)
{
  static const char ARM_DATA_PROCESSING_OPCODES[][4] =
    {
      "AND","EOR","SUB","RSB",
      "ADD","ADC","SBC","RSC",
      "TST","TEQ","CMP","CMN",
      "ORR","MOV","BIC","MVN"
    };

  return ARM_DATA_PROCESSING_OPCODES[arm_op_data_processing_opcode(op_)];
}

static
const
char*
arm_op_shift_type(const uint32_t op_)
{
  static const char SHIFT_TYPES[][4] =
    {
      "LSL","LSR","ASR","ROR"
    };

  return SHIFT_TYPES[(op_ & 0x00000060) >> 5];
}

static
void
arm_op_print_shift(const uint32_t op_)
{
  /* Immediate  */
  if((op_ & 0x00000010) == 0x00000000)
    {
      if((op_ & 0x00000FF0) == 0x00000060)
        {
          printf("RRX");
        }
      else
        {
          uint8_t shift_amt;
          const char *shift_type;

          shift_amt  = ((op_ & 0x00000F80) >> 7);
          if(shift_amt == 0)
            return;

          shift_type = arm_op_shift_type(op_);

          printf(", %s %d",shift_type,shift_amt);
        }
    }
  else
    {
      uint8_t Rs;
      const char *shift_type;

      Rs         = ((op_ & 0x00000F00) >> 8);
      shift_type = arm_op_shift_type(op_);

      printf(", %s r%d",shift_type,Rs);
    }
}

static
void
arm_op_print_data_processing_op2(const uint32_t op_)
{
  char immediate;

  immediate = !!(op_ & 0x02000000);
  if(immediate)
    {
      uint8_t rotate;
      uint16_t imm;

      rotate = ((op_ & 0x00000F00) >> 8);
      imm    = ((op_ & 0x000000FF) >> 0);

      printf(", &%X",rotr32(imm,rotate*2));
    }
  else
    {
      uint8_t Rm;
      uint8_t shift_type;

      Rm = (op_ & 0x0000000F);

      printf(", r%d",Rm);
      arm_op_print_shift(op_);
    }
}

static
void
arm_op_print_data_processing_1(const uint32_t op_)
{
  uint8_t Rd;
  uint8_t set_condition_codes;
  const char *opcode;

  opcode              = arm_op_data_processing_opcode_str(op_);
  set_condition_codes = !!(op_ & 0x00100000);
  Rd                  = ((op_ & 0x0000F000) >> 12);

  printf("%s%s%s\tr%d",
         opcode,
         arm_op_dis_condition_mnemonic(op_),
         (set_condition_codes ? "S" : ""),
         Rd);

  arm_op_print_data_processing_op2(op_);
}

static
void
arm_op_print_data_processing_2(const uint32_t op_)
{
  uint8_t Rn;
  const char *opcode;

  opcode = arm_op_data_processing_opcode_str(op_);
  Rn     = ((op_ & 0x000F0000) >> 16);

  printf("%s%s\tr%d",
         opcode,
         arm_op_dis_condition_mnemonic(op_),
         Rn);

  arm_op_print_data_processing_op2(op_);
}

static
void
arm_op_print_data_processing_3(const uint32_t op_)
{
  char Rn;
  char Rd;
  char set_condition_codes;
  const char *opcode;

  opcode              = arm_op_data_processing_opcode_str(op_);
  set_condition_codes = !!(op_ & 0x00100000);
  Rn                  = ((op_ & 0x000F0000) >> 16);
  Rd                  = ((op_ & 0x0000F000) >> 12);

  printf("%s%s%s\tr%d, r%d",
         opcode,
         arm_op_dis_condition_mnemonic(op_),
         (set_condition_codes ? "S" : ""),
         Rd,
         Rn);

  arm_op_print_data_processing_op2(op_);
}

static
void
arm_op_print_data_processing(const uint32_t op_)
{
  uint8_t opcode;

  opcode = arm_op_data_processing_opcode(op_);
  switch(opcode)
    {
    case ARM_DP_MOV:
    case ARM_DP_MVN:
      arm_op_print_data_processing_1(op_);
      break;
    case ARM_DP_CMP:
    case ARM_DP_CMN:
    case ARM_DP_TEQ:
    case ARM_DP_TST:
      arm_op_print_data_processing_2(op_);
      break;
    default:
      arm_op_print_data_processing_3(op_);
      break;
    }
}

static
void
arm_op_print_branches(const uint32_t op_)
{
  uint32_t link;
  int32_t  offset;

  link = !!(op_ & 0x01000000);
  offset = sign_extend_24_32(op_ & 0x00FFFFFF);
  printf("B%s%s\t&%X",
         arm_op_dis_condition_mnemonic(op_),
         (link ? "L" : ""),
         offset);
}

static
void
arm_op_print_software_interupt(const uint32_t op_)
{
  printf("SWI%s\t&%X\t; %s",
         arm_op_dis_condition_mnemonic(op_),
         (op_ & 0x00FFFFFF),
         freedo_debug_swi_func(op_));
}

static
void
arm_op_print_single_data_transfer(const uint32_t op_)
{
  uint8_t store;
  uint8_t immediate;
  uint8_t pre_index;
  uint8_t up;
  uint8_t byte;
  uint8_t write_back;

  store      = !!(op_ & 0x00100000);
  write_back = !!(op_ & 0x00200000);
  byte       = !!(op_ & 0x00400000);
  
}

static
void
arm_op_print_block_data_transfer(const uint32_t op_)
{

}

static
void
arm_op_print_multiply(const uint32_t op_)
{
  uint8_t Rd;
  uint8_t Rn;
  uint8_t Rs;
  uint8_t Rm;
  uint8_t accumulate;
  uint8_t set_condition_code;

  Rd = ((op_ & 0x000F0000) >> 16);
  Rn = ((op_ & 0x0000F000) >> 12);
  Rs = ((op_ & 0x00000F00) >>  8);
  Rm = ((op_ & 0x0000000F) >>  0);
  accumulate         = !!(op_ & 0x00200000);
  set_condition_code = !!(op_ & 0x00100000);

  if(accumulate)
    printf("MLA%s%s\tr%d, r%d, r%d, r%d",
           arm_op_dis_condition_mnemonic(op_),
           (set_condition_code ? "S" : ""),
           Rd,Rm,Rs,Rn);
  else
    printf("MUL%s%s\tr%d, r%d, r%d",
           arm_op_dis_condition_mnemonic(op_),
           (set_condition_code ? "S" : ""),
           Rd,Rm,Rs);
}

static
void
arm_op_print_single_data_swap(const uint32_t op_)
{
  uint8_t byte;
  uint8_t Rn;
  uint8_t Rd;
  uint8_t Rm;

  byte = !!(op_ & 0x00400000);
  Rn   = ((op_ & 0x000F0000) >> 16);
  Rd   = ((op_ & 0x0000F000) >> 12);
  Rm   = ((op_ & 0x0000000F) >> 00);

  printf("SWP%s%s\tr%d, %d, [r%d]",
         arm_op_dis_condition_mnemonic(op_),
         (byte ? "B" : ""),
         Rd,
         Rm,
         Rn);
}

static arm_opcode_t OPCODE_TYPES[] =
  {
    {0x0E000000,0x0A000000,arm_op_print_branches}, // Branch
    {0x0C000000,0x00000000,arm_op_print_data_processing}, // Data processing
    {0x0FC000F0,0x00000090,arm_op_print_multiply}, // Multiply
    {0x0C000000,0x04000000,arm_op_print_single_data_transfer}, // Single data transfer
    {0x0E000000,0x08000000,arm_op_print_block_data_transfer}, // block data transfer
    {0x0FB00FF0,0x01000090,arm_op_print_single_data_swap}, // single data swap
    {0x0F000000,0x0F000000,arm_op_print_software_interupt}  // software interupt
  };

int
freedo_debug_arm_disassemble(uint32_t op_)
{
  int found;
  uint8_t condition;
  const char *condition_mnemonic;

  found = 0;
  for(uint64_t i = 0; i < sizeof(OPCODE_TYPES)/sizeof(OPCODE_TYPES[0]); i++)
    {
      if((op_ & OPCODE_TYPES[i].mask) == OPCODE_TYPES[i].pattern)
        {
          printf("%08X ",op_);
          if(OPCODE_TYPES[i].print)
            (*OPCODE_TYPES[i].print)(op_);
          printf("\n");

          found = 1;
        }
    }

  if(!found)
    {
      printf("%08X ::UNKNOWN::\n",op_);
      abort();
    }

  return 0;
}
