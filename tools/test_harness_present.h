#pragma once

#include <stdint.h>

#define OPERA_PRESENT_MAGIC UINT32_C(0x5652504f)
#define OPERA_PRESENT_VERSION UINT32_C(2)
#define OPERA_PRESENT_HEADER_SIZE 16U
// START carries sample Hz, nominal frame mHz, and scaled display aspect.
#define OPERA_PRESENT_START_SIZE 12U
#define OPERA_PRESENT_ASPECT_SCALE UINT32_C(1000000)
#define OPERA_PRESENT_MAX_PAYLOAD (8U * 1024U * 1024U)
#define OPERA_PRESENT_MAX_LABEL 1024U

enum opera_present_message_type
{
  OPERA_PRESENT_HELLO = 1,
  OPERA_PRESENT_ACK = 2,
  OPERA_PRESENT_START = 3,
  OPERA_PRESENT_VIDEO = 4,
  OPERA_PRESENT_AUDIO = 5,
  OPERA_PRESENT_END = 6
};

enum opera_present_ack_status
{
  OPERA_PRESENT_ACK_OK = 0,
  OPERA_PRESENT_ACK_BUSY = 1,
  OPERA_PRESENT_ACK_PROTOCOL = 2,
  OPERA_PRESENT_ACK_AUDIO = 3
};

static inline
void
opera_present_put_u32(uint8_t  *out_,
                      uint32_t  value_)
{
  out_[0] = (uint8_t)(value_ & UINT32_C(0xff));
  out_[1] = (uint8_t)((value_ >> 8U) & UINT32_C(0xff));
  out_[2] = (uint8_t)((value_ >> 16U) & UINT32_C(0xff));
  out_[3] = (uint8_t)((value_ >> 24U) & UINT32_C(0xff));
}

static inline
uint32_t
opera_present_get_u32(const uint8_t *in_)
{
  return (uint32_t)in_[0] |
    ((uint32_t)in_[1] << 8U) |
    ((uint32_t)in_[2] << 16U) |
    ((uint32_t)in_[3] << 24U);
}

static inline
void
opera_present_put_u64(uint8_t  *out_,
                      uint64_t  value_)
{
  opera_present_put_u32(out_, (uint32_t)(value_ & UINT64_C(0xffffffff)));
  opera_present_put_u32(out_ + 4U, (uint32_t)(value_ >> 32U));
}

static inline
uint64_t
opera_present_get_u64(const uint8_t *in_)
{
  return (uint64_t)opera_present_get_u32(in_) |
    ((uint64_t)opera_present_get_u32(in_ + 4U) << 32U);
}

static inline
void
opera_present_make_header(uint8_t  *out_,
                          uint32_t  type_,
                          uint32_t  payload_size_)
{
  opera_present_put_u32(out_, OPERA_PRESENT_MAGIC);
  opera_present_put_u32(out_ + 4U, OPERA_PRESENT_VERSION);
  opera_present_put_u32(out_ + 8U, type_);
  opera_present_put_u32(out_ + 12U, payload_size_);
}
