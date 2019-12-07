#pragma once

#include <stdint.h>

static
inline
uint32_t
rotl32(const uint32_t x_,
       const uint32_t r_)
{
  return ((x_ << r_) | (x_ >> ((-r_) & 31)));
}

static
inline
uint32_t
rotr32(const uint32_t x_,
       const uint32_t r_)
{
  return ((x_ >> r_) | (x_ << ((-r_) & 31)));
}
