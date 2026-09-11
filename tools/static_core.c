/*
 * Static-core build support for tools/test_harness.c (-DSTATIC_CORE).
 *
 * The default harness dlopen()s the core .so at runtime; the cross-arch
 * verification lanes (qemu-user aarch64 / armv7 runs referenced in
 * jit.md) instead link core + harness into one static binary and resolve
 * the libretro entry points by name through this table.  Build recipe:
 * pass -DSTATIC_CORE when compiling test_harness.c and link this file
 * together with the core objects (see the zig examples in jit.md).
 */

#include "libretro.h"

#include <string.h>

typedef struct
{
  const char *name;
  void       *fn;
} static_core_sym_t;

#define STATIC_CORE_SYM(symbol_)  { #symbol_, (void *)&symbol_ }

static const static_core_sym_t s_static_core_syms[] =
{
  STATIC_CORE_SYM(retro_api_version),
  STATIC_CORE_SYM(retro_set_environment),
  STATIC_CORE_SYM(retro_set_video_refresh),
  STATIC_CORE_SYM(retro_set_audio_sample),
  STATIC_CORE_SYM(retro_set_audio_sample_batch),
  STATIC_CORE_SYM(retro_set_input_poll),
  STATIC_CORE_SYM(retro_set_input_state),
  STATIC_CORE_SYM(retro_init),
  STATIC_CORE_SYM(retro_deinit),
  STATIC_CORE_SYM(retro_get_system_info),
  STATIC_CORE_SYM(retro_get_system_av_info),
  STATIC_CORE_SYM(retro_set_controller_port_device),
  STATIC_CORE_SYM(retro_load_game),
  STATIC_CORE_SYM(retro_unload_game),
  STATIC_CORE_SYM(retro_run)
};

void
static_core_lookup(const char  *name_,
                   void       **out_)
{
  unsigned i_;

  *out_ = NULL;

  for(i_ = 0; i_ < (unsigned)(sizeof(s_static_core_syms) /
                              sizeof(s_static_core_syms[0])); i_++)
    {
      if(!strcmp(name_,s_static_core_syms[i_].name))
        {
          *out_ = s_static_core_syms[i_].fn;
          return;
        }
    }
}
