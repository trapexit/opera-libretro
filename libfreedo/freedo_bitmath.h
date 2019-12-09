#pragma once

#include "inline.h"

#include <stdint.h>

static
INLINE
uint32_t
rotl32(const uint32_t x_,
       const uint32_t r_)
{
  return ((x_ << r_) | (x_ >> ((-r_) & 31)));
}

static
INLINE
uint32_t
rotr32(const uint32_t x_,
       const uint32_t r_)
{
  return ((x_ >> r_) | (x_ << ((-r_) & 31)));
}

static
INLINE
int32_t
sign_extend_24_32(const int32_t x_)
{
  struct {int32_t x:24;} s;

  return s.x = x_;
}
