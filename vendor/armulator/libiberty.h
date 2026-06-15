/* replay-tooling shim for binutils' <libiberty.h>.

   armsupp.c is the only vendored file that includes libiberty.h, and the only
   symbol it uses from it is ARRAY_SIZE. Providing just that avoids dragging in
   the full libiberty header (and library). */

#ifndef REPLAY_ARMULATOR_LIBIBERTY_H
#define REPLAY_ARMULATOR_LIBIBERTY_H

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof (a) / sizeof ((a)[0]))
#endif

#endif /* REPLAY_ARMULATOR_LIBIBERTY_H */
