#include <stdio.h>
#include <stdint.h>


int
freedo_debug_swi_func_raw(const char     *buf_,
                          const uint64_t  len_,
                          uint32_t        swi_)
{
  switch(swi_ & 0x00FFFFFF)
    {
    case 0x00101:
      snprintf(buf_,len_,"%s","void Debug(void);");
      break;
    case 0x10000:
      snprintf(buf_,len_,"%s","Item CreateSizedItem(int32 ctype, TagArg *p, int32 size);");
      break;
    case 0x10001:
      sprintf(buf_,len_,"%s","int32 WaitSignal(uint32 sigMask);");
      break;
    case 0x10002:
      sprintf(buf_,len_,"%s","Err SendSignal(Item task, uint32 sigMask);");
      break;
    case 0x10003:
      sprintf(buf_,len_,"%s","Err DeleteItem(Item i);");
      break;
    default:
      return -1;
    }

  return 0;
}
