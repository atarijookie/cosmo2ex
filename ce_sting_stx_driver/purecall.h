#ifndef _PURECALL_H_
#define _PURECALL_H_

// Calling code which isn't built by this gcc, see purecall.s. Used for STinG (Pure C) and for the MiNT kernel (gcc -mshort):
// both take int16 arguments on stack as 2 bytes, this gcc would push 4. The letters tell the arguments: l = long / pointer,
// w = int16. The result of functions returning int16 must be cast to int16, the upper half of d0 is undefined then.

#include "cetypes.h"

int32 pc_call_0   (void *fn);
int32 pc_call_l   (void *fn, int32 a);
int32 pc_call_w   (void *fn, int32 w);
int32 pc_call_ll  (void *fn, int32 a, int32 b);
int32 pc_call_lw  (void *fn, int32 a, int32 w);
int32 pc_call_lll (void *fn, int32 a, int32 b, int32 c);
int32 pc_call_llw (void *fn, int32 a, int32 b, int32 w);
int32 pc_call_lllw(void *fn, int32 a, int32 b, int32 c, int32 w);

// copy, fast when both addresses are even
void fastCopy(void *dst, const void *src, uint32 len);

#endif
