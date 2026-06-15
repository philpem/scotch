/* replay-tooling shim for the GDB simulator's generated <defs.h>.

   The vendored ARMulator sources begin with `#include "defs.h"`, which in the
   binutils-gdb tree is an autoconf-generated header pulling in the whole sim
   build configuration. None of the files we vendor (armemu.c, armemu32.c,
   armsupp.c, arminit.c, armvirt.c) reference any HAVE_ or WITH_ configuration
   macro, so a minimal header that supplies the standard library declarations
   they expect is sufficient. */

#ifndef REPLAY_ARMULATOR_DEFS_H
#define REPLAY_ARMULATOR_DEFS_H

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* The vendored ansidecl.h is older than the binutils-gdb sim sources and does
   not define ATTRIBUTE_FALLTHROUGH, which armemu.c uses. Provide a portable
   fallback before any vendored header is included. */
#ifndef ATTRIBUTE_FALLTHROUGH
# if defined(__GNUC__) && __GNUC__ >= 7
#  define ATTRIBUTE_FALLTHROUGH __attribute__((__fallthrough__))
# else
#  define ATTRIBUTE_FALLTHROUGH ((void) 0)
# endif
#endif

#endif /* REPLAY_ARMULATOR_DEFS_H */
