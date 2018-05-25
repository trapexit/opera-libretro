#ifndef LIBFREEDO_SIGN_EXTEND_H_INCLUDED
#define LIBFREEDO_SIGN_EXTEND_H_INCLUDED

#include "inline.h"

#include <stdint.h>

static
INLINE
int32_t
sign_extend_26_32(const int32_t x_)
{
  struct {int32_t x:26;} s;

  return s.x = x_;
}

#endif
