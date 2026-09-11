#ifndef _PORT_COROUTINE_STACK_ALLOC_H
#define _PORT_COROUTINE_STACK_ALLOC_H

/* Minimal, C-safe declarations so extern/libco/libco.c can reach the guarded
 * stack allocator through LIBCO_MALLOC / LIBCO_FREE without pulling in the rest
 * of the port headers. CMake force-includes this file into libco.c only; see
 * include/port/coroutine_stack.h for what the allocator does and why. */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void *PartyBoard_CoroutineStackAlloc(size_t size);
void PartyBoard_CoroutineStackFree(void *pointer);

#ifdef __cplusplus
}
#endif

#endif
