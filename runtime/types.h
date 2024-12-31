#ifndef _CILK_TYPES_H
#define _CILK_TYPES_H

#include <stdint.h>

typedef uint32_t worker_id;
#define WORKER_ID_FMT PRIu32
typedef struct __cilkrts_worker __cilkrts_worker;
typedef struct __cilkrts_stack_frame __cilkrts_stack_frame;
typedef struct global_state global_state;
typedef __uint128_t double_ptr;

// upper 64 bits are unbounded counter, lower 64 bits are the Closure * value
typedef __uint128_t stack_ptr;

// upper 64 bits are [index - 12 bits, counter - 48 bits][Closure *]
typedef __uint128_t stack_top_ptr;

#define NO_WORKER 0xffffffffu /* type worker_id */

// Constant representing invalid worker.
#define INVALID_WORKER (__cilkrts_worker *)0xbfbfbfbfbf

#endif /* _CILK_TYPES_H */
