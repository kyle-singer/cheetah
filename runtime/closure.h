#ifndef _CLOSURE_H
#define _CLOSURE_H

// Includes
#include <stdatomic.h>
#include "debug.h"

#include "cilk-internal.h"
#include "fiber.h"
#include "mutex.h"

#include "closure-type.h"

static inline bool Closure_is_removed(__cilkrts_worker *const w, worker_id self, uintptr_t right_sib_removed) {
    return (right_sib_removed & 1ULL) != 0;
}

static inline Closure *Closure_get_right_sib(Closure *closure) {
    uintptr_t right_sib_removed = atomic_load_explicit(&closure->right_sib_removed, memory_order_seq_cst);
    return (Closure *)(right_sib_removed & ~1ULL);
}

static inline void Closure_clean(__cilkrts_worker *const w, Closure *t) {
    printf("cleaning closure   %p    worker   %d\n", t, w->self);
    atomic_store_explicit(&t->join_counter, 0, memory_order_seq_cst);
    t->has_cilk_callee = false;
    t->spawn_parent = NULL;
    t->call_parent = NULL;
    atomic_store_explicit(&t->right_sib_removed, (uintptr_t)NULL & ~1ULL, memory_order_seq_cst); // reset remove bit 
    t->left_sib = NULL;
    t->left_most_fiber = NULL;
    t->status = CLOSURE_RUNNING;
    t->frame = NULL;
    t->fiber_child = NULL;

    // push to the tail of the linked list
    // atomic_store_explicit(&t->free_list_next, NULL, memory_order_seq_cst);
    // sanity checks
    if (w) {
        CILK_ASSERT(w, t->left_sib == (Closure *)NULL);
        CILK_ASSERT(w, Closure_get_right_sib(t) == (Closure *)NULL);
        CILK_ASSERT(w, t->right_most_child == (Closure *)NULL);

        CILK_ASSERT(w, t->user_ht == (hyper_table *)NULL);
        CILK_ASSERT(w, t->child_ht == (hyper_table *)NULL);
        CILK_ASSERT(w, t->right_ht == (hyper_table *)NULL);
    } else {
        CILK_ASSERT_G(t->left_sib == (Closure *)NULL);
        CILK_ASSERT_G(Closure_get_right_sib(t) == (Closure *)NULL);
        CILK_ASSERT_G(t->right_most_child == (Closure *)NULL);

        CILK_ASSERT_G(t->user_ht == (hyper_table *)NULL);
        CILK_ASSERT_G(t->child_ht == (hyper_table *)NULL);
        CILK_ASSERT_G(t->right_ht == (hyper_table *)NULL);
    }
}

static inline stack_ptr pack_free_list_node(uint64_t counter, Closure *closure) {
    if (counter != (counter & 0xFFFFFFFFFFFFULL)) {
        printf("counter    %d   excpect   %d\n", counter, counter & 0xFFFFFFFFFFFFULL);
    }
    CILK_ASSERT_G(counter == (counter & 0xFFFFFFFFFFFFULL)); // make sure counter only occupied 48 bits
    return ((stack_ptr)counter << 64) | (uintptr_t)closure;
}

static inline stack_top_ptr pack_free_list_top(__uint16_t index, uint64_t counter, Closure *closure) {
    CILK_ASSERT_G(index == (index & 0xFFF)); // make sure index only occupied 12 bits
    CILK_ASSERT_G(counter == (counter & 0xFFFFFFFFFFFFULL)); // make sure counter only occupied 48 bits
    return ((stack_ptr)index << 112) | ((stack_ptr)counter << 64) | (uintptr_t)closure;
}

static inline void unpack_free_list_top(stack_top_ptr top_ptr, __uint16_t *index, uint64_t *counter, Closure **closure) {
    *closure = (Closure *)(uintptr_t)(top_ptr & 0xFFFFFFFFFFFFFFFFULL);
    *counter = (uint64_t)((top_ptr >> 64) & 0xFFFFFFFFFFFFULL);
    *index = (__uint16_t)((top_ptr >> 112) & 0xFFF);
}

static inline void unpack_free_list_node(stack_ptr node, uint64_t *counter, Closure **closure) {
    *closure = (Closure *)(uintptr_t)(node & 0xFFFFFFFFFFFFFFFFULL);
    *counter = (uint64_t)((node >> 64) & 0xFFFFFFFFFFFFULL);
}

static inline Closure *get_free_list_closure(stack_ptr node) {
    return (Closure *)(node & 0xFFFFFFFFFFFFFFFFULL);
}

static inline __uint64_t pack_join_counter(int32_t join_counter, bool hit_sync) {
    __uint64_t jc_bits = (__uint64_t)((uint32_t)join_counter);
    __uint64_t jc_bool = ((__uint64_t)hit_sync) << 32;
    return (jc_bits | jc_bool);
}

static inline void unpack_join_counter(__uint64_t packed, int32_t *jc_bits, bool *hit_sync) {
    *jc_bits = (int32_t)(packed & 0xFFFFFFFFULL);
    *hit_sync = (bool)((packed >> 32) & 0x1);
}

static inline bool Closure_hit_sync(__uint64_t packed) {
    return (bool)((packed >> 32) & 0x1);
}

static inline __attribute__((always_inline)) double_ptr 
pack_pointers(__cilkrts_stack_frame ** ptr1, Closure * ptr2) {
    return ((double_ptr)ptr1 << 64) | (uintptr_t)ptr2;
}

static inline __attribute__((always_inline)) double_ptr 
pack(__cilkrts_stack_frame ** ptr1, int ptr2) {
    return ((double_ptr)ptr1 << 64) | ptr2;
}

static inline __attribute__((always_inline)) __cilkrts_stack_frame ** 
unpack_exc(double_ptr packed) {
    // Store exception pointer in high bits
    return (__cilkrts_stack_frame **)(packed >> 64);
}

static inline __attribute__((always_inline)) Closure * 
unpack_closure(double_ptr packed) {
    // Store closure pointer in low bits
    // gdb -> py print(2596132336160535047815725313949696 %(1 << 64))
    return (Closure *)packed;
}

static inline __attribute__((always_inline)) int 
unpack_offset(double_ptr packed) {
    // Store exception pointer in high bits
    return (int64_t)packed;
}

// STODO, implement better CAS loop. returns new value itself. dont need atomic load. use compare CAS

static inline __attribute((always_inline)) void
decrement_exc(__cilkrts_worker *w) {
    double_ptr old_value, new_value;
     __cilkrts_stack_frame **exc;
    Closure * closure;

    // Loop to retry if compare-and-swap fails
    do {
        old_value = atomic_load_explicit(&w->exc_closure, memory_order_seq_cst);
        exc = unpack_exc(old_value);
        closure = unpack_closure(old_value);

        // Prepare the new value
        new_value = pack_pointers(exc - 1, closure);

        // Attempt to update atomically
        // atomic_compare_exchange_strong returns true if the update was successful
    } while (!atomic_compare_exchange_strong(&w->exc_closure, &old_value, new_value));
}

static inline __attribute((always_inline)) void
increment_exc(__cilkrts_worker *w) {
    double_ptr old_value, new_value;
     __cilkrts_stack_frame **exc;
    Closure * closure;

    // Loop to retry if compare-and-swap fails
    do {
        old_value = atomic_load_explicit(&w->exc_closure, memory_order_seq_cst);
        exc = unpack_exc(old_value);
        closure = unpack_closure(old_value);

        // Prepare the new value
        new_value = pack_pointers(exc + 1, closure);

        // Attempt to update atomically
        // atomic_compare_exchange_strong returns true if the update was successful
    } while (!atomic_compare_exchange_strong(&w->exc_closure, &old_value, new_value));
}

static inline __attribute((always_inline)) void
decrement_join_counter_and_fetch(__cilkrts_worker *w, Closure *closure, int *new_jc, bool *did_hit_sync) {
    __uint64_t old_value, new_value;
    old_value = atomic_load_explicit(&closure->join_counter, memory_order_seq_cst);

    // Loop to retry if compare-and-swap fails
    do {
        int32_t join_counter;
        bool hit_sync;
        printf("trying decrement join counter   %p      w   %d\n", closure, w->self);
        // Prepare the new value
        unpack_join_counter(old_value, &join_counter, &hit_sync);
        new_value = pack_join_counter(join_counter - 1, hit_sync);

        *new_jc = join_counter - 1;
        *did_hit_sync = hit_sync;

        // Attempt to update atomically
        // atomic_compare_exchange_strong returns true if the update was successful
    } while (!atomic_compare_exchange_strong(&closure->join_counter, &old_value, new_value));
}

static inline __attribute((always_inline)) void
increment_join_counter(__cilkrts_worker *w, Closure *closure) {
    __uint64_t old_value, new_value;
    old_value = atomic_load_explicit(&closure->join_counter, memory_order_seq_cst);

    // Loop to retry if compare-and-swap fails
    do {
        int32_t join_counter;
        bool hit_sync;
        // Prepare the new value
        unpack_join_counter(old_value, &join_counter, &hit_sync);
        printf("trying increment join counter   %p      w   %d    counter   %d\n", closure, w->self, join_counter);
        new_value = pack_join_counter(join_counter + 1, hit_sync);

        // Attempt to update atomically
        // atomic_compare_exchange_strong returns true if the update was successful
    } while (!atomic_compare_exchange_strong(&closure->join_counter, &old_value, new_value));
}

static inline __attribute((always_inline)) void
update_exc(__cilkrts_worker *w, __cilkrts_stack_frame ** exc) {
    double_ptr old_value, new_value;
    Closure * closure;

    // Loop to retry if compare-and-swap fails
    do {
        old_value = atomic_load_explicit(&w->exc_closure, memory_order_seq_cst);
        closure = unpack_closure(old_value);

        // Prepare the new value
        new_value = pack_pointers(exc, closure);

        // Attempt to update atomically
        // atomic_compare_exchange_strong returns true if the update was successful
    } while (!atomic_compare_exchange_strong(&w->exc_closure, &old_value, new_value));
}

static inline __attribute((always_inline)) void
update_closure(__cilkrts_worker *w, Closure * closure) {
    double_ptr old_value, new_value;
    __cilkrts_stack_frame **exc;

    // Loop to retry if compare-and-swap fails
    do {
        printf("updating closure for worker     %d    closure %p\n", w->self, closure);
        old_value = atomic_load_explicit(&w->exc_closure, memory_order_seq_cst);
        exc = unpack_exc(old_value);

        // Prepare the new value
        new_value = pack_pointers(exc, closure);

        // Attempt to update atomically
        // atomic_compare_exchange_strong returns true if the update was successful
    } while (!atomic_compare_exchange_strong(&w->exc_closure, &old_value, new_value));
}

static inline __attribute((always_inline)) bool
update_exc_closure_abort(__cilkrts_worker *w, __cilkrts_stack_frame **old_exc, Closure * old_closure, __cilkrts_stack_frame **new_exc, Closure * new_closure) {
    double_ptr old_value = pack_pointers(old_exc, old_closure);
    double_ptr new_value = pack_pointers(new_exc, new_closure);
    return atomic_compare_exchange_strong(&w->exc_closure, &old_value, new_value);
}

static inline __attribute((always_inline)) Closure *
fetch_and_update_closure(__cilkrts_worker *w, Closure * new_closure) {
    double_ptr old_value, new_value;
    __cilkrts_stack_frame **exc;
    Closure *closure;

    // Loop to retry if compare-and-swap fails
    do {
        printf("fetch updating closure for worker     %d  closure     %p\n", w->self, new_closure);
        old_value = atomic_load_explicit(&w->exc_closure, memory_order_seq_cst);
        exc = unpack_exc(old_value);
        closure = unpack_closure(old_value);

        // Prepare the new value
        new_value = pack_pointers(exc, new_closure);

        // Attempt to update atomically
        // atomic_compare_exchange_strong returns true if the update was successful
    } while (!atomic_compare_exchange_strong(&w->exc_closure, &old_value, new_value));

    return closure;
}

static inline __attribute((always_inline)) bool
fetch_and_update_closure_abort(__cilkrts_worker *w, Closure * new_closure, Closure *old_closure, double_ptr* fetch) {
    printf("fetch updating closure abort for worker     %d   closure    %p\n", w->self, new_closure);
    *fetch = atomic_load_explicit(&w->exc_closure, memory_order_seq_cst);
    __cilkrts_stack_frame **exc = unpack_exc(*fetch);
    *fetch = pack_pointers(exc, (Closure *)NULL);
    double_ptr new_value = pack_pointers(exc, new_closure);

    return atomic_compare_exchange_strong(&w->exc_closure, fetch, new_value);
}

static inline __attribute((always_inline)) bool
update_closure_expected_abort(__cilkrts_worker *w, __cilkrts_stack_frame** old_exc, Closure* new_closure, double_ptr* fetch) {
    printf("updating closure expected abort for worker     %d   old_exc     %p   new closure    %p     old closure  %p\n", w->self, old_exc, new_closure, unpack_closure(*fetch));
    double_ptr new_value = pack_pointers(old_exc, new_closure);

    return atomic_compare_exchange_strong(&w->exc_closure, fetch, new_value);
}

static inline const char *Closure_status_to_str(enum ClosureStatus status) {
    switch (status) {
    case CLOSURE_RUNNING:
        return "running";
    case CLOSURE_SUSPENDED:
        return "suspended";
    case CLOSURE_RETURNING:
        return "returning";
    case CLOSURE_READY:
        return "ready";
    case CLOSURE_PRE_INVALID:
        return "pre-invalid";
    case CLOSURE_POST_INVALID:
        return "post-invalid";
    case CLOSURE_SYNC:
        return "sync";
    default:
        return "unknown";
    }
}

#if CILK_DEBUG
// static inline void Closure_assert_ownership(__cilkrts_worker *const w,
//                                             worker_id self,
//                                             Closure *t) {
//     CILK_ASSERT(
//         w, atomic_load_explicit(&t->mutex_owner, memory_order_seq_cst) == self);
// }

static inline void Closure_assert_alienation(__cilkrts_worker *const w,
                                             worker_id self,
                                             Closure *t) {
    CILK_ASSERT(
        w, atomic_load_explicit(&t->mutex_owner, memory_order_seq_cst) != self);
}

static inline void Closure_checkmagic(__cilkrts_worker *const w, Closure *t) {
    switch (t->status) {
    case CLOSURE_RUNNING:
    case CLOSURE_SUSPENDED:
    case CLOSURE_RETURNING:
    case CLOSURE_READY:
        return;
    case CLOSURE_POST_INVALID:
        CILK_ABORT(w, "destroyed closure");
    default:
        CILK_ABORT(w, "invalid closure");
    }
}

// #define Closure_assert_ownership(w, s, t) Closure_assert_ownership(w, s, t)
#define Closure_assert_alienation(w, s, t) Closure_assert_alienation(w, s, t)
#define Closure_checkmagic(w, t) Closure_checkmagic(w, t)
#else
// #define Closure_assert_ownership(w, s, t)
#define Closure_assert_alienation(w, s, t)
#define Closure_checkmagic(w, t)
#endif // CILK_DEBUG

#include "cilk2c.h"
#include "global.h"
#include "internal-malloc.h"
#include "readydeque.h"

static inline void Closure_change_status(__cilkrts_worker *const w, Closure *t,
                                         enum ClosureStatus old,
                                         enum ClosureStatus status) {
    printf("w   %d      closure status actually     %s\n", w->self, Closure_status_to_str(t->status));
    CILK_ASSERT(w, t->status == old);
    t->status = status;
}

static inline void Closure_set_status(__cilkrts_worker *const w, Closure *t,
                                      enum ClosureStatus status) {
    t->status = status;
}

// static inline int Closure_trylock(__cilkrts_worker *const w, worker_id self, Closure *t) {
//     Closure_checkmagic(w, t);
//     worker_id current_owner =
//         atomic_load_explicit(&t->mutex_owner, memory_order_seq_cst);
//     if ((current_owner == NO_WORKER) &&
//         atomic_compare_exchange_weak_explicit(&t->mutex_owner, &current_owner,
//                                               self, memory_order_acq_rel,
//                                               memory_order_seq_cst))
//         return 1;

//     return 0;
// }

// static inline void Closure_lock(__cilkrts_worker *const w, worker_id self, Closure *t) {
//     Closure_checkmagic(w, t);
//     while (true) {
//         worker_id current_owner =
//             atomic_load_explicit(&t->mutex_owner, memory_order_seq_cst);
//         if ((current_owner == NO_WORKER) &&
//             atomic_compare_exchange_weak_explicit(
//                 &t->mutex_owner, &current_owner, self, memory_order_acq_rel,
//                 memory_order_seq_cst))
//             break;
//         busy_loop_pause();
//     }
// }

// static inline void Closure_unlock(__cilkrts_worker *const w, worker_id self, Closure *t) {
//     Closure_checkmagic(w, t);
//     Closure_assert_ownership(w, self, t);
//     atomic_store_explicit(&t->mutex_owner, NO_WORKER, memory_order_release);
// }

// need to be careful when calling this function --- we check whether a
// frame is set stolen (i.e., has a full frame associated with it), but note
// that the setting of this can be delayed.  A thief can steal a spawned
// frame, but it cannot fully promote it until it remaps its TLMM stack,
// because the flag field is stored in the frame on the TLMM stack.  That
// means, a frame can be stolen, in the process of being promoted, and
// mean while, the stolen flag is not set until finish_promote.
static inline int Closure_at_top_of_stack(__cilkrts_worker *const w,
                                          __cilkrts_stack_frame *const frame) {
    __cilkrts_stack_frame **head =
        unpack_exc(atomic_load_explicit(&w->exc_closure, memory_order_seq_cst));
    __cilkrts_stack_frame **tail =
        atomic_load_explicit(&w->tail, memory_order_seq_cst);
    
    return (head == tail && __cilkrts_stolen(frame));
}

static inline int32_t get_join_counter(__uint64_t packed) {
    return (int32_t)(packed & 0xFFFFFFFFULL);
}

static inline int Closure_has_children(Closure *cl) {
    return (cl->has_cilk_callee || get_join_counter(atomic_load_explicit(&cl->join_counter, memory_order_seq_cst)) != 0);
}

static inline void Closure_init(Closure *t, __cilkrts_stack_frame *frame) {
    atomic_store_explicit(&t->mutex_owner, NO_WORKER, memory_order_seq_cst);
    t->owner_ready_deque = NO_WORKER;
    t->status = CLOSURE_PRE_INVALID;
    t->has_cilk_callee = false;
    t->exception_pending = false;
    atomic_store_explicit(&t->join_counter, 0, memory_order_seq_cst);

    t->frame = frame;
    printf("null fiber for closure  %p\n", t);
    t->fiber_child = NULL;
    t->ext_fiber_child = NULL;

    t->orig_rsp = NULL;

    t->callee = NULL;

    t->call_parent = NULL;
    t->spawn_parent = NULL;

    t->left_sib = NULL;
    // t->right_sib = NULL;
    t->right_most_child = NULL;

    t->next_ready = NULL;
    t->prev_ready = NULL;

    t->user_ht = NULL;
    t->child_ht = NULL;
    t->right_ht = NULL;
}

static inline Closure *Closure_create(__cilkrts_worker *const w,
                                      __cilkrts_worker *const closure_owner,
                                      Closure *new_closure,
                                      __cilkrts_stack_frame *sf) {
    /* cilk_internal_malloc returns sufficiently aligned memory */
    // Closure *new_closure =
    //     cilk_internal_malloc(w, sizeof(*new_closure), IM_CLOSURE);
    CILK_ASSERT(w, new_closure >= closure_owner->closure_stack_head);
    CILK_ASSERT(w, new_closure != NULL);
    Closure *init_val = (Closure *)NULL;

    if (atomic_compare_exchange_strong(&new_closure->stack_top, &init_val, closure_owner->closure_stack_head)) {
        printf("create closure  %p      worker  %d      closure_owner   %d\n", new_closure, w->self, closure_owner->self);
        CILK_ASSERT(w, new_closure->status == CLOSURE_PRE_INVALID);
        new_closure->stack_top = closure_owner->closure_stack_head;
        Closure_init(new_closure, sf);

        cilkrts_alert(CLOSURE, w, "Allocate closure %p", (void *)new_closure);

        return new_closure;
    } else {
        // another thief did init
        printf("w    %d      another thief created closure    %p\n", w->self, new_closure);
        return new_closure;
    }
    
    // if (new_closure->stack_top != NULL) {
        
    // }
    // CILK_ASSERT(w, new_closure->stack_top == NULL);
    // removed this assertion because benign racing between theives
}

static inline void Closure_clear_frame(Closure *cl) {
    cl->frame = NULL;
}

static inline void Closure_set_frame(__cilkrts_worker *w,
                                     Closure *cl,
                                      __cilkrts_stack_frame *sf) {
    CILK_ASSERT(w, !cl->frame);
    printf("setting frame   %p      closure   %p    worker  %d\n", sf, cl, w->self);
    cl->frame = sf;
}


// // Note that we must have the lock on the parent when invoking this function
// static inline void double_link_children(__cilkrts_worker *const w,
//                                         Closure *left, Closure *right) {

//     if (left) {
//         CILK_ASSERT(w, CLosure_get_right_sib(left) == (Closure *)NULL);
//         left->right_sib = right;
//     }

//     if (right) {
//         CILK_ASSERT(w, right->left_sib == (Closure *)NULL);
//         right->left_sib = left;
//     }
// }

static void increment_free_list_size(__cilkrts_worker *w) {
    int free_list_size = atomic_load_explicit(&w->g->free_list_size, memory_order_seq_cst);
    int new_value;

    // Loop to retry if compare-and-swap fails
    do {
        new_value = free_list_size + 1;

        // Attempt to update atomically
        // atomic_compare_exchange_strong returns true if the update was successful
    } while (!atomic_compare_exchange_strong(&w->g->free_list_size, &free_list_size, new_value));

    printf("worker   %d   incremented free list size to   %d\n", w->self, new_value);
}

static void decrement_free_list_size(__cilkrts_worker *w) {
    int free_list_size = atomic_load_explicit(&w->g->free_list_size, memory_order_seq_cst);
    int new_value;

    // Loop to retry if compare-and-swap fails
    do {
        new_value = free_list_size - 1;

        // Attempt to update atomically
        // atomic_compare_exchange_strong returns true if the update was successful
    } while (!atomic_compare_exchange_strong(&w->g->free_list_size, &free_list_size, new_value));

    printf("worker   %d   decremented free list size to   %d\n", w->self, new_value);

    // CILK_ASSERT(w, new_value >= 0);
    
}

static inline __attribute__((always_inline)) double_ptr 
pack_stamped_closure(int stamp, Closure * closure) {
    return (((double_ptr)stamp) << 64) | (uintptr_t)closure;
}

// use free_list_era as a stamp
static inline __attribute__((always_inline)) Closure *get_closure(double_ptr stamped_closure) {
    return (Closure *)stamped_closure;
}

static inline __attribute__((always_inline)) int get_stamp(double_ptr stamped_closure) {
    return (int)(stamped_closure >> 64);
}

static void finish_global_stack_update(global_state *g, __uint16_t index, __uint64_t counter, Closure *recent_closure) {
    Closure * old_closure = get_free_list_closure(atomic_load_explicit(&g->free_list[index], memory_order_seq_cst));
    stack_ptr old_stack_node = pack_free_list_node(counter - 1, old_closure);
    stack_ptr new_stack_node = pack_free_list_node(counter, recent_closure);
    atomic_compare_exchange_strong_explicit(&g->free_list[index], &old_stack_node, new_stack_node, memory_order_seq_cst, memory_order_seq_cst);
}

static bool global_stack_push(struct __cilkrts_worker *const w, Closure *t) {
    Closure *recent_closure = NULL;
    stack_top_ptr stamped_recent_closure;

    __uint16_t index = 0;
    __uint64_t counter = 0;
    CILK_ASSERT(w, t->free_list_next == NULL);

    while (true) {
        printf("global freeing closure     %p    worker    %d\n", t, w->self);
        // if second try, cur_tail already got updated on failed CAS stodo
        stamped_recent_closure = atomic_load_explicit(&w->g->free_list_top, memory_order_seq_cst);
        unpack_free_list_top(stamped_recent_closure, &index, &counter, &recent_closure);
        printf("push to global stack list   index   %d   counter   %d   head   %p   w  %d\n", index, counter, recent_closure, w->self);
        // CILK_ASSERT(w, cur_head); // we use a dummy node to keep this non-null

        // help previous modification
        finish_global_stack_update(w->g, index, counter, recent_closure);
        
        if (index == 999) {
            return false;
        }

        stack_ptr above_top = atomic_load_explicit(&w->g->free_list[index + 1], memory_order_seq_cst);
        __uint64_t new_counter;
        Closure *new_closure;
        unpack_free_list_node(above_top, &new_counter, &new_closure);
        if (atomic_compare_exchange_strong_explicit(&w->g->free_list_top, &stamped_recent_closure, pack_free_list_top(index + 1, new_counter + 1, t), memory_order_seq_cst, memory_order_seq_cst)) {
            printf("linking new closure  worker   %d    closure   %p   new_ind   %d prev was    %p\n", w->self, t, index + 1, recent_closure);
            return true;
        }
    } 
}

static void Closure_stack_free(struct __cilkrts_worker *const w,
                                   Closure *t) {
                                
    CILK_ASSERT(w, t == t->stack_top);
    if (w->closure_stack_head == t) {
        printf("resetting stack tail free   worker  %d\n", w->self);
        w->closure_stack_head = NULL;
        atomic_store_explicit(&w->closure_stack_tail, NULL, memory_order_seq_cst);
    }

    Closure_clean(w, t);

    // CILK_ASSERT(w, !atomic_load_explicit(&t->free_list_next, memory_order_seq_cst));

    if (w->free_list_head == NULL) {
        printf("worker local free   closure   %p    w    %d  was null\n", t, w->self);
        w->free_list_head = t;
        w->free_list_head->free_list_next = NULL;
        w->local_free_list_size += 1;
    } else {
        if (w->local_free_list_size > 10) {
            printf("global free   closure  %p   w   %d\n", t, w->self);
            bool success = global_stack_push(w, t);
            if (success) {
                return;
            }
        }
        // global stack full, add to local stack
        printf("worker local free   closure   %p   w    %d\n", t, w->self);
        Closure *old_head = w->free_list_head;
        t->free_list_next = old_head;
        w->free_list_head = t;
        CILK_ASSERT(w, w->free_list_head->free_list_next == old_head);
        w->local_free_list_size += 1;
    }
}

static inline void Closure_clean_root(Closure *t) {
    atomic_store_explicit(&t->join_counter, 0, memory_order_seq_cst);
    t->has_cilk_callee = false;
    t->spawn_parent = NULL;
    t->call_parent = NULL;
    atomic_store_explicit(&t->right_sib_removed, (uintptr_t)NULL & ~1ULL, memory_order_seq_cst); // reset remove bit 
    t->left_sib = NULL;
    t->left_most_fiber = NULL;
    t->status = CLOSURE_RUNNING;
    // atomic_store_explicit(&t->free_list_next, NULL, memory_order_seq_cst);
    // sanity checks
    CILK_ASSERT_G(t->left_sib == (Closure *)NULL);
    CILK_ASSERT_G(Closure_get_right_sib(t) == (Closure *)NULL);
    CILK_ASSERT_G(t->right_most_child == (Closure *)NULL);

    CILK_ASSERT_G(t->user_ht == (hyper_table *)NULL);
    CILK_ASSERT_G(t->child_ht == (hyper_table *)NULL);
    CILK_ASSERT_G(t->right_ht == (hyper_table *)NULL);
}

static inline void Closure_reset_children(__cilkrts_worker *const w, worker_id self, Closure *parent) {
    Closure *next_child = atomic_load_explicit(&parent->right_most_child, memory_order_seq_cst);
    printf("w   %d  resetting children     parent  %p   right most child was   %p\n", w->self, parent, next_child);
    Closure *orig = next_child;
    bool reached_end = false;

    if (next_child == NULL) {
        printf("no children\n");
        // no siblings
        return;
    }

    uintptr_t old_value = atomic_load_explicit(&next_child->right_sib_removed, memory_order_seq_cst);
    uintptr_t new_value;
    Closure *prev_child = next_child;
    hyper_table *active_ht = NULL;
    printf("restting children    w   %d    child    %p     parent    %p\n", w->self, prev_child, parent);
    while (Closure_is_removed(w, self, old_value)) {
        // must have left_sib pointer by the time you sync
        next_child = next_child->left_sib;

        if (prev_child->right_ht) {
            printf("merging hashtables child     %p     parent   %p\n", prev_child, parent);
            active_ht = merge_two_hts(w, prev_child->right_ht, active_ht);
            prev_child->right_ht = NULL; // no more children returning at this point
        }

        Closure_clean(w, prev_child);
        printf("unlinking child     %p\n", prev_child);

        if (prev_child == prev_child->stack_top) {
            printf("freeing child stack   %p\n", prev_child);
            Closure_stack_free(w, prev_child);
        }

        prev_child = next_child;

        if (next_child == NULL) {
            reached_end = true;
            break;
        }
        old_value = atomic_load_explicit(&next_child->right_sib_removed, memory_order_seq_cst);
    }

    printf("non removed child is    %p    worker    %d\n", next_child, w->self);

    bool is_removed = atomic_compare_exchange_strong(&parent->right_most_child, &orig, NULL);
    printf("right most child was actually    %p  worker   %d\n", orig, w->self);
    CILK_ASSERT(w, is_removed);
    CILK_ASSERT(w, reached_end);

    parent->child_ht = merge_two_hts(w, parent->child_ht, active_ht);
}

/***
 * Only the scheduler is allowed to alter the closure tree.
 * Consequently, these operations are private.
 *
 * Insert the newly created child into the closure tree.
 * The child closure is newly created, which makes it the new right
 * most child of parent.  Setup the left/right sibling for this new
 * child, and reset the parent's right most child pointer.
 *
 * Note that we don't need locks on the children to double link them.
 * The old right most child will not follow its right_sib link until
 * it's ready to return, and it needs lock on the parent to do so, which
 * we are holding.  The pointer to new right most child is not visible
 * to anyone yet, so we don't need to lock that, either.
 * double linking left and right; the right is always the new child
 ***/
static inline
void Closure_add_child(__cilkrts_worker *const w, worker_id self, Closure *parent,
                       Closure *child) {

    /* ANGE: w must have the lock on parent */
    // Closure_assert_ownership(w, self, parent);
    /* ANGE: w must NOT have the lock on child */
    
    // Closure_assert_alienation(w, self, child);

    // setup sib links between parent's right most child and the new child
    Closure * next_child;

    // update right most child first
    // need a loop because it may be returning / deleted / cleaned up by other siblings
    do {
        printf("w   %d  adding child    %p      parent  %p\n", w->self, child, parent);
        next_child = atomic_load_explicit(&parent->right_most_child, memory_order_seq_cst);

        // Attempt to update atomically
        // atomic_compare_exchange_strong returns true if the update was successful
    } while (!atomic_compare_exchange_strong(&parent->right_most_child, &next_child, child));

    if (next_child == NULL) {
        // no siblings
        return;
    }

    uintptr_t old_value = atomic_load_explicit(&next_child->right_sib_removed, memory_order_seq_cst);
    uintptr_t new_value;
    Closure *prev_child = next_child;
    // Loop to retry if compare-and-swap fails
    do {
        bool is_last = false;
        printf("adding right sibling  worker   %d   right child   %p    sib   %p\n", w->self, child, old_value);

        while (Closure_is_removed(w, self, old_value)) {
            printf("removed child   worker   %d\n", w->self);
            next_child = next_child->left_sib; // must have left_sib pointer by the time you set remove bit
            Closure_clean(w, prev_child);

            if (prev_child == prev_child->stack_top) {
                printf("freeing child stack   %p\n", prev_child);
                Closure_stack_free(w, prev_child);
            }
            
            prev_child = next_child;
            if (next_child == NULL) {
                is_last = true;
                break;
            }
            old_value = atomic_load_explicit(&next_child->right_sib_removed, memory_order_seq_cst);
        }

        if (is_last) {
            // no sibling pointers to update
            return;
        }
        CILK_ASSERT(w, !Closure_is_removed(w, self, old_value));

        // child->right_sib_removed may or may not be marked depending on race with victim

        // Prepare the new value
        new_value = (uintptr_t)child & ~1ULL;

        // Attempt to update atomically
        // atomic_compare_exchange_strong returns true if the update was successful
    } while (!atomic_compare_exchange_strong(&next_child->right_sib_removed, &old_value, new_value));

    printf("adding left sib    %p   for child   %p\n", next_child, child);
    child->left_sib = next_child;
}

// // unlink the closure from its left and right siblings
// // Note that we must have the lock on the parent when invoking this function
// static inline void unlink_child(__cilkrts_worker *const w, Closure *cl) {

//     if (cl->left_sib) {
//         CILK_ASSERT(w, cl->left_sib->right_sib == cl);
//         cl->left_sib->right_sib = cl->right_sib;
//     }
//     if (cl->right_sib) {
//         CILK_ASSERT(w, cl->right_sib->left_sib == cl);
//         cl->right_sib->left_sib = cl->left_sib;
//     }
//     // used only for error checking
//     cl->left_sib = (Closure *)NULL;
//     cl->right_sib = (Closure *)NULL;
// }

/***
 * Remove the child from the closure tree.
 * At this point we should already have reduced all views that this
 * child has.  We need to unlink it from its left/right sibling, and reset
 * the right most child pointer in parent if this child is currently the
 * right most child.
 *
 * Note that we need locks both on the parent and the child.
 * We always hold lock on the parent when unlinking a child, so only one
 * child gets unlinked at a time, and one child gets to modify the steal
 * tree at a time.
 ***/
// static inline
// void Closure_remove_child(__cilkrts_worker *const w, worker_id self, Closure *parent,
//                           Closure *child) {
//     CILK_ASSERT(w, child);
//     CILK_ASSERT(w, parent == child->spawn_parent);
//     printf("w   %d  removing child    %p      parent  %p\n", w->self, child, parent);

//     // Closure_assert_ownership(w, self, parent);
//     // Closure_assert_ownership(w, self, child);

//     if (child == parent->right_most_child) {
//         CILK_ASSERT(w, child->right_sib == (Closure *)NULL);
//         parent->right_most_child = child->left_sib;
//     }

//     CILK_ASSERT(w, child->right_ht == (hyper_table *)NULL);

//     unlink_child(w, child);
// }

static inline void Closure_mark_child_remove(__cilkrts_worker *const w, worker_id self, Closure *child) {
    CILK_ASSERT(w, child->status == CLOSURE_RETURNING);
    child->status = CLOSURE_RUNNING;    // may reuse the stack entry on a later steal
    
    uintptr_t old_value, new_value;

    // Loop to retry if compare-and-swap fails
    do {
        printf("marking child for removal     %d    closure  %p   parent  %p\n", w->self, child, child->spawn_parent);
        old_value = atomic_load_explicit(&child->right_sib_removed, memory_order_seq_cst);
        CILK_ASSERT(w, !Closure_is_removed(w, self, old_value));

        // Prepare the new value
        new_value = old_value | 1ULL;

        // Attempt to update atomically
        // atomic_compare_exchange_strong returns true if the update was successful
    } while (!atomic_compare_exchange_strong(&child->right_sib_removed, &old_value, new_value));
    child->spawn_parent = NULL;

}

/***
 * This function is called during promote_child, when we know we have multiple
 * frames in the stacklet.
 * We create a new closure for the new spawn_parent, and temporarily use
 * that to represent all frames in between the new spawn_parent and the
 * old closure on top of the victim's deque.  In case where some other child
 * of the old closure returns, it needs to know that the old closure has
 * outstanding call children, so it won't resume the suspended old closure
 * by mistake.
 ***/
static inline
void Closure_add_temp_callee(__cilkrts_worker *const w, Closure *caller,
                             Closure *callee) {
    CILK_ASSERT(w, !(caller->has_cilk_callee));
    CILK_ASSERT(w, callee->spawn_parent == NULL);

    callee->call_parent = caller;
    caller->has_cilk_callee = true;
}

static inline
void Closure_add_callee(__cilkrts_worker *const w, Closure *caller,
                        Closure *callee) {
    // ANGE: instead of checking has_cilk_callee, we just check if callee is
    // NULL, because we might have set the has_cilk_callee in
    // Closure_add_tmp_callee to prevent the closure from being resumed.
    printf("adding callee   worker  %d   caller   %p    callee    %p\n", w->self, caller, callee);
    CILK_ASSERT(w, caller->callee == NULL);
    CILK_ASSERT(w, callee->spawn_parent == NULL);
    CILK_ASSERT(w, (callee->frame->flags & CILK_FRAME_DETACHED) == 0);

    callee->call_parent = caller;
    caller->callee = callee;
    caller->has_cilk_callee = true;
}

static inline
void Closure_remove_callee(__cilkrts_worker *const w, Closure *caller) {

    // A child is not double linked with siblings if it is called
    // so there is no need to unlink it.
    CILK_ASSERT(w, caller->status == CLOSURE_SUSPENDED);
    CILK_ASSERT(w, caller->has_cilk_callee);
    caller->has_cilk_callee = false;
    caller->callee = NULL;
}

/* This function is used for steal, the next function for sync.
   The invariants are slightly different. */
static inline void Closure_suspend_victim(__cilkrts_worker *thief,
                                          __cilkrts_worker *victim,
                                          worker_id thief_id, worker_id victim_id,
                                          Closure *cl) {

    Closure *cl1_orig;
    Closure *cl1;

    Closure_checkmagic(thief, cl);
    // Closure_assert_ownership(thief, thief_id, cl);
    // deque_assert_ownership(deques, thief, thief_id, victim_id);

    CILK_ASSERT(thief, cl == thief->g->root_closure || cl->spawn_parent ||
                           cl->call_parent || (cl - 1)->stack_top || cl == cl->stack_top);
    // worst case, check cl - 1 is a valid address

    printf("thief   %d      suspend victim %d   closure  %p\n", thief_id, victim_id, cl);
    Closure_change_status(thief, cl, CLOSURE_RUNNING, CLOSURE_SUSPENDED);
    CILK_ASSERT(thief, unpack_closure(atomic_load_explicit(&victim->exc_closure, memory_order_seq_cst)) != cl);

    // cl1_orig = deque_xtract_bottom(deques, thief, thief_id, victim_id);
    
    // cl1 = fetch_and_update_closure(victim, (Closure *)NULL);

    // CILK_ASSERT(thief, cl1_orig == cl1);
    // CILK_ASSERT(thief, cl == cl1);
    // CILK_ASSERT(thief, cl == cl1_orig);
    USE_UNUSED(cl1);
}

static inline bool Closure_suspend_on_sync(__cilkrts_worker *const w, worker_id self,
                                   Closure *cl, __cilkrts_stack_frame** old_exc, int32_t join_counter) {

    Closure *cl1;

    cilkrts_alert(SCHED, w, "Closure_suspend %p", (void *)cl);

    Closure_checkmagic(w, cl);
    // Closure_assert_ownership(w, self, cl);
    // deque_assert_ownership(deques, w, self, self);

    CILK_ASSERT(w, cl == w->g->root_closure || cl->spawn_parent ||
                       cl->call_parent);
    CILK_ASSERT(w, cl->frame != NULL);
    CILK_ASSERT(w, __cilkrts_stolen(cl->frame));

    __uint64_t new_jc = pack_join_counter(join_counter, true);
    __uint64_t old_value = pack_join_counter(join_counter, false);

    Closure_change_status(w, cl, CLOSURE_RUNNING, CLOSURE_SYNC);
    // stodo
    if (!atomic_compare_exchange_strong(&cl->join_counter, &old_value, new_jc)) {
        // join counter changed
        Closure_change_status(w, cl, CLOSURE_SYNC, CLOSURE_RUNNING);
        return false;
    }
    printf("suspend  %d   closure  %p\n", self, cl);

     //wont work stodo
    double_ptr fetch = pack_pointers(old_exc, cl);

    // cl1_orig = deque_xtract_bottom(deques, w, self, self);
    // STODO do i need to loop
    bool success = update_closure_expected_abort(w, old_exc, (Closure *)NULL, &fetch);
    if (!success) {
        printf("closure was actually   %p    exc was   %p    worker   %d\n", unpack_closure(fetch), unpack_exc(fetch), w->self);
        CILK_ASSERT(w, false);
    }
    return true;

    // cl1 = fetch_and_update_closure(w, (Closure *)NULL);

    // CILK_ASSERT(w, cl1_orig == cl1);
    // CILK_ASSERT_POINTER_EQUAL(w, cl, cl1);

    // CILK_ASSERT(w, cl == cl1_orig);
    // USE_UNUSED(cl1);
}

static inline void Closure_make_ready(Closure *cl) { cl->status = CLOSURE_READY; }

/* ANGE: destroy the closure and internally free it (put back to global
   pool) */
static inline void Closure_destroy(struct __cilkrts_worker *const w,
                                   Closure *t) {
    CILK_ASSERT(w, t >= t->stack_top);
    cilkrts_alert(CLOSURE, w, "Deallocate closure %p", (void *)t);
    Closure_checkmagic(w, t);
    t->status = CLOSURE_RUNNING;    // reset to default state for reuse
    Closure_clean(w, t);
    if (t == t->stack_top) {
        printf("freeing pool stack     %p    era   %d     worker    %d\n", t, t->free_list_era, w->self);
        Closure_stack_free(w, t);
        // free(t);
        // w->closure_stack_head = NULL;
    } else {
        printf("non top offset      %d     closure   %p\n", t - t->stack_top, t);
        CILK_ASSERT(w, t->status == CLOSURE_RUNNING);
        // t->stack_top = NULL;
        // t->stack_offset = 0;
    }

    // cilk_internal_free(w, t, sizeof(*t), IM_CLOSURE);
}

/* Destroy the closure and internally free it (put back to global pool), after
   workers have been terminated. */
static inline void Closure_destroy_global(struct global_state *const g,
                                          Closure *t) {
    cilkrts_alert(CLOSURE, NULL, "Deallocate closure %p", (void *)t);
    t->status = CLOSURE_POST_INVALID;
    Closure_clean_root(t);

    CILK_ASSERT_G(t == t->stack_top);
    printf("freeing stack global      %p\n", t);
    free(t);
    // stodo check double free
    // dont free here because double free will occur
    

    // cilk_internal_free_global(g, t, sizeof(*t), IM_CLOSURE);
}

#endif
