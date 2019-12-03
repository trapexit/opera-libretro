#include <stdio.h>
#include <stdint.h>

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

int
freedo_debug_arm_disassemble(uint32_t op_)
{
  uint8_t condition;
  const char *condition_mnemonic;

  condition = arm_dis_condition(op_);
  condition_mnemonic = arm_dis_condition_mnemonic(condition);

  if((op_ & 0x0F000000) == 0x0F000000)
    printf("SWI%s 0x%x\n",condition_mnemonic,(op_ & 0x00FFFFFF));
  else if((op_ & 0x0B000000) == 0x0B000000)
    printf("BL%s 0x%x\n",condition_mnemonic,(op_ & 0x00FFFFFF));
  else if((op_ & 0x0A000000) == 0x0A000000)
    printf("B%s 0x%x\n",condition_mnemonic,(op_ & 0x00FFFFFF));

  return 0;
}
