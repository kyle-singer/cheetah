#ifndef _CLOSURE_TYPE_H
#define _CLOSURE_TYPE_H

#include "cilk-internal.h"
#include "fiber.h"
#include "local-hypertable.h"
#include "mutex.h"
#include "types.h"

// Forward declaration
typedef struct Closure Closure;

// enum ClosureStatus {
//     /* Closure.status == 0 is invalid */
//     CLOSURE_RUNNING = 42,
//     CLOSURE_SUSPENDED,
//     CLOSURE_RETURNING,
//     CLOSURE_READY,
//     CLOSURE_PRE_INVALID, /* before first real use */
//     CLOSURE_POST_INVALID /* after destruction */
// };

enum ClosureStatus {
    CLOSURE_RUNNING = 0,
    CLOSURE_SUSPENDED,
    CLOSURE_SYNC,
    CLOSURE_RETURNING,
    CLOSURE_READY,
    CLOSURE_PRE_INVALID, /* before first real use */
    CLOSURE_POST_INVALID /* after destruction */
};

/*
 * the list of children is not distributed among
 * the children themselves, in order to avoid extra protocols
 * and locking.
 */
struct Closure {
    __cilkrts_stack_frame *frame; /* rest of the closure */

    _Atomic(Closure *) stack_top;

    struct cilk_fiber *left_most_fiber;
    struct cilk_fiber *call_parent_fiber;
    struct cilk_fiber *fiber_child;
    struct cilk_fiber *ext_fiber_child;

    worker_id owner_ready_deque; /* debug only */

    enum ClosureStatus status : 8; /* doubles as magic number */
    bool has_cilk_callee;
    bool exception_pending;
    _Atomic(__uint64_t) join_counter; /* number of outstanding spawned children */
    char *orig_rsp; /* the rsp one should use when sync successfully */

    Closure *callee;

    Closure *call_parent;  /* the "parent" closure that called */
    Closure *spawn_parent; /* the "parent" closure that spawned */

    Closure *left_sib;  // left *spawned* sibling in the closure tree
    _Atomic(uintptr_t) right_sib_removed; // right *spawned* sibling in the closure tree.  least significant bit 1 if this closure is marked for removal, 0 otherwise
    // right most *spawned* child in the closure tree
    _Atomic(Closure *) right_most_child;
    Closure *free_list_next;      // singly linked list kept track of in global state

    int64_t free_list_era;     // 0 for every closure except a top closure
    bool is_sentinel;   // debugging purposes
    /*
     * stuff related to ready deque.
     *
     * ANGE: for top of the ReadyDeque, prev_ready = NULL
     *       for bottom of the ReadyDeque, next_ready = NULL
     *       next_ready pointing downward, prev_ready pointing upward
     *
     *       top
     *  next | ^
     *       | | prev
     *       v |
     *       ...
     *  next | ^
     *       | | prev
     *       v |
     *      bottom
     */
    Closure *next_ready;
    Closure *prev_ready;

    _Atomic(hyper_table *) right_ht; // used by right siblings when theyre reducing with this closure
    hyper_table *child_ht;
    hyper_table *user_ht;

    _Atomic(worker_id) mutex_owner __attribute__((aligned(CILK_CACHE_LINE)));

} __attribute__((aligned(CILK_CACHE_LINE)));

#endif
