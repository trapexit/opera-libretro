#include <stdio.h>
#include <stdint.h>
#include <assert.h>

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
uint8_t
arm_dis_condition(const uint32_t op_)
{
  return (op_ >> 28);
}

static
const
char*
arm_dis_condition_mnemonic(const uint8_t cond_)
{
  const char *mnemonics[] = {"EQ","NE","CS","CC",
                             "MI","PL","VS","VC",
                             "HI","LS","GE","LT",
                             "GT","LE","AL","NV"};
  return mnemonics[cond_ & 0xF];
}

enum arm_opidx_e
  {
    ARM_OP_ADC_IMM,
    ARM_OP_ADC_REG_IMM,
    ARM_OP_ADC_REG_REG,
    ARM_OP_ADD_IMM,
    ARM_OP_ADD_REG_IMM,
    ARM_OP_ADD_REG_REG,
    ARM_OP_AND_IMM,
    ARM_OP_AND_REG_IMM,
    ARM_OP_AND_REG_REG,
    ARM_OP_B,
    ARM_OP_BIC_IMM,
    ARM_OP_BIC_REG_IMM,
    ARM_OP_BIC_REG_REG,
    ARM_OP_BL,
    ARM_OP_BX,
    ARM_OP_CDP,
    ARM_OP_CMN_IMM,
    ARM_OP_CMN_REG_IMM,
    ARM_OP_CMN_REG_REG,
    ARM_OP_CMP_IMM,
    ARM_OP_CMP_REG_IMM,
    ARM_OP_CMP_REG_REG,
    ARM_OP_EOR_IMM,
    ARM_OP_EOR_REG_IMM,
    ARM_OP_EOR_REG_REG,
    ARM_OP_LDC,
    ARM_OP_LDM,
    ARM_OP_LDR,
    ARM_OP_MCR,
    ARM_OP_MLA,
    ARM_OP_MLA_S,
    ARM_OP_MOV_IMM,
    ARM_OP_MOV_REG_IMM,
    ARM_OP_MOV_REG_REG,
    ARM_OP_MRC,
    ARM_OP_MRS_CPSR,
    ARM_OP_MRS_SPSR,
    ARM_OP_MSR_CPSR,
    ARM_OP_MSR_CPSR_IMM,
    ARM_OP_MSR_CPSR_REG,
    ARM_OP_MSR_SPSR,
    ARM_OP_MSR_SPSR_IMM,
    ARM_OP_MSR_SPSR_REG,
    ARM_OP_MUL,
    ARM_OP_MUL_S,
    ARM_OP_MVN_IMM,
    ARM_OP_MVN_REG_IMM,
    ARM_OP_MVN_REG_REG,
    ARM_OP_ORR_IMM,
    ARM_OP_ORR_REG_IMM,
    ARM_OP_ORR_REG_REG,
    ARM_OP_RSB_IMM,
    ARM_OP_RSB_REG_IMM,
    ARM_OP_RSB_REG_REG,
    ARM_OP_RSC_IMM,
    ARM_OP_RSC_REG_IMM,
    ARM_OP_RSC_REG_REG,
    ARM_OP_SBC_IMM,
    ARM_OP_SBC_REG_IMM,
    ARM_OP_SBC_REG_REG,
    ARM_OP_SDR,
    ARM_OP_STC,
    ARM_OP_STM,
    ARM_OP_SUB_IMM,
    ARM_OP_SUB_REG_IMM,
    ARM_OP_SUB_REG_REG,
    ARM_OP_SWI,
    ARM_OP_SWP,
    ARM_OP_SWP_B,
    ARM_OP_TEQ_IMM,
    ARM_OP_TEQ_REG_IMM,
    ARM_OP_TEQ_REG_REG,
    ARM_OP_TST_IMM,
    ARM_OP_TST_REG_IMM,
    ARM_OP_TST_REG_REG,
    ARM_OP_UNDEFINED,
    ARM_OP_END
  };
typedef enum arm_opidx_e arm_opidx_t;

typedef struct arm_opcode_s arm_opcode_t;
struct arm_opcode_s
{
  arm_opidx_t idx;
  char name[4];
  uint32_t mask;
  uint32_t pattern;
};

arm_opcode_t OPCODES[] =
  {
    {ARM_OP_ADC_IMM,     "ADC",0x0FE00000,0x02A00000},
    {ARM_OP_ADC_REG_IMM, "ADC",0x0FE00010,0x00A00000},
    {ARM_OP_ADC_REG_REG, "ADC",0x0FE00090,0x00A00010},
    {ARM_OP_ADD_IMM,     "ADD",0x0FE00000,0x02800000},
    {ARM_OP_ADD_REG_IMM, "ADD",0x0FE00010,0x00800000},
    {ARM_OP_ADD_REG_REG, "ADD",0x0FE00090,0x00800010},
    {ARM_OP_AND_IMM,     "AND",0x0FE00000,0x02000000},
    {ARM_OP_AND_REG_IMM, "AND",0x0FE00010,0x00000000},
    {ARM_OP_AND_REG_REG, "AND",0x0FE00090,0x00000010},
    {ARM_OP_B,           "B  ",0x0F000000,0x0A000000},
    {ARM_OP_BIC_IMM,     "BIC",0x0FE00000,0x03C00000},
    {ARM_OP_BIC_REG_IMM, "BIC",0x0FE00010,0x01C00000},
    {ARM_OP_BIC_REG_REG, "BIC",0x0FE00090,0x01C00010},
    {ARM_OP_BL,          "BL ",0x0F000000,0x0B000000},
    {ARM_OP_BX,          "BX ",0x0FF000F0,0x01200010},
    {ARM_OP_CMN_IMM,     "CMN",0x0FF00000,0x03700000},
    {ARM_OP_CMN_REG_IMM, "CMN",0x0FF00010,0x01700000},
    {ARM_OP_CMN_REG_REG, "CMN",0x0FF00090,0x01700010},
    {ARM_OP_CMP_IMM,     "CMP",0x0FF00000,0x03500000},
    {ARM_OP_CMP_REG_IMM, "CMP",0x0FF00010,0x01500000},
    {ARM_OP_CMP_REG_REG, "CMP",0x0FF00090,0x01500010},
    {ARM_OP_EOR_IMM,     "EOR",0x0FE00000,0x02200000},
    {ARM_OP_EOR_REG_IMM, "EOR",0x0FE00010,0x00200000},
    {ARM_OP_EOR_REG_REG, "EOR",0x0FE00090,0x00200010},
    {ARM_OP_LDM,         "LDM",0x0E100000,0x08100000},
    {ARM_OP_LDR,         "LDR",0x0C100000,0x04100000},
    {ARM_OP_MLA,         "MLA",0x0FF000F0,0x00200090},
    {ARM_OP_MLA_S,       "MLA",0x0FF000F0,0x00300090},
    {ARM_OP_MOV_IMM,     "MOV",0x0FE00000,0x03A00000},
    {ARM_OP_MOV_REG_IMM, "MOV",0x0FE00010,0x01A00000},
    {ARM_OP_MOV_REG_REG, "MOV",0x0FE00090,0x01A00010},
    {ARM_OP_MRS_CPSR,    "MRS",0x0FFF0FFF,0x014F0000},
    {ARM_OP_MRS_SPSR,    "MRS",0x0FFF0FFF,0x010F0000},
    {ARM_OP_MSR_CPSR,    "MSR",0x0FFFFFF0,0x0129F000},
    {ARM_OP_MSR_CPSR_IMM,"MSR",0x0FFFF000,0x0128F000},
    {ARM_OP_MSR_CPSR_REG,"MSR",0x0FFFF000,0x0328F000},
    {ARM_OP_MSR_SPSR,    "MSR",0x0FFFFFF0,0x0169F000},
    {ARM_OP_MSR_SPSR_IMM,"MSR",0x0FFFF000,0x0168F000},
    {ARM_OP_MSR_SPSR_REG,"MSR",0x0FFFF000,0x0368F000},
    {ARM_OP_MUL,         "MUL",0x0FF000F0,0x00000090},
    {ARM_OP_MUL_S,       "MUL",0x0FF000F0,0x00100090},
    {ARM_OP_MVN_IMM,     "MVN",0x0FE00000,0x03E00000},
    {ARM_OP_MVN_REG_IMM, "MVN",0x0FE00010,0x01E00000},
    {ARM_OP_MVN_REG_REG, "MVN",0x0FE00090,0x01E00010},
    {ARM_OP_ORR_IMM,     "ORR",0x0FE00000,0x03800000},
    {ARM_OP_ORR_REG_IMM, "ORR",0x0FE00010,0x01800000},
    {ARM_OP_ORR_REG_REG, "ORR",0x0FE00090,0x01800010},
    {ARM_OP_RSB_IMM,     "RSB",0x0FE00000,0x02600000},
    {ARM_OP_RSB_REG_IMM, "RSB",0x0FE00010,0x00600000},
    {ARM_OP_RSB_REG_REG, "RSB",0x0FE00090,0x00600010},
    {ARM_OP_RSC_IMM,     "RSC",0x0FE00000,0x02E00000},
    {ARM_OP_RSC_REG_IMM, "RSC",0x0FE00010,0x00E00000},
    {ARM_OP_RSC_REG_REG, "RSC",0x0FE00090,0x00E00010},
    {ARM_OP_SBC_IMM,     "SBC",0x0FE00000,0x02C00000},
    {ARM_OP_SBC_REG_IMM, "SBC",0x0FE00010,0x00C00000},
    {ARM_OP_SBC_REG_REG, "SBC",0x0FE00090,0x00C00010},
    {ARM_OP_SDR,         "SDR",0x0C100000,0x04000000},
    {ARM_OP_STM,         "STM",0x0E100000,0x08000000},
    {ARM_OP_SUB_IMM,     "SUB",0x0FE00000,0x02400000},
    {ARM_OP_SUB_REG_IMM, "SUB",0x0FE00010,0x00400000},
    {ARM_OP_SUB_REG_REG, "SUB",0x0FE00090,0x00400010},
    {ARM_OP_SWI,         "SWI",0x0F000000,0x0F000000},
    {ARM_OP_SWP,         "SWP",0x0FF00FF0,0x01000090},
    {ARM_OP_SWP_B,       "SWP",0x0FF00FF0,0x01400090},
    {ARM_OP_TEQ_IMM,     "TEQ",0x0FE00000,0x03200000},
    {ARM_OP_TEQ_REG_IMM, "TEQ",0x0FE00010,0x01200000},
    {ARM_OP_TEQ_REG_REG, "TEQ",0x0FE00090,0x01200010},
    {ARM_OP_TST_IMM,     "TST",0x0FE00000,0x03000000},
    {ARM_OP_TST_REG_IMM, "TST",0x0FE00010,0x01000000},
    {ARM_OP_TST_REG_REG, "TST",0x0FE00090,0x01000010},
    {ARM_OP_CDP,         "CDP",0x0F000010,0x0E000000},
    {ARM_OP_LDC,         "LDC",0x0E100000,0x0C100000},
    {ARM_OP_STC,         "STC",0x0E100000,0x0C000000},
    {ARM_OP_MRC,         "MRC",0x0E100010,0x0C100010},
    {ARM_OP_MCR,         "MCR",0x0E000010,0x0C000010},
    {ARM_OP_UNDEFINED,   "UND",0x0E000010,0x06000010},

    {ARM_OP_END,"---",0,0xFFFFFFFF}
  };


int
freedo_debug_arm_disassemble(uint32_t op_)
{
  int found;
  uint8_t condition;
  const char *condition_mnemonic;

  condition = arm_dis_condition(op_);
  condition_mnemonic = arm_dis_condition_mnemonic(condition);

  found = 0;
  for(uint64_t i = 0; i < sizeof(OPCODES)/sizeof(OPCODES[0]); i++)
    {
      if((op_ & OPCODES[i].mask) == OPCODES[i].pattern)
        {
          //          printf("%08X %s%s\n",op_,OPCODES[i].name,condition_mnemonic);
          found = 1;
          break;
        }
    }

  if(!found)
    {
      printf("%08X ::UNKNOWN::\n",op_);
      abort();
    }


  return 0;
}
