#include "debug.h"
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stddef.h> 
#include <cpuid.h>
#ifdef __linux__
#include <sched.h>
#endif
#include <stdio.h>
#include <string.h>
#include <unwind.h>

#ifdef __APPLE__
#include <mach/mach_time.h>
#endif

#include "cilk-internal.h"
#include "cilk2c.h"
#include "closure.h"
#include "fiber-header.h"
#include "fiber.h"
#include "frame.h"
#include "global.h"
#include "jmpbuf.h"
#include "local-hypertable.h"
#include "local.h"
#include "readydeque.h"
#include "scheduler.h"
#include "worker_coord.h"
#include "worker_sleep.h"

// ==============================================
// Global and thread-local variables.
// ==============================================

// Boolean tracking whether the Cilk program is using an extension, e.g.,
// pedigrees.
bool __cilkrts_use_extension = false;

// Boolean tracking whether the execution is currently in a cilkified region.
bool __cilkrts_need_to_cilkify = true;

// TLS pointer to the current worker structure.
__thread __cilkrts_worker *__cilkrts_tls_worker = &default_worker;

// TLS pointer to the current fiber header.
//
// Although we could store the current fiber header in the worker, the code on
// the work needs to access the current fiber header more frequently than the
// worker itself.  Thus, it's notably faster to store a pointer to the current
// fiber header itself in TLS.
__thread struct cilk_fiber *__cilkrts_current_fh = NULL;
//__thread struct __cilkrts_stack_frame **__cilkrts_tls_shadow_stack_init = NULL;

// ==============================================
// Misc. helper functions
// ==============================================

/***********************************************************
 * Internal random number generator.
 ***********************************************************/
static void rts_srand(__cilkrts_worker *const w, unsigned int seed) {
    w->l->rand_next = seed;
}

static unsigned int update_rand_state(unsigned int state) {
    return state * 1103515245 + 12345;
}

static unsigned int get_rand(unsigned int state) {
    return state >> 16;
}

// static __cilkrts_stack_frame** get_circular_exc(__cilkrts_stack_frame **normal_exc) {

// }



static void worker_change_state(__cilkrts_worker *w,
                                enum __cilkrts_worker_state s) {
    /* TODO: Update statistics based on state change. */
    CILK_ASSERT(w, w->l->state != s);
    w->l->state = s;
#if 0
    /* This is a valuable assertion but there is no way to make it
       work.  The reducer map is not moved out of the worker until
       long after scheduler state has been entered.  */
    if (s != WORKER_RUN) {
	CILK_ASSERT(w, w->reducer_map == NULL);
    }
#endif
}

__attribute__((always_inline))
static bool reset_exception_pointer(__cilkrts_worker *const w, worker_id self,
                                    Closure *cl, __cilkrts_stack_frame **old_exc, double_ptr *old_value) {
    // Closure_assert_ownership(w, self, cl);
    CILK_ASSERT(w, (cl->frame == NULL) || (w->fiber->worker == w));
    return atomic_compare_exchange_strong_explicit(&w->exc_closure, old_value, pack_pointers(old_exc + 1, cl), memory_order_seq_cst, memory_order_relaxed);
}

/* Unused for now but may be helpful later
static void signal_immediate_exception_to_all(__cilkrts_worker *const w) {
    int i, active_size = w->g->nworkers;
    __cilkrts_worker *curr_w;

    for(i=0; i<active_size; i++) {
        curr_w = w->g->workers[i];
        curr_w->exc = EXCEPTION_INFINITY;
    }
    // make sure the exception is visible, before we continue
    Cilk_fence();
}
*/

// used by a thief getting ready to execute a stolen computation
static void setup_for_execution(__cilkrts_worker *w, Closure *t) {
    //printf("setup for execution worker  %d      closure    %p\n", w->self, t);
    cilkrts_alert(SCHED, w, "(setup_for_execution) closure %p", (void *)t);
    struct cilk_fiber *fh = w->fiber;

    CILK_ASSERT(w, fh);
    fh->worker = w;
    Closure_set_status(w, t, CLOSURE_RUNNING);

    __cilkrts_stack_frame **init = w->l->shadow_stack;
    // __cilkrts_stack_frame **old_tail = atomic_load_explicit(&w->tail, memory_order_relaxed);
    __cilkrts_stack_frame **old_exc = unpack_exc(atomic_load_explicit(&w->exc_closure, memory_order_relaxed));
    __cilkrts_stack_frame **circular_head = (((old_exc - init) & w->stack_bitmask) + init);

    ptrdiff_t exc_ind = circular_head - init;
    ptrdiff_t delta = (((-exc_ind) & w->stack_bitmask) + w->stack_bitmask + 1) & w->stack_bitmask;

    __cilkrts_stack_frame **new_init = old_exc + delta;

    CILK_ASSERT(w, ((new_init - init) & w->stack_bitmask) + init == init);
    CILK_ASSERT(w, new_init != init);

    //printf("updating exc to   %p   worker   %d\n", new_init, w->self);

    uint64_t new_tail = new_init - w->ltq_start;
    atomic_store_explicit(&w->tail, new_tail, memory_order_release); 
    atomic_store_explicit(&w->exc_closure, pack_pointers(new_init, t), memory_order_release);
    
    /* push the first frame on the current_stack_frame */
    __cilkrts_stack_frame *sf = t->frame;

    fh->current_stack_frame = sf;
    sf->fh = fh;
    __cilkrts_current_fh = fh;
}

// ANGE: When this is called, either a) a worker is about to pass a sync (though
// not on the right fiber), or b) a worker just performed a provably good steal
// successfully
// JFC: This is called from
// worker_scheduler -> ... -> Closure_return -> provably_good_steal_maybe
// user code -> __cilkrts_sync -> Cilk_sync
static void setup_for_sync(__cilkrts_worker *w, worker_id self, Closure *t) {

    // Closure_assert_ownership(w, self, t);
    // ANGE: this must be true since in case a) we would have freed it in
    // Cilk_sync, or in case b) we would have freed it when we first returned to
    // the runtime before doing the provably good steal.
    //printf("setup sync  w   %d      closure     %p   wfiber   %p    closure fiber  %p\n", w->self, t, w->fiber, t->fiber_child);
    CILK_ASSERT(w, w->fiber != t->fiber_child || t == w->g->root_closure);
    //printf("setup sync  w   %d      closure     %p\n", w->self, t);

    Closure_reset_children(w, self, t);
    Closure *to_free = NULL;

    if (w->closure_stack_head != NULL && t->stack_top != w->closure_stack_head && w->closure_stack_head != w->g->root_closure) {
        to_free = w->closure_stack_head;
    }

    // ANGE: note that in case a) this fiber won't get freed for awhile,
    // since we will longjmp back to the original function's fiber and
    // never go back to the runtime; we will only free it either once
    // when we get back to the runtime or when we encounter a case
    // where we need to.
    if (w->fiber) {
        //printf("deallocating fiber setup for worker  %d    closure   %p   fiber   %p\n", w->self, t, w->fiber);
        cilk_fiber_deallocate_to_pool(w, w->fiber);

        if (to_free) {
            // printf("setup closure stack freeing   %p   worker   %d   root closure %p\n", to_free, w->self, w->g->root_closure);
            // memlogger_logf("setup closure stack freeing   %p   free list era    %d   worker   %d\n", to_free, to_free->free_list_era, w->self);
            Closure_stack_free(w, to_free);
        }

        // if (w->closure_stack_head != NULL && t->stack_top != w->closure_stack_head && w->closure_stack_head != w->g->root_closure) {
        //     // if (w->closure_stack_head != unpack_closure(w->g->stack_free_list_head)) {
        //     //     //printf("freeing stack   %p\n", w->closure_stack_head);
        //     //     free(w->closure_stack_head);
        //     // } else {
        //     Closure *to_free = w->closure_stack_head;
        //         //printf("setup closure stack freeing   %p   worker   %d\n", w->closure_stack_head, w->self);
        //     // }
        //     // Closure_stack_free(w, w->closure_stack_head);
            

        //     Closure_clean(w, to_free);
        //     Closure_stack_free(w, to_free);
        //     // if (t == w->g->root_closure) {
        //     //     atomic_store_explicit(&w->closure_stack_tail, t + 1, memory_order_seq);
        //     // } else {
        //     //     atomic_store_explicit(&w->closure_stack_tail, t + 1, memory_order_seq);
        //     // }
            
        // }
    }
    // CILK_ASSERT(w, !to_free || t->stack_top == t->stole_top);
    w->closure_stack_head = t->stack_top;
    //printf("sync setting tail to   %p   worker   %d\n", t + 1, w->self);
    atomic_store_explicit(&w->closure_stack_tail, t + 1, memory_order_release);

    w->fiber = t->fiber_child;
    t->fiber_child = NULL; 

    // if (USE_EXTENSION) {
    //     if (t->ext_fiber)
    //         cilk_fiber_deallocate_to_pool(w, t->ext_fiber);
    //     t->ext_fiber = t->ext_fiber_child;
    //     t->ext_fiber_child = NULL;
    // }

    CILK_ASSERT(w, w->fiber);
    // __cilkrts_alert(STEAL | ALERT_FIBER, w,
    //         "(setup_for_sync) set t %p and t->fiber %p", (void *)t,
    //         (void *)t->fiber);
    __cilkrts_set_synced(t->frame);

    struct cilk_fiber *fh = w->fiber;
    __cilkrts_current_fh = fh;
    t->frame->fh = fh;
    fh->worker = w;
    CILK_ASSERT_POINTER_EQUAL(w, fh->current_stack_frame, t->frame);

    SP(t->frame) = (void *)t->orig_rsp;
    // if (USE_EXTENSION) {
    //     // Set the worker's extension (analogous to updating the worker's stack
    //     // pointer).
    //     w->extension = t->frame->extension;
    //     // Set the worker's extension stack to be the start of the saved
    //     // extension fiber.
    //     w->ext_stack = sysdep_get_stack_start(t->ext_fiber);
    // }
    t->orig_rsp = NULL; // unset once we have sync-ed

    t->left_most_fiber = NULL;
}

// ==============================================
// TLS related functions
// ==============================================

CHEETAH_INTERNAL void __cilkrts_set_tls_worker(__cilkrts_worker *w) {
    __cilkrts_tls_worker = w;
    //__cilkrts_tls_shadow_stack_init = w->l ? w->l->shadow_stack : NULL;
}

// ==============================================
// Closure return protocol related functions
// ==============================================

/* Doing an "unconditional steal" to steal back the call parent closure */
static Closure *setup_call_parent_resumption(__cilkrts_worker *const w,
                                             worker_id self,
                                             Closure *t) {
    // deque_assert_ownership(deques, w, self, self);
    // Closure_assert_ownership(w, self, t);
    //printf("resuming call parent   %p   worker    %d\n", t, w->self);

    CILK_ASSERT_POINTER_EQUAL(w, w, __cilkrts_get_tls_worker());

    Closure_change_status(w, t, CLOSURE_SUSPENDED, CLOSURE_RUNNING);

    return t;
}

void Cilk_set_return(__cilkrts_worker *const w) {
    Closure *t;

    cilkrts_alert(RETURN, w, "(Cilk_set_return)");
    worker_id self = w->self;

    // deque_lock_self(deques, self);
    // t_orig = deque_peek_bottom(deques, w, self, self);
    // could have been stolen from
    double_ptr fetch = atomic_load_explicit(&w->exc_closure, memory_order_relaxed);
    t = unpack_closure(fetch);
    __cilkrts_stack_frame** old_exc = unpack_exc(fetch);
    //printf("returning");
    // CILK_ASSERT(w, t_orig == t);
    // Closure_lock(w, self, t);

    // printf("starting w   %d  set_return     %p     cilk callee    %d     join counter    %d\n", w->self, t, t->has_cilk_callee, get_join_counter));

    CILK_ASSERT(w, t->status == CLOSURE_RUNNING);
    CILK_ASSERT(w, Closure_has_children(t) == 0);

    // all hyperobjects from child or right sibling must have been reduced
    CILK_ASSERT(w, t->child_ht == (hyper_table *)NULL &&
                       t->right_ht == (hyper_table *)NULL);
    
    CILK_ASSERT(w, t->call_parent);
    CILK_ASSERT(w, t->spawn_parent == NULL || t == t->stack_top);    // we may have set it optimistically for a future steal
    CILK_ASSERT(w, (t->frame->flags & CILK_FRAME_DETACHED) == 0);

    Closure *call_parent = t->call_parent;

    double_ptr new_value = pack_pointers(old_exc, call_parent);
    atomic_store_explicit(&w->exc_closure, new_value, memory_order_release);

    // bool success = expected_abort(w, old_exc, call_parent, &fetch);
    // if (!success) {
    //     //printf("closure was actually   %p    exc was   %p    worker   %d\n", unpack_closure(fetch), unpack_exc(fetch), w->self);
    //     CILK_ASSERT(w, false);
    // }

    // USE_UNUSED(t1);
    // CILK_ASSERT(w, t == t1);
    CILK_ASSERT(w, __cilkrts_stolen(t->frame));

    // deque_add_bottom(deques, w, call_parent, self, self);

    t->frame = NULL;
    // Closure_unlock(w, self, t);

    // Closure_lock(w, self, call_parent);
    CILK_ASSERT(w, call_parent->call_parent_fiber == w->fiber);
    // t->fiber = NULL;
    // if (USE_EXTENSION) {
    //     CILK_ASSERT(w, call_parent->ext_fiber == t->ext_fiber);
    //     t->ext_fiber = NULL;
    // }
    t->call_parent = NULL;
    Closure_remove_callee(w, call_parent);
    setup_call_parent_resumption(w, self, call_parent);
    CILK_ASSERT_POINTER_EQUAL(w, old_exc, w->ltq_start + w->tail);
    // Closure_unlock(w, self, call_parent);

    // deque_unlock_self(deques, self);

    //printf("w   %d  set_return      closure   %p\n", w->self, t);
    // this must be the cleanup of a spawn parent
    // CILK_ASSERT(w, call_parent->stack_top != t);
    // Closure_destroy(w, t);
    Closure_clean(w, t);
}

static Closure *provably_good_steal_maybe(__cilkrts_worker *const w,
                                          worker_id self, Closure *parent, int join_counter, bool hit_sync) {

    // Closure_assert_ownership(w, self, parent);
    local_state *l = w->l;
    // cilkrts_alert(STEAL, w, "(provably_good_steal_maybe) cl %p",
    //               (void *)parent);
    CILK_ASSERT(w, !l->provably_good_steal);
    //printf("try provably good steal  parent   %p   children     %d   callee  %p    worker   %d    parent status   %s\n", parent, atomic_load_explicit(&parent->join_counter, memory_order_seq_), parent->has_cilk_callee, w->self, Closure_status_to_str(parent->status));
    // int32_t join_counter;
    // bool hit_sync;
    // unpack_join_counter(atomic_load_explicit(&parent->join_counter, memory_order_seq_), &join_counter, &hit_sync);

    if (!parent->has_cilk_callee && join_counter == 0 && hit_sync) {
        // cilkrts_alert(STEAL | ALERT_SYNC, w,
        //      "(provably_good_steal_maybe) completing a sync");

        CILK_ASSERT(w, parent->frame != NULL);

        /* do a provably-good steal; this is *really* simple */
        l->provably_good_steal = true;

        setup_for_sync(w, self, parent);
        // CILK_ASSERT(w, parent->owner_ready_deque == NO_WORKER);
        // Closure_make_ready(parent);
        atomic_store_explicit(&parent->join_counter, 0, memory_order_release);
        Closure_change_status(w, parent, CLOSURE_SYNC, CLOSURE_RUNNING);

        //printf("w   %d  (provably_good_steal_maybe) returned %p\n", w->self, parent);

        cilkrts_alert(STEAL | ALERT_SYNC, w,
                      "(provably_good_steal_maybe) returned %p",
                      (void *)parent);

        return parent;
    }

    return NULL;
}
static void reduce_siblings_maybe(__cilkrts_worker *const w, Closure *child, Closure *parent, bool is_left_most) {
    // Get the current active hypermap.
    // printf("reduce siblings   child   %p    parent    %p   worker   %d   leftmost   %d\n", child, parent, w->self, is_left_most);
    hyper_table *active_ht;
    if (child->right_ht != NULL) {
        active_ht =  merge_two_hts(w, child->right_ht, w->hyper_table);
    } else {
        active_ht = w->hyper_table;
    }

    w->hyper_table = NULL;
    child->right_ht = NULL;
    Closure *orig_child = child;

    while (true) {
        // printf("merging tables for child closure   %p    worker    %d\n", child, w->self);

        // Get the "left" hypermap, which either belongs to a left sibling, if
        // it exists, or the parent, otherwise.
        hyper_table *lht;
        Closure *left_sib = child->left_sib;
        // if (child != orig_child) {
        //     child->left_sib = NULL;
        // }
        
        // a return should only be executed on a closure once. this is the only 
        // time that this value would be written to
        // once a closure has been marked for removal, it is safe to do reads of its child->right_ht
        // CILK_ASSERT(w, child->right_ht == NULL); 

        if (left_sib == NULL) {
            // printf("no left sib   child   %p   left sib   %p\n", child);
            // deposit the active hash table into our right_ht for our parent or a right sibling to reduce with later
            // because sibling pointers havent been initialized yet
            //printf("found no left siblings of child    %p     worker    %d\n", child, w->self);
            // printf("no left sib depositing tables of child    %p    worker    %d\n", orig_child, w->self);
            if (child != orig_child) {
                // memlogger_logf("1 reduce closure stack freeing   %p   free list era    %d   worker   %d\n", child, child->free_list_era, w->self);
                Closure_destroy(w, child);
                if (child == orig_child->left_sib) {
                    orig_child->left_sib = left_sib;
                }
            }
            orig_child->right_ht = active_ht;
            return;
        } 

        // printf("has left sib   child   %p   left sib   %p\n", child, left_sib);
        uintptr_t left_right_sib = atomic_load_explicit(&left_sib->right_sib_removed, memory_order_acquire);
        if (!Closure_is_removed(w, w->self, left_right_sib)) {
            // if it hasn't been removed yet, we cant reduce with it or any further left siblings
            // CILK_ASSERT(w, left_sib->right_ht == NULL);
            uintptr_t new_right_sib = (uintptr_t)orig_child & ~1ULL; // dont set next of a removed child

            if (atomic_compare_exchange_strong_explicit(&left_sib->right_sib_removed, &left_right_sib, new_right_sib, memory_order_release, memory_order_relaxed) || left_right_sib == new_right_sib) {
                // Closure *cur_child = orig_child->left_sib;
                // while (cur_child != left_sib) {
                //     Closure *next_child = cur_child->left_sib;
                //     Closure_stack_free(w, cur_child);
                //     cur_child = next_child;
                // }
                if (child != orig_child) {
                    // memlogger_logf("2 reduce closure stack freeing   %p   free list era    %d   worker   %d\n", child, child->free_list_era, w->self);
                    Closure_destroy(w, child);
                }

                orig_child->left_sib = left_sib;

                // deposit our view for right children /parent to reduce
                // printf("left sib depositing tables of child    %p    worker    %d\n", orig_child, w->self);
                CILK_ASSERT(w, orig_child->right_ht == NULL);
                orig_child->right_ht = active_ht;

                //printf("depositing tables of child    %p    worker    %d\n", orig_child, w->self);

                return;
            } else {
                // printf("retry merge child   %p\n", child);
                continue;
            }
        } else {
            lht = left_sib->right_ht;

            if (!atomic_compare_exchange_strong_explicit(&left_sib->right_ht, &lht, NULL, memory_order_release, memory_order_relaxed)) {
                // n no one else should be touchhing this hypertable while were not removed
                CILK_ASSERT(w, false);
                // orig_child->right_ht = active_ht;
            }

            // If we have no hypermaps on the left, there may still be a left sibling of our left sibling that needs to be reduced.
            if (lht == NULL) {
                /* move to next child */
                //printf("moving to next left sib   %p    of child    %p\n", child->left_sib, child);
                CILK_ASSERT(w, child->right_ht == NULL);
                if (child != orig_child) {
                    // memlogger_logf("3 reduce closure stack freeing   %p   free list era    %d   worker   %d\n", child, child->free_list_era, w->self);
                    Closure_destroy(w, child);
                    if (child == orig_child->left_sib) {
                        orig_child->left_sib = left_sib;
                    }
                }
                
                child = left_sib;
                continue;
            } else {
                // merge reducers
                if (is_left_most) {
                    CILK_ASSERT(w, false);
                }
                //printf("merged hts   %p     worker    %d\n", lht, w->self);
                active_ht = merge_two_hts(w, lht, active_ht);
                if (child != orig_child) {
                    // memlogger_logf("4 reduce closure stack freeing   %p   free list era    %d   worker   %d\n", child, child->free_list_era, w->self);
                    Closure_destroy(w, child);
                    if (child == orig_child->left_sib) {
                        orig_child->left_sib = left_sib;
                    }
                }
                child = left_sib;
                continue;
            }
            //printf("acquired left sib   %p  for   child   %p   worker    %d\n", left_sib, child, w->self);
        }
    }
}

/***
 * Return protocol for a spawned child.
 *
 * Some notes on reducer implementation (which was taken out):
 *
 * If any reducer is accessed by the child closure, we need to reduce the
 * reducer view with the child's right_ht, and its left sibling's
 * right_ht (or parent's child_ht if it's the left most child)
 * before we unlink the child from its sibling closure list.
 *
 * When we modify the sibling links (left_sib / right_sib), we always lock
 * the parent and the child.  When we retrieve the reducer maps from left
 * sibling or parent from their place holders (right_ht / child_ht),
 * we always lock the closure from whom we are getting the maps from.
 * The locking order is always parent first then child, right child first,
 * then left.
 *
 * Once we have done the reduce operation, we try to deposit reducers
 * from the child to either its left sibling's right_ht or parent's
 * child_ht.  Note that even though we have performed the reduce, by
 * the time we deposit the views, the child's left sibling may have
 * changed, or child may become the new left most child.  Similarly,
 * the child's right_ht may have something new again.  If that's the
 * case, we need to do the reduce again.
 *
 * This function returns a closure to be executed next, or NULL if none.
 * The child must not be locked by ourselves, and be in no deque.
 ***/
static Closure *Closure_return(__cilkrts_worker *const w, worker_id self,
                               Closure *child) {

    Closure *res = (Closure *)NULL;
    Closure *parent = child->spawn_parent;
    bool is_left_most = false;

    if (parent == (Closure *)NULL) {
        // is left most
        parent = child - 1;
    }
    
    if (parent == child - 1) {
        is_left_most = true;
    }

    CILK_ASSERT(w, parent);

    // printf("closure_return  w   %d      parent  %p      child   %p    sf    %p    parent status   %s   is_left_most   %d    child status   %s   child top     %p    my top    %p\n", w->self, parent, child, child->frame, Closure_status_to_str(parent->status), is_left_most, Closure_status_to_str(child->status), child->stack_top, w->closure_stack_head);
    CILK_ASSERT(w, child);
    // STODO, jc >0 means someone attempting to steal?
    // CILK_ASSERT(w, get_join_counter(atomic_load_explicit(&child->join_counter,)) == 0);
    CILK_ASSERT(w, child->status == CLOSURE_RETURNING);
    // CILK_ASSERT(w, child->owner_ready_deque == NO_WORKER);
    // Closure_assert_alienation(w, self, child);

    CILK_ASSERT(w, child->has_cilk_callee == 0);
    CILK_ASSERT(w, child->call_parent == NULL);

    CILK_ASSERT(w, parent != NULL);

    cilkrts_alert(RETURN, w, "(Closure_return) child %p, parent %p",
                  (void *)child, (void *)parent);

    /* The frame should have passed a sync successfully meaning it
       has not accumulated any maps from its children and the
       active map is in the worker rather than the closure. */
    CILK_ASSERT(w, !child->child_ht && !child->user_ht);

    /* If in the future the worker's map is not created lazily,
       assert it is not null here. */

    /* need a loop as multiple siblings can return while we
       are performing reductions */

    // always lock from top to bottom
    // Closure_lock(w, self, parent);
    // Closure_lock(w, self, child);

    // Deal with reducers.
    
    // if there are any siblings to the left of us that are marked for removal, reduce with their hashtables,
    // clean up those sibling closure stacks if possible
    // child->right_ht = w->hyper_table;
    // w->hyper_table = NULL;
    if (child->left_sib == NULL) {
        // doesn't need to be modified atomically because it won't be read until the parent passes a sync which can
        // only happen after this child decrements the join counter

        // the left-most child is the only one that can deposit views into the parent. this remains the same child
        // until the parent passes the sync
        if (is_left_most) {
            CILK_ASSERT(w, parent->child_ht == NULL);
            parent->child_ht = w->hyper_table;
        } else {
            hyper_table *right_ht = atomic_load_explicit(&child->right_ht, memory_order_acquire);
            hyper_table *new_ht = w->hyper_table;
            do {
                if (right_ht != NULL) {
                    new_ht = merge_two_hts(w, right_ht, w->hyper_table);
                }

            } while (!atomic_compare_exchange_weak_explicit(&child->right_ht, &right_ht, new_ht, memory_order_release, memory_order_acquire));
        }
        w->hyper_table = NULL;
    } else {
        reduce_siblings_maybe(w, child, parent, is_left_most);
    }

    /* The returning closure and its parent are locked. */

    // Execute left-holder logic for stacks.
    if (is_left_most) {
        // We are leftmost, pass stack/fiber up to parent.
        // Thus, no stack/fiber to free.
        CILK_ASSERT(w, w->fiber);
        //printf("parent frame  %p    wframe  %p\n", parent->frame, w->fiber->current_stack_frame);
        CILK_ASSERT(
            w, parent->frame == NULL || parent->frame == w->fiber->current_stack_frame);

        //printf("parent was ready    worker  %d    parent   %p    child   %p\n", w->self, parent, child);
        if (parent->left_most_fiber != NULL) {
            CILK_ASSERT(w, parent->left_most_fiber == w->fiber || parent == w->g->root_closure);
        }
        parent->fiber_child = w->fiber;
        CILK_ASSERT(w, w->fiber);
        
        // if (USE_EXTENSION) {
        //     parent->ext_fiber_child = child->ext_fiber;
        // }
    } else {
        // child->left_sib || parent->fiber_child. might not have left sib
        // Case where we are not the leftmost stack.
        //printf("deallocating fiber return for worker  %d    child   %p     left sib    %p    wfiber  %p      parent fiber   %p\n", w->self, child, child->left_sib, w->fiber, parent->fiber_child);
        CILK_ASSERT(w, !is_left_most);
        CILK_ASSERT(w, parent->fiber_child != w->fiber || parent == w->g->root_closure);
        
        cilk_fiber_deallocate_to_pool(w, w->fiber);
        // if (USE_EXTENSION && child->ext_fiber) {
        //     cilk_fiber_deallocate_to_pool(w, child->ext_fiber);
        // }
    }

    w->fiber = NULL;
    w->ext_fiber = NULL;

    // Propagate whether the parent needs to handle an exception.  We could
    // check the hypermap for an exception reducer, but using a separate boolean
    // avoids the expense of a table lookup.
    if (child->exception_pending) {
        parent->exception_pending = true;
        parent->frame->flags |= CILK_FRAME_EXCEPTION_PENDING;
    }

    // Closure_remove_child(w, self, parent, child); // unlink child from tree
    
    // we have deposited our views and unlinked; we can quit now
    // invariant: we can only decide to quit when we see no more maps
    // from the right, we have deposited our own views, and unlink from
    // the tree.  All these are done while holding lock on the parent.
    // Before, another worker could deposit more views into our
    // right_ht slot after we decide to quit, but now this cannot
    // occur as the worker depositing the views to our right_ht also
    // must hold lock on the parent to do so.
    // Closure_unlock(w, self, child);
    /*    Closure_unlock(w, parent);*/

    //printf("w    %d     return      %p\n", w->self, child);
    // Closure_destroy(w, child);

    /*    Closure_lock(w, parent);*/

    CILK_ASSERT(w, parent->status != CLOSURE_RETURNING);
    CILK_ASSERT(w, parent->frame != NULL || parent->status != CLOSURE_READY);
    // CILK_ASSERT(w, parent->frame->magic == CILK_STACKFRAME_MAGIC);

    int new_jc;
    bool did_hit_sync;

    // CILK_ASSERT(w, join_counter >= 0);
    Closure_mark_child_remove(w, self, child, parent);
    decrement_join_counter_and_fetch(w, parent, &new_jc, &did_hit_sync);

    res = provably_good_steal_maybe(w, self, parent, new_jc, did_hit_sync);

    if (res) {
        hyper_table *child_ht = parent->child_ht;
        hyper_table *active_ht = parent->user_ht;
        parent->child_ht = NULL;
        parent->user_ht = NULL;
        // printf("provably good steal   child   %p   parent  %p  worker   %d    left most   %d\n", child, parent, w->self, is_left_most);
        CILK_ASSERT(w, w->hyper_table == NULL);
        if (child_ht != NULL) {
            w->hyper_table = merge_two_hts(w, child_ht, active_ht);
        } else {
            w->hyper_table = active_ht;
        }
        
        setup_for_execution(w, res);
    } else {
        w->closure_stack_head = NULL;
        atomic_store_explicit(&w->closure_stack_tail, NULL, memory_order_relaxed);
        // if (child->stack_top != w->g->root_closure) {
        //     w->closure_stack_head = NULL;
        //     atomic_store_explicit(&w->closure_stack_tail, NULL, memory_order_seq_);
        //     //printf("return resetting tail   worker   %d\n", w->self);
        // } else {
        //     //printf("didnt reset tail   worker   %d\n", w->self);
        // }
    }

    // Closure_unlock(w, self, parent);


    return res;
}

/*
 * ANGE: t is returning; call the return protocol; see comments above
 * Closure_return.  res is either the next closure to execute
 * (provably-good-steal the parent closure), or NULL if nothing should be
 * executed next.
 *
 * Only called from do_what_it_says when the closure->status =
 * CLOSURE_RETURNING
 */
static Closure *return_value(__cilkrts_worker *const w, worker_id self,
                             Closure *t) {
    cilkrts_alert(RETURN, w, "(return_value) closure %p", (void *)t);

    Closure *res = NULL;
    CILK_ASSERT(w, t->status == CLOSURE_RETURNING);
    CILK_ASSERT(w, t->call_parent == NULL);

    if (t->call_parent == NULL) {
        res = Closure_return(w, self, t);
    } /* else {
      // ANGE: the ONLY way a closure with call parent can reach here
      // is when the user program calls Cilk_exit, leading to global abort
      // Not supported at the moment
    } */

    cilkrts_alert(RETURN, w, "(return_value) returning closure %p", (void *)t);

    return res;
}

/*
 * This is called from the user code (in cilk2c_inlined) if E >= T.  Two
 * possibilities:
 *   1. Someone stole the last frame from this worker, hence E >= T when child
 *   returns.
 *   2. Someone invokes signal_immediate_exception with the closure currently
 *   running on the worker's deque.  This is only possible with abort.
 */
void Cilk_exception_handler(__cilkrts_worker *w, char *exn, __cilkrts_stack_frame **head, __cilkrts_stack_frame **tail, Closure *t) {

    // Closure *t_orig;
    // Closure *t;
    worker_id self = w->self;

    // deque_lock_self(deques, self);
    // t_orig = deque_peek_bottom(deques, w, self, self);
    // t = unpack_closure(atomic_load_explicit(&w->exc_closure, memory_order_acquir));
    // CILK_ASSERT_POINTER_EQUAL(w, t_orig, t);

    
    // Closure_lock(w, self, t);

    cilkrts_alert(EXCEPT, w, "(Cilk_exception_handler) closure %p!", (void *)t);
    // //printf("exception_handler   t    %p     status     %s   worker  %d      head    %p      tail    %p\n", t, Closure_status_to_str(t->status), w->self, old_exc, tail);
    CILK_ASSERT(w, t);

    /* Reset the E pointer. */

    /* These will not change while the deque is locked. */
    // we failed the cas so we need to get our new closure
    // double_ptr exc_closure = atomic_load_explicit(&w->exc_closure, memory_order_acquir);
    // t = unpack_closure(old_value);
    // __cilkrts_stack_frame **head = unpack_exc(old_value);

    CILK_ASSERT(w, t->status == CLOSURE_RUNNING ||
                       // for during abort process
                       t->status == CLOSURE_RETURNING);

    // if (head > tail) {
        // deque is empty
        cilkrts_alert(EXCEPT, w, "(Cilk_exception_handler) this is a steal!");
        // if (NULL != exn) {
        //     // The spawned child is throwing an exception.  Save that exception
        //     // object for later processing.
        //     struct closure_exception *exn_r =
        //         (struct closure_exception *)internal_reducer_lookup(
        //             w, &exception_reducer, sizeof(exception_reducer),
        //             init_exception_reducer, reduce_exception_reducer);
        //     exn_r->exn = exn;
        //     t->exception_pending = true;
        // }

        CILK_ASSERT(w, Closure_has_children(t) == 0);
        Closure_set_status(w, t, CLOSURE_RETURNING);

        w->l->returning = true;
        CILK_ASSERT(w, head == tail + 1);
        CILK_ASSERT(w, w->return_closure == NULL);
        w->return_closure = t;

        atomic_store_explicit(&w->exc_closure, pack_pointers(head, (Closure *)NULL), memory_order_relaxed);
        atomic_store_explicit(&w->tail, (tail - w->ltq_start) + 1, memory_order_relaxed);

        // Closure_unlock(w, self, t);

        longjmp_to_runtime(w); // NOT returning back to user code

    // } else { // not steal, not abort; false alarm
    //     CILK_ASSERT(w, false);
    //     // Closure_unlock(w, self, t);
    //     // deque_unlock_self(deques, self);

    //     return;
    // }
}

// ==============================================
// Steal related functions
// ==============================================

static inline bool trivial_stacklet(const __cilkrts_stack_frame *head) {
    assert(head);

    bool is_trivial = (head->flags & CILK_FRAME_DETACHED);

    return is_trivial;
}

/*
 * Do the thief part of Dekker's protocol.  Return the head pointer upon
 * success, NULL otherwise.  The protocol fails when the victim already popped T
 * so that E=T.
 */
static __cilkrts_stack_frame **do_dekker_on(__cilkrts_worker *const w,
                                            worker_id self,
                                            __cilkrts_worker *const victim_w,
                                            Closure *cl) {

    /* Force a global order between the increment of exc above and any
       decrement of tail by the victim.  __cilkrts_leave_frame must also
       have a SEQ_CST fence or atomic.  Additionally the increment of
       tail in compiled code has release semantics and needs to be paired
       with an acquire load unless there is an intervening fence. */
    

    /*
     * The thief won't steal from this victim if there is only one frame on cl's
     * stack
     */

    //  double_ptr exc_closure = atomic_load_explicit(&victim_w->exc_closure, memory_order_acquire);
    // __cilkrts_stack_frame **head = unpack_exc(exc_closure);
    // Closure *cur_closure = unpack_closure(exc_closure);

    // atomic_thread_fence(memory_order_seq_cst);

    // __cilkrts_stack_frame **tail =
    //     atomic_load_explicit(&victim_w->tail, memory_order_acquire);

    double_ptr exc_closure = atomic_load_explicit(&victim_w->exc_closure, memory_order_seq_cst);
    __cilkrts_stack_frame **head = unpack_exc(exc_closure);
    Closure *cur_closure = unpack_closure(exc_closure);

    uint64_t tail_idx =
        atomic_load_explicit(&victim_w->tail, memory_order_seq_cst);
    __cilkrts_stack_frame **tail = victim_w->ltq_start + tail_idx;
    if (head >= tail || cur_closure != cl || cur_closure->status != CLOSURE_RUNNING) {
        //printf("w   %d      steal failed    head    %p     cur_closure     %p    victim     %d\n", w->self, head, cur_closure, victim_w->self);
        return NULL;
    }

    return head;
}

/***
 * Promote the child frame of parent to a full closure.
 * Detach the parent and return it.
 *
 * Assumptions: the parent is running on victim, and we own
 * the locks of both parent and deque[victim].
 * The child keeps running on the same cache of the parent.
 * The parent's join counter is incremented.
 *
 * In order to promote a child frame to a closure,
 * the parent's frame must be the last in its ready queue.
 *
 * Returns the child.
 *
 * ANGE: I don't think this function actually detach the parent.  Someone
 *       calling this function has to do deque_xtract_top on the victim's
 *       deque to get the parent closure.  This is the only time I can
 *       think of, where the ready deque contains more than one frame.
 ***/
static Closure *promote_child(__cilkrts_stack_frame **head,
                              __cilkrts_worker *const w,
                              __cilkrts_worker *const victim_w, Closure *cl,
                              Closure **res, worker_id self, worker_id pn, bool *is_left_most,
                              __cilkrts_stack_frame *cl_frame, __cilkrts_stack_frame *frame_to_steal) {
    // deque_assert_ownership(deques, w, self, pn);
    // Closure_assert_ownership(w, self, cl);

    // __cilkrts_stack_frame **still_active = do_dekker_on(w, self, victim_w, cl);
    // if (!still_active) {
    //     *res = (Closure *)NULL;
    //     return (Closure *)NULL;
    // }

    Closure *steal_tail;
    Closure *spawn_child;
    Closure *stole_top = victim_w->closure_stack_head;
    
    if (cl >= stole_top && cl < stole_top + w->stack_bitmask + 1) {
        steal_tail = cl + 1;
        *is_left_most = true;
        //printf("same stack  w   %d    cl    %p\n", w->self, cl);
    } else {
        steal_tail = atomic_load_explicit(&victim_w->closure_stack_tail, memory_order_acquire);
        //printf("fresh stack w   %d    cl    %p    steal tail   %p\n", w->self, cl, steal_tail);
    }

    if (cl_frame == frame_to_steal || trivial_stacklet(frame_to_steal)) {   // stolen before / spawning expression
        spawn_child = steal_tail;
        *res = cl;
    } else {
        *res = steal_tail;
        spawn_child = steal_tail + 1;
    }

    // ANGE: This must be true if we get this far.
    // Note that it can be that H == T here; victim could have done T-- after
    // the thief passes Dekker, in which case, thief gets the last frame, and H
    // == T.  Victim won't be able to proceed further until the thief finishes
    // stealing, releasing the deque lock; at which point, the victim will
    // realize that it should return back to runtime.
    //
    // These assertions are commented out because they can impact performance
    // noticeably by introducing contention.
    /* CILK_ASSERT(w, head <= victim_w->exc); */
    /* CILK_ASSERT(w, head <= victim_w->tail); */

    CILK_ASSERT(w, frame_to_steal != NULL);
    //printf("w       %d      victim      %d      steal tail      %p      child tail     %p    cl      %p    frame    %p\n", w->self, victim_w->self, *res, spawn_child, cl, frame_to_steal);
    
    return spawn_child;

    // ANGE: if cl's frame is set AND equal to the frame at *HEAD, cl must be
    // either the root frame or have been stolen before.  On the other hand, if
    // cl's frame is not set, the top stacklet may contain one frame (the
    // detached spawn helper resulted from spawning an expression) or more than
    // one frame, where the right-most (oldest) frame is a spawn helper that
    // called a Cilk function (regular cilk_spawn of function).
    // if (cl->frame == frame_to_steal) { // stolen before
    //     //printf("stolen before   w   %d  victim  %d  closure     %p\n", w->self, victim_w->self, cl);
    //     CILK_ASSERT(w, __cilkrts_stolen(frame_to_steal));
    //     spawn_parent = cl;
    //     child_tail = steal_tail;
    //     *res = cl;
    // } else if (trivial_stacklet(frame_to_steal)) { // spawning expression
    //     //printf("spawning expression   w   %d  victim  %d  closure     %p\n", w->self, victim_w->self, cl);
    //     CILK_ASSERT(w, __cilkrts_not_stolen(frame_to_steal));
    //     CILK_ASSERT(w, frame_to_steal->call_parent &&
    //                        __cilkrts_stolen(frame_to_steal->call_parent));
    //     CILK_ASSERT(w, (frame_to_steal->flags & CILK_FRAME_LAST) == 0);
    //     // __cilkrts_set_stolen(frame_to_steal);
    //     // move set stolen to CAS
    //     // move set frame
    //     // Closure_set_frame(w, cl, frame_to_steal);
    //     spawn_parent = cl;
    //     child_tail = steal_tail;
    //     *res = cl;
    // } else { // spawning a function and stacklet never gotten stolen before
    //     // cl->frame could either be NULL or some older frame (e.g.,
    //     // cl->frame was stolen and resumed, it calls another frame which
    //     // spawned, and the spawned frame is the frame_to_steal now). ANGE:
    //     // if this is the case, we must create a new Closure representing
    //     // the left-most frame (the one to be stolen and resume).
    //     //printf("1 w     %p    victim    %p   new_closure_top  %p   new_closure    %p   offset %d\n", w->closure_stack_head, victim_w->closure_stack_head, (cl + 1)->stack_top, cl + 1, (cl + 1)->stack_offset);
    //     //printf("never stolen   w   %d  victim  %d  closure     %p\n", w->self, victim_w->self, cl);
    //     spawn_parent = steal_tail;
    //     // spawn_parent = Closure_create(w, victim_w, steal_tail, frame_to_steal);
    //     // __cilkrts_set_stolen(frame_to_steal);
    //     // move set stolen to CAS
    //     // Closure_set_status(w, spawn_parent, CLOSURE_RUNNING);

    //     // At this point, spawn_parent is a new Closure formally associated with
    //     // the stolen frame, frame_to_steal, meaning that spawn_parent->frame is
    //     // set to point to frame_to_steal.  The remainder of this function will
    //     // insert spawn_parent as a callee of cl and create a new Closure,
    //     // spawn_child, for the spawned child computation.  The spawn_child
    //     // Closure is nominally associated with the child of the stolen frame,
    //     // but the Closure's frame pointer is not set.
    //     //
    //     // Pictorially, this code path organizes the Closures and stack frames
    //     // as follows, where cl->frame may or may not be set to point to a stack
    //     // frame, as denoted by the dashed arrow.
    //     //
    //     //     *Closures*           *Stack frames*
    //     //     +----+
    //     //     | cl | - - - - - - > (called frame or spawn helper)
    //     //     +----+                     ^
    //     //       ^                  ...   |
    //     //       |                  (zero or more called frames)
    //     //       v                  ...   ^
    //     //     +--------------+           |
    //     //     | spawn_parent | --> (frame_to_steal)
    //     //     +--------------+           ^
    //     //       ^                        |
    //     //       |                        |
    //     //       v                        |
    //     //     +-------------+            |
    //     //     | spawn_child |      (spawn helper)
    //     //     +-------------+
    //     //
    //     // There is no need to promote any of the called frames between the
    //     // frame_to_steal and the frame (nominally) associated with cl.  All of
    //     // those frames are called frames.  When frame_to_steal returns,
    //     // spawn_parent will be popped, returning the closure tree to a familiar
    //     // state: cl will be (nominally) associated with a frame that has called
    //     // frames below it, and a worker will be working on the bottommost of
    //     // those called frames.

    //     // Closure_add_callee(w, cl, spawn_parent);
    //     // spawn_parent->call_parent = cl;
    //     // move to after CAS

    //     // suspend cl & remove it from deque
    //     // Closure_suspend_victim(deques, w, victim_w, self, pn, cl);
    //     // move suspend to after

    //     // Closure_lock(w, self, spawn_parent);
    //     *res = spawn_parent;
    //     child_tail = steal_tail + 1;
    // }

    // if (!do_dekker_on(w, self, victim_w, cl)) {
    //     *res = (Closure *)NULL;
    //     return (Closure *)NULL;
    // }

    // // stodo do we need join counter anymore
    // increment_join_counter_and_fetch(w, spawn_parent);

    

    // CILK_ASSERT(w, spawn_parent->has_cilk_callee == 0);
    
    // Closure *spawn_child = child_tail;
    // move update tail abort to after CAS


    /***
     * Register this child, which sets up its sibling links.
     * We do this here instead of in finish_promote, because we must setup
     * the sib links for the new child before its pointer escapses.
     ***/
    // move add child
    // Closure_add_child(w, self, spawn_parent, spawn_child);


    // move add head pointer

    // if (*res == (Closure *)NULL) {
    //     // deque_xtract_top(deques, w, self, pn);
    //     // *res = unpack_closure(atomic_load_explicit(&victim_w->exc_closure, memory_order_seq_cs));
    //     // res may have alreayd been updated, cant use it
    //     *res = cl;
    //     // might want to abort STODO
    //     // CILK_ASSERT_POINTER_EQUAL(w, cl, *res);
    //     // might be this or next
    // }

    /* at this point the child can be freely executed */
    // return spawn_child;
}

/***
 * Finishes the promotion process.  The child is already fully promoted
 * and requires no more work (we only use the given pointer to identify
 * the child).  This function does some more work on the parent to make
 * the promotion complete.
 *
 * ANGE: This includes promoting everything along the stolen stacklet
 * into full closures.
 ***/
static void finish_promote(__cilkrts_worker *const w, worker_id self,
                           __cilkrts_worker *const victim_w, Closure *parent,
                           bool has_frames_to_promote) {

    // Closure_assert_ownership(w, self, parent);
    CILK_ASSERT(w, parent->has_cilk_callee == 0);
    CILK_ASSERT(w, __cilkrts_stolen(parent->frame));

    __cilkrts_set_unsynced(parent->frame);
    // move unsynched
    /* Make the parent ready */
    Closure_make_ready(parent);

    return;
}

/***
 * ANGE: This function promotes all frames in the top-most stacklet into
 * its own closures and also creates a new child closure to leave it with
 * the victim.  Normally this is invoked by Closure_steal, but a worker
 * may also invoke Closure steal on itself (for the purpose of race detecting
 * Cilk code with reducers).  Thus, this function is written to check for
 * that --- if w == victim_w, we don't actually create a new fiber for
 * the stolen parent.
 *
 * NOTE: this function assumes that w holds the lock on victim_w's deque
 * and Closure cl and releases them before returning.
 ***/
static Closure *extract_top_spawning_closure(__cilkrts_stack_frame **head,
                                             __cilkrts_worker *const w,
                                             __cilkrts_worker *const victim_w,
                                             Closure *cl, worker_id self,
                                             worker_id victim_id,
                                             __cilkrts_stack_frame *cl_frame) {
    Closure *res = NULL, *child;
    

    //printf("extracting cl   %p      stack top    %p     worker  %d      victim  %d      tail    %p      old_exc    %p\n", cl, cl->stack_top, self, victim_id, atomic_load_explicit(&victim_w->tail, ), head);

    // deque_assert_ownership(deques, w, self, victim_id);
    // Closure_assert_ownership(w, self, cl);

    /*
     * if dekker passes, promote the child to a full closure,
     * and steal the parent
     */

    __cilkrts_stack_frame **init = victim_w->l->shadow_stack;
    __cilkrts_stack_frame **circular_head = (((head - init) & w->stack_bitmask) + init);
    __cilkrts_stack_frame *frame_to_steal = *circular_head;
    bool is_left_most = false;
    child = promote_child(head, w, victim_w, cl, &res, self, victim_id, &is_left_most, cl_frame, frame_to_steal);
    

    // we got beat by another theif
    if (res == (Closure *)NULL) {
        return res;
    }

    // attempt to commit the steal by incrementing the exception pointer of the victim
    double_ptr old_value = pack_pointers(head, cl);
    double_ptr new_value = pack_pointers(head + 1, child);
    if (!atomic_compare_exchange_weak_explicit(&victim_w->exc_closure, &old_value, new_value, memory_order_seq_cst, memory_order_relaxed)) {
        // we might have suspended the victim and the victim returned but we didnt succeed in steal
        //printf("w       %d      aborted at end      victim      %d   expect_exc     %p   victim_exc  %p      victim_closure  %p\n", w->self, victim_w->self, head, unpack_exc(old_value), unpack_closure(old_value));
        return (Closure *)NULL;
        
    } else {
        // "cl    %p     stack top    %p    new era   %d    old era   %d     victim     %d    head  %p\n", w->self, cl, cl->stack_top, cl->stack_top->free_list_era, era, victim_w->self, head
        // memlogger_logf("succeeded steal     w   %d      victim    %d    cl  %p     cl stack top    %p     cl era    %d    res    %p    new victim closure  %p   head   %p\n", w->self, victim_id, cl, cl->stack_top,cl->stack_top->free_list_era, res, unpack_closure(new_value), head + 1);
        // printf("succeeded steal     w   %d      victim    %d    cl  %p     res    %p    new victim closure  %p   res status     %s    head   %p\n", w->self, victim_id, cl, res, unpack_closure(new_value), Closure_status_to_str(res->status), head + 1);
        CILK_ASSERT(w, cl);
        
        // child->stack_top = victim_w->closure_stack_head;
        CILK_ASSERT(w, cl->stack_top);

        // cl would be suspended if its already been stolen
        CILK_ASSERT(w, cl->status == CLOSURE_RUNNING);
        // CILK_ASSERT(w, cl->owner_ready_deque == pn);

        CILK_ASSERT(w, child);

        CILK_ASSERT(w, res->status == CLOSURE_RUNNING); // can happen if the suspend interleaves after return

        /* cl may have a call parent: it might be promoted as its containing
        * stacklet is stolen, and it's call parent is promoted into full and
        * suspended
        */
        CILK_ASSERT(w, cl == w->g->root_closure || cl->spawn_parent ||
                        cl->call_parent || (cl - 1)->stack_top || cl == cl->stack_top);
        // worst case, check cl - 1 is a valid address or were on a fresh stack

        if (cl_frame == frame_to_steal) {   // stolen before
            //printf("stolen before   w   %d  victim  %d  closure     %p\n", w->self, victim_w->self, cl);
            CILK_ASSERT(w, __cilkrts_stolen(frame_to_steal));
        } else if (trivial_stacklet(frame_to_steal)) { // spawning expression
            //printf("spawning expression   w   %d  victim  %d  closure     %p\n", w->self, victim_w->self, cl);
            CILK_ASSERT(w, __cilkrts_not_stolen(frame_to_steal));
            CILK_ASSERT(w, frame_to_steal->call_parent &&
                            __cilkrts_stolen(frame_to_steal->call_parent));
            CILK_ASSERT(w, (frame_to_steal->flags & CILK_FRAME_LAST) == 0);
            __cilkrts_set_stolen(frame_to_steal);
            Closure_set_frame(w, cl, frame_to_steal);
        } else { // spawning a function and stacklet never gotten stolen before
            //printf("never stolen   w   %d  victim  %d  closure     %p\n", w->self, victim_w->self, cl);
            Closure_set_frame(w, res, frame_to_steal);
            __cilkrts_set_stolen(frame_to_steal);
            
            res->spawn_parent = NULL; // we optimistically set this value but end up setting up 2 closures
            Closure_add_callee(w, cl, res);
            res->call_parent = cl;
            cl->call_parent_fiber = frame_to_steal->fh;

            CILK_ASSERT(w, cl->call_parent_fiber);

            is_left_most = true;

            // suspend cl & remove it from deque
            Closure_suspend_victim(w, victim_w, self, victim_id, cl);
        }

        if (res->orig_rsp == NULL) {
            res->orig_rsp = SP(frame_to_steal);
        }

        CILK_ASSERT(w, res->has_cilk_callee == 0);
        if (!is_left_most) {
            // printf("child   %p   parent    %p    res   %p   w    %d   v   %d\n", child, child->spawn_parent, res, w->self, victim_w->self);
            CILK_ASSERT(w, child->spawn_parent == res);
        }
        
        child->spawn_parent = res;   // need this pointer to put ur map and fiber on return. may matter for when you return and try provably good steal. stodo - dont attempt if null
        // CILK_ASSERT(w, stole_head == res->stack_top);
        /***
         * Register this child, which sets up its sibling links.
         * We do this here instead of in finish_promote, because we must setup
         * the sib links for the new child before its pointer escapses.
         ***/
        Closure_add_child(w, self, res, child, is_left_most);
        increment_join_counter(w, res);

        CILK_ASSERT(w, res->frame == frame_to_steal);

        
        w->fiber = cilk_fiber_allocate_from_pool(w);
        //printf("allocating fiber for worker  %d    victim   %d\n", w->self, victim_id);
        if (is_left_most) {
            struct cilk_fiber *parent_fiber = frame_to_steal->fh;
            CILK_ASSERT(w, parent_fiber);
            CILK_ASSERT(w, res->left_most_fiber == NULL || res->left_most_fiber == parent_fiber);
            res->left_most_fiber = parent_fiber;
            //printf("setting left most fiber for closure  %p   worker   %d   victim   %d\n", res, w->self, victim_id);
        } 

        // // make sure we are not holding the lock on child
        // child->fiber = parent_fiber;
        // if (USE_EXTENSION) {
        //     child->ext_fiber = parent_ext_fiber;
        // }

        /* insert the closure on the victim processor's deque */
        // deque_add_bottom(deques, w, child, self, victim_id);
        cilkrts_alert(STEAL, w,
                    "(Closure_steal) promote gave cl/res/child = %p/%p/%p",
                    (void *)cl, (void *)res, (void *)child);
        //printf("w   %d  won the steal   victim      %d   child status  %s\n", w->self, victim_w->self, Closure_status_to_str(child->status));

        return res;
    }
}

// static bool Closure_stack_pop(struct __cilkrts_worker *const w, Closure **existing_stack) {
//     // always pop from the head
//     //printf("try pop stack   worker   %d \n", w->self);
    
//     Closure* cur_head = NULL;
//     Closure* head_next = NULL;
//     double_ptr stamped_cur_head;
//     double_ptr stamped_head_next;
    
//     while (true) {
//         stamped_cur_head = atomic_load_explicit(&w->g->stack_free_list_head, memory_order_seq_cs);
//         cur_head = get_closure(stamped_cur_head);
//         CILK_ASSERT(w, cur_head);   // not null becuase we use dummy node

//         stamped_head_next = atomic_load_explicit(&cur_head->free_list_next, memory_order_seq_ct);
//         head_next = get_closure(stamped_head_next);
        
//         //printf("popping from stack list  worker   %d    cur head   %p    cur stamp   %d   head next   %p   next stamp   %d\n", w->self, cur_head, get_stamp(stamped_cur_head), head_next, get_stamp(stamped_head_next));
//         // atomic_store_explicit(&cur_head->free_list_next, NULL, memory_order_seq_st);

//         // If head_next is null, queue is empty (only the dummy node)
//         if (head_next == NULL) {
//             return false; // Empty
//         }

//         // The node we want to remove is head_next
//         // Attempt to move 'head' to 'head_next'
//         if (atomic_compare_exchange_strong_explicit(&w->g->stack_free_list_head, &stamped_cur_head, stamped_head_next,
//                                              memory_order_seq_ct,
//                                              memory_order_seq_ct)) {
//             // Successfully dequeued
//             //printf("succeded deque    %p   before was    %p    worker    %d\n", head_next, cur_head, w->self);
//             *existing_stack = head_next;
//             w->g->last_dummy = cur_head;
//             // atomic_store_explicit(&cur_head->free_list_next, NULL, memory_order_seq_st);
//             return true;
//         }
//         // If CAS fails, retry
//     }
// }

// static Closure * Closure_stack_allocate(struct __cilkrts_worker *const w) {
//     Closure *existing_stack = (Closure *)NULL;
//     if (Closure_stack_pop(w, &existing_stack)) {
//         decrement_free_list_size(w);
//         existing_stack->free_list_era += 1;
//         //printf("allocating existing stack    %p    era   %d    worker   %d\n", existing_stack, existing_stack->free_list_era, w->self);
//         return existing_stack;
//     } else {
//         //printf("run alloc  worker   %d\n", w->self);
//         Closure *new_stack = (struct Closure *)calloc(w->g->options.deqdepth, sizeof(struct Closure));
//         CILK_ASSERT(w, new_stack->free_list_era == 0);
//         //printf("allocating new stack    %p    worker    %d     free list size   %d\n", new_stack, w->self, w->g->free_list_size);

//         return new_stack;
//     }
// }


static bool global_stack_pop(struct __cilkrts_worker *const w, Closure **existing_stack) {
    // always pop from the head
    //printf("try pop global stack   worker   %d \n", w->self);
    
    Closure* recent_closure = NULL;
    __uint16_t index = 0;
    __uint64_t counter = 0;

    stack_top_ptr stamped_recent_closure = atomic_load_explicit(&w->g->free_list_top, memory_order_acquire);
    
    while (true) {
        unpack_free_list_top(stamped_recent_closure, &index, &counter, &recent_closure);
        //printf("popping global   top was  %p   ind   %d   counter   %d\n", recent_closure, index, counter);

        // help previous modification
        finish_global_stack_update(w->g, index, counter, recent_closure);

        if (index == 0) {
            return false;
        }

        stack_ptr below_top = atomic_load_explicit(&w->g->free_list[index - 1], memory_order_acquire);
        __uint64_t new_counter;
        Closure *new_closure;
        unpack_free_list_node(below_top, &new_counter, &new_closure);
        if (atomic_compare_exchange_weak_explicit(&w->g->free_list_top, &stamped_recent_closure, pack_free_list_top(index - 1, new_counter + 1, new_closure), memory_order_release, memory_order_acquire)) {
            *existing_stack = recent_closure;
            //printf("popping from stack list  worker   %d    closure   %p  new_ind   %d   next cl is   %p   next_ind   %d\n", w->self, recent_closure, index, new_closure, index - 1);
            return true;
        }
    }

        // prinf("popping from stack list  worker   %d    ")
        
        // atomic_store_explicit(&cur_head->free_list_next, NULL, );

        // If head_next is null, queue is empty (only the dummy node)
        // if (head_next == NULL) {
        //     return false; // Empty
        // }

        // The node we want to remove is head_next
        // Attempt to move 'head' to 'head_next'
        // if (atomic_compare_exchange_strong_explicit(&w->g->stack_free_list_head, &stamped_cur_head, stamped_head_next,
        //                                      memory_order_seq_c\st,
        //                                      memory_order_seq_ct)) {
        //     // Successfully dequeued
        //     //printf("succeded deque    %p   before was    %p    worker    %d\n", head_next, cur_head, w->self);
        //     *existing_stack = head_next;
        //     w->g->last_dummy = cur_head;
        //     // atomic_store_explicit(&cur_head->free_list_next, NULL, memory_order_seq_st);
        //     return true;
        // }
        // If CAS fails, retry
}

static Closure * Closure_stack_allocate(struct __cilkrts_worker *const w) {
    Closure *existing_stack = (Closure *)NULL;
    if ( w->local_free_list_size > 0) {
        CILK_ASSERT(w, w->free_list_head != NULL);
        //printf("worker local allocate   worker   %d\n", w->self);
        existing_stack = w->free_list_head;
        w->free_list_head = existing_stack->free_list_next;
        existing_stack->free_list_era += 1;
        existing_stack->free_list_next = NULL;
        w->local_free_list_size -= 1;
        CILK_ASSERT(w, w->local_free_list_size >= 0);
        if (w->local_free_list_size == 0) {
            CILK_ASSERT(w, w->free_list_head == NULL);
        }
        //printf("allocating worker-local existing stack    %p    era   %d    worker   %d\n", existing_stack, existing_stack->free_list_era, w->self);
        return existing_stack;
    } else {
        bool success = global_stack_pop(w, &existing_stack);
        if (success) {
            existing_stack->free_list_era += 1;
            existing_stack->free_list_next = NULL;
            //printf("allocating global existing stack    %p    era   %d    worker   %d\n", existing_stack, existing_stack->free_list_era, w->self);
            return existing_stack;
        }
        //printf("run alloc  worker   %d\n", w->self);
        Closure *new_stack = (struct Closure *)calloc(w->stack_bitmask + 1, sizeof(struct Closure));

        for (int i = 0; i < w->stack_bitmask + 1; i++) {
            new_stack[i].stack_top = new_stack;
            // Initialize other fields as needed
        }
        w->num_alloc += 1;
        // printf("allocating new stack    %p    worker    %d     free list size   %d\n", new_stack, w->self, w->g->free_list_size);

        return new_stack;
    }
}

/*
 * stealing protocol.  Tries to steal from the victim; returns a
 * stolen closure, or NULL if none.
 */
static Closure *Closure_steal(__cilkrts_worker **workers,
                              __cilkrts_worker *const w,
                              worker_id self, worker_id victim) {
    
    Closure *cl_orig;
    Closure *cl;
    Closure *res = (Closure *)NULL;
    __cilkrts_worker *victim_w;
    victim_w = workers[victim];

    // Fast test for an unsuccessful steal attempt using only read operations.
    // This fast test seems to improve parallel performance.
    double_ptr exc_closure = atomic_load_explicit(&victim_w->exc_closure, memory_order_relaxed);
    __cilkrts_stack_frame **head = unpack_exc(exc_closure);
    uint64_t tail_idx = atomic_load_explicit(&victim_w->tail, memory_order_relaxed);
    __cilkrts_stack_frame **tail = victim_w->ltq_start + tail_idx;

    if (head >= tail) {
        WHEN_SCHED_STATS(w->l->stats.empty_deque++);
        return NULL;
    }

    cl = unpack_closure(exc_closure);

    // printf("i am victim     %d      worker      %d      closure     %p     old_exc   %p     tail   %p\n", victim_w->self, w->self, cl, old_exc, tail);
    // CILK_ASSERT_POINTER_EQUAL(w, cl_orig, cl);

    if (cl) {
        // if (Closure_hit_sync(atomic_load_explicit(&cl->join_counter, memory_order_relaxed))) {
        //     printf("w   %d   skipping steal of sync suspended closure    %p\n", w->self, cl);
        //     return NULL;
        // }
        // stodo does this help performance
        CILK_ASSERT(w, cl);     // may not be true if worker already got rid of stack
        enum ClosureStatus status = cl->status;

        // cilkrts_alert(STEAL, "[%d]: trying steal from W%d; cl=%p",
        // (void *)victim, (void *)cl);

        switch (status) {
        case CLOSURE_RUNNING: {

            /* send the exception to the worker */
            __cilkrts_stack_frame **head = do_dekker_on(w, self, victim_w, cl);
            // memlogger_logf("i am victim     %d      worker      %d      closure     %p     old_exc   %p     tail   %p\n", victim_w->self, w->self, cl, head + 1, tail);

            if (head) {
                cilkrts_alert(STEAL, w,
                              "(Closure_steal) can steal from W%d; cl=%p",
                              victim, (void *)cl);

                __cilkrts_stack_frame *cl_frame = cl->frame;
                int64_t era = cl->stack_top->free_list_era;

                res = extract_top_spawning_closure(head, w, victim_w,
                                                   cl, self, victim, cl_frame);

                // at this point, more steals can happen from the victim.
                // deque_unlock(deques, self, victim);

                if (res == (Closure *)NULL) {
                    WHEN_SCHED_STATS(w->l->stats.extract_closure_failed++);
                    goto give_up;
                }
                
                CILK_ASSERT(w, w->fiber);
                CILK_ASSERT(w, cl->frame == cl_frame);
                CILK_ASSERT(w, cl->stack_top->free_list_era == era);    // sanity check to make sure this isnt a reuse of the stack
                // Closure_assert_ownership(w, self, res);

                // ANGE: finish the promotion process in finish_promote
                finish_promote(w, self, victim_w, res,
                               /* has_frames_to_promote */ false);

                cilkrts_alert(STEAL, w,
                              "(Closure_steal) success; res %p has "
                              "fiber %p; left most child has fiber %p",
                              (void *)res, (void *)w->fiber,
                              (void *)res->left_most_fiber);
                setup_for_execution(w, res);
                //printf("closure stack head was   %p   worker   %d    eq %d\n", w->closure_stack_head, w->self, w->closure_stack_head == NULL);
                
                w->closure_stack_head = Closure_stack_allocate(w);
                CILK_ASSERT(w, w->closure_stack_head);
                atomic_store_explicit(&w->closure_stack_tail, w->closure_stack_head, memory_order_release);
                // memlogger_logf("using stack   %p   free list era    %d   victim   %d   worker   %d   my tail   %p\n", w->closure_stack_head, w->closure_stack_head->free_list_era, victim_w->self, w->self, atomic_load_explicit(&w->tail, memory_order_relaxed));
                w->closure_stack_head->spawn_parent = res;
            } else {
                WHEN_SCHED_STATS(w->l->stats.null_head++);
                goto give_up;
            }
            break;
        }
        case CLOSURE_RETURNING: /* ok, let it leave alone */
            WHEN_SCHED_STATS(w->l->stats.closure_returning++);
        give_up:
            // MUST unlock the closure before the queue;
            // see rule D in the file PROTOCOLS
            // Closure_unlock(w, self, cl);
            // deque_unlock(deques, self, victim);
            break;
        case CLOSURE_SUSPENDED:
            WHEN_SCHED_STATS(w->l->stats.closure_suspended++);
            // you might have been pre-empted, come back and realized someone else already stole
            //printf("Found suspended closure in ready deque    closure    %p      worker  %d    victim   %d\n", cl, w->self, victim_w->self);
            break;
        case CLOSURE_POST_INVALID:
            WHEN_SCHED_STATS(w->l->stats.closure_post_invalid++);
            // you might have been pre-empted, come back and realized someone else already stole
            //printf("Found post invalid closure in ready deque    closure    %p      worker  %d    victim   %d\n", cl, w->self, victim_w->self);
            break;
        case CLOSURE_PRE_INVALID:
            WHEN_SCHED_STATS(w->l->stats.closure_pre_invalid++);
            // closure may not be fully ready yet but thats okay
            //printf("Found pre invalid closure in ready deque    closure    %p      worker  %d    victim   %d\n", cl, w->self, victim_w->self);
            break;
        case CLOSURE_READY:
            WHEN_SCHED_STATS(w->l->stats.closure_ready++);
            // closure may not be fully ready yet but thats okay
            //printf("Found ready closure in ready deque    closure    %p      worker  %d    victim   %d\n", cl, w->self, victim_w->self);
            break;
        case CLOSURE_SYNC:
            WHEN_SCHED_STATS(w->l->stats.closure_sync++);
            // closure hit a sync
            //printf("Found sync suspended closure in ready deque    closure    %p      worker  %d    victim   %d\n", cl, w->self, victim_w->self);
            break;
        default:
            // It's possible that this steal attempt peeked the root closure
            // from the top of a deque while a new Cilkified region was
            // starting.
            if (cl != w->g->root_closure)
                cilkrts_bug(victim_w, "Bug: %s closure in ready deque    closure    %p      worker  %d    victim   %d",
                            Closure_status_to_str(cl->status), cl, w->self, victim_w->self);
        }
    } else {
        // deque_unlock(deques, self, victim);
        WHEN_SCHED_STATS(w->l->stats.null_closure++);
        //----- EVENT_STEAL_EMPTY_DEQUE
    }

    return res;
}

// ==============================================
// Scheduling functions
// ==============================================

CHEETAH_INTERNAL_NORETURN
void longjmp_to_user_code(__cilkrts_worker *w, Closure *t) {
    CILK_ASSERT(w, w->l->state == WORKER_RUN);

    __cilkrts_stack_frame *sf = t->frame;
    struct cilk_fiber *fiber = w->fiber;

    CILK_ASSERT(w, sf && fiber);

    local_state *l = w->l;
    if (l->provably_good_steal) {
        // in this case, we simply longjmp back into the original fiber
        // the SP(sf) has been updated with the right orig_rsp already

        // NOTE: This is a hack to disable these asserts if we are longjmping to
        // the personality function.  __cilkrts_throwing(sf) is true only when
        // the personality function is syncing sf.
        if (!__cilkrts_throwing(sf)) {
            CILK_ASSERT(w, t->orig_rsp == NULL);
            CILK_ASSERT(w, (sf->flags & CILK_FRAME_LAST) ||
                               in_fiber(fiber, (char *)FP(sf)));
            CILK_ASSERT(w, in_fiber(fiber, (char *)SP(sf)));
        }

        l->provably_good_steal = false;
    } else { // this is stolen work; the fiber is a new fiber
        // This is the first time we run the root closure in this Cilkified
        // region.  The closure has been completely setup at this point by
        // invoke_cilkified_root().  We just need jump to the user code.
        global_state *g = w->g;
        bool *initialized = &g->root_closure_initialized;
        if (t == g->root_closure && *initialized == false) {
            *initialized = true;
        } else {
            //printf("longjmp fiber  %p    worker   %d\n", fiber, w->self);
            void *new_rsp = sysdep_reset_stack_for_resume(fiber, sf);
            USE_UNUSED(new_rsp);
            CILK_ASSERT(w, SP(sf) == new_rsp);
            // if (USE_EXTENSION) {
            //     w->extension = sf->extension;
            //     w->ext_stack = sysdep_get_stack_start(t->ext_fiber);
            // }
        }
    }
    CILK_SWITCH_TIMING(w, INTERVAL_SCHED, INTERVAL_WORK);
#if CILK_ENABLE_ASAN_HOOKS
    if (!__cilkrts_throwing(sf)) {
        sanitizer_start_switch_fiber(fiber);
    } else {
        struct closure_exception *exn_r = get_exception_reducer_or_null(w);
        if (exn_r) {
            sanitizer_start_switch_fiber(exn_r->throwing_fiber);
        }
    }
#endif // CILK_ENABLE_ASAN_HOOKS
    sysdep_longjmp_to_sf(sf);
}

__attribute__((noreturn)) void longjmp_to_runtime(__cilkrts_worker *w) {
    cilkrts_alert(SCHED | ALERT_FIBER, w, "(longjmp_to_runtime)");

    CILK_SWITCH_TIMING(w, INTERVAL_WORK, INTERVAL_SCHED);
    /* Can't change to WORKER_SCHED yet because the reducer map
       may still be set. */
    sanitizer_start_switch_fiber(NULL);
    __builtin_longjmp(w->l->rts_ctx, 1);
}

/* This function implements a sync in user code, including the implicit
   sync at the end of a function.  It is only called if compiled code
   finds CILK_FRAME_UNSYCHED is set.  It returns SYNC_READY if there
   are no children and execution can continue.  Otherwise it returns
   SYNC_NOT_READY to suspend the frame. */
int Cilk_sync(__cilkrts_worker *const w, __cilkrts_stack_frame *frame) {

    // cilkrts_alert(SYNC, w, "(Cilk_sync) frame %p", (void *)frame);

    Closure *t;
    int res = SYNC_READY;

    //----- EVENT_CILK_SYNC
    worker_id self = w->self;
    // deque_lock_self(deques, self);
    // t_orig = deque_peek_bottom(deques, w, self, self);
    double_ptr exc_closure = atomic_load_explicit(&w->exc_closure, memory_order_relaxed);
    t = unpack_closure(exc_closure);
    __cilkrts_stack_frame** old_exc = unpack_exc(exc_closure);
    uint64_t tail_idx = atomic_load_explicit(&w->tail, memory_order_relaxed);
    __cilkrts_stack_frame **tail = w->ltq_start + tail_idx;
    CILK_ASSERT(w, old_exc == tail); // if not true, got stolen?
    
    // CILK_ASSERT(w, t_orig == t);
    // Closure_lock(w, self, t);
    /* assert we are really at the top of the stack */
    // CILK_ASSERT(w, Closure_at_top_of_stack(w, frame)); //stodo might be false 
    //printf("syncing worker  %d      closure     %p      exc   %p   tail  %p\n", w->self, t, old_exc, tail);
    

    CILK_ASSERT(w, t);

    // printf("syncing  worker  %d      closure     %p      stack top    %p    children   %d   closure_status      %s    frame   %p     head   %p\n", w->self, t, t->stack_top, atomic_load_explicit(&t->join_counter, ), Closure_status_to_str(t->status), frame, old_exc);
    CILK_ASSERT(w, t->status == CLOSURE_RUNNING);
    CILK_ASSERT(w, frame && (t->frame == frame));
    CILK_ASSERT(w, __cilkrts_stolen(frame));
    CILK_ASSERT(w, t->has_cilk_callee == 0);
    // CILK_ASSERT(w, w, t->frame->magic == CILK_STACKFRAME_MAGIC);

    // each sync is executed only once; since we occupy user_ht only
    // when sync fails, the user_ht should remain NULL at this point.
    CILK_ASSERT(w, t->user_ht == (hyper_table *)NULL);

    int32_t join_counter;
    bool hit_sync;
    bool suspended = false;

    hyper_table *ht = w->hyper_table;
    w->hyper_table = NULL;

    t->user_ht = ht; /* was reset this in case we dont suspend */
    __uint64_t parent_state = atomic_load_explicit(&t->join_counter, memory_order_acquire);

    while (true) {
        unpack_join_counter(parent_state, &join_counter, &hit_sync);
        CILK_ASSERT(w, !hit_sync);
        
        if (join_counter != 0) {
            
            __uint64_t new_jc = pack_join_counter(join_counter, true);

            Closure_change_status(w, t, CLOSURE_RUNNING, CLOSURE_SYNC);
            // stodo
            if (!atomic_compare_exchange_weak_explicit(&t->join_counter, &parent_state, new_jc, memory_order_release, memory_order_acquire)) {
                // join counter changed
                Closure_change_status(w, t, CLOSURE_SYNC, CLOSURE_RUNNING);
            } else {
                double_ptr new_value = pack_pointers(old_exc, (Closure *)NULL);
                atomic_store_explicit(&w->exc_closure, new_value, memory_order_release);
                suspended = true;
                break;
            }

            // suspended = Closure_suspend_on_sync(w, self, t, old_exc + 1, join_counter, &parent_state);

             
            //printf("suspend  %d   closure  %p\n", self, cl);

            //wont work stodo
            // double_ptr fetch = pack_pointers(old_exc, cl);

            // STODO do i need to loop
            

            // bool success = expected_abort(w, old_exc, (Closure *)NULL, &fetch);
            // if (!success) {
            //     //printf("closure was actually   %p    exc was   %p    worker   %d\n", unpack_closure(fetch), unpack_exc(fetch), w->self);
            //     CILK_ASSERT(w, false);
            // }
            // return true;
            // if (suspended) {
            //     break;
            // }
        } else {
            break;
        }
    }

    if (suspended) {
        cilkrts_alert(SYNC, w,
                      "(Cilk_sync) Closure %p has outstanding children",
                      (void *)t);
        if (w->fiber) {
            // //printf("deallocating fiber sync for worker  %d    closure   %p   fiber  %p\n", w->self, t, w->fiber);
            cilk_fiber_deallocate_to_pool(w, w->fiber);
            //  && w->closure_stack_head->spawn_parent->stack_top != t->stack_top && w->closure_stack_head->call_parent == NULL

            if (w->closure_stack_head != NULL && w->closure_stack_head != w->g->root_closure && w->closure_stack_head != t->stack_top) {
                //printf("sync closure stack freeing   %p   worker   %d\n", w->closure_stack_head, w->self);
                // memlogger_logf("sync closure stack freeing   %p   free list era    %d   worker   %d    head    %p     tail     %p\n", w->closure_stack_head, w->closure_stack_head->free_list_era, w->self, old_exc + 1, tail + 1);
                Closure_destroy(w, w->closure_stack_head);
            }
        }
        
        // if (t->stack_top != w->closure_stack_head && w->closure_stack_head != NULL && t != w->g->root_closure) {
        //     Closure_clean(w, w->closure_stack_head);
        //     Closure_stack_free(w, w->closure_stack_head);
        // }
        // if (USE_EXTENSION && t->ext_fiber) {
        //     cilk_fiber_deallocate_to_pool(w, t->ext_fiber);
        // }
        w->fiber = NULL;
        // t->ext_fiber = NULL;
        // Place holder for the current reducer hypermap.  Other hypermaps will
        // be reduced before the sync as this Closure's children return, and
        // views in this hypermap will need to be reduced with those when a
        // provably good steal occurs.
        res = SYNC_NOT_READY;
        // Closure_change_status(w, t, CLOSURE_RUNNING, CLOSURE_SYNC);
        //printf("suspended reset tail   worker   %d   stack   %p\n", w->self, w->closure_stack_head);
        // atomic_store_explicit(&w->tail, tail + 1, memory_order_relaxed);
        w->closure_stack_head = NULL;
        atomic_store_explicit(&w->closure_stack_tail, NULL, memory_order_relaxed);
        
    } else {
        cilkrts_alert(SYNC, w, "(Cilk_sync) closure %p sync successfully",
                      (void *)t);
        
        // atomic_store_explicit(&w->exc_closure, pack_pointers(old_exc + 1, t), memory_order_release);
        // // reset_exception_pointer(w, self, t, old_exc, &exc_closure);
        // // CILK_ASSERT(w, success);
        // // won the race
        // // stodo should i do tail+1
        // // //printf("w   %d  won the race setting head   closure   %p    exc   %p    tail    %p\n", self, t, old_exc + 1, tail + 1);
        // atomic_store_explicit(&w->tail, tail + 1, memory_order_relaxed);
        
        // in the case that we dont end up suspending, we want to keep the hypertable on the worker not closure
        setup_for_sync(w, self, t);
    }

    // Closure_unlock(w, self, t);
    // deque_unlock_self(deques, self);

    if (res == SYNC_READY) {
        // will deposit remaining children views into t->child_ht
        hyper_table *child_ht = t->child_ht;
        CILK_ASSERT(w, w->hyper_table == NULL);
        if (child_ht) {
            t->child_ht = NULL;
            w->hyper_table = merge_two_hts(w, child_ht, t->user_ht);
            t->user_ht = NULL;
        } else {
            w->hyper_table = t->user_ht;
            t->user_ht = NULL;
        }

#if CILK_ENABLE_ASAN_HOOKS
        sanitizer_unpoison_fiber(w->fiber);
        if (!__cilkrts_throwing(frame)) {
            sanitizer_start_switch_fiber(w->fiber);
        } else {
            struct closure_exception *exn_r = get_exception_reducer_or_null(w);
            if (exn_r) {
                sanitizer_start_switch_fiber(exn_r->throwing_fiber);
            }
        }
#endif // CILK_ENABLE_ASAN_HOOKS
    }

    return res;
}

static void do_what_it_says(__cilkrts_worker *w,
                            worker_id self, Closure *t) {
    __cilkrts_stack_frame *f;
    local_state *l = w->l;

    do {
        cilkrts_alert(SCHED, w, "(do_what_it_says) closure %p", (void *)t);

        switch (t->status) {
        case CLOSURE_RUNNING:
            cilkrts_alert(SCHED, w, "(do_what_it_says) CLOSURE_READY");
            /* just execute it */
            f = t->frame;
            cilkrts_alert(SCHED, w, "(do_what_it_says) resume_sf = %p",
                          (void *)f);
            //printf("do_what_it_says     w   %d  closure   %p    stack top     %p\n", w->self, t, t->stack_top);
            CILK_ASSERT(w, f);
            USE_UNUSED(f);

            // MUST unlock the closure before locking the queue
            // (rule A in file PROTOCOLS)
            // deque_lock_self(deques, self);
            // deque_add_bottom(deques, w, t, self, self);
            // move this to setup_for_execution
            // update_closure_abort(w, t);

            //printf("w   %d      claimed     %p\n", w->self, t);

            // deque_unlock_self(deques, self);

            /* now execute it */
            cilkrts_alert(SCHED, w, "(do_what_it_says) Jump into user code");

            // longjmp invalidates non-volatile variables
            __cilkrts_worker *volatile w_save = w;
            if (__builtin_setjmp(l->rts_ctx) == 0) {
                //printf("jumping to user code  worker    %d    closure   %p\n", w->self, t);
                worker_change_state(w, WORKER_RUN);
                longjmp_to_user_code(w, t);
            } else {
                w = w_save;
                l = w->l;
                self = w->self;
                __cilkrts_current_fh = NULL;
                CILK_ASSERT_POINTER_EQUAL(w, w, __cilkrts_get_tls_worker());
                sanitizer_finish_switch_fiber();
                worker_change_state(w, WORKER_SCHED);

                // If this worker finished the cilkified region, mark the
                // computation as no longer cilkified, to signal the thread that
                // originally cilkified the execution.
                if (l->exiting) {
                    l->exiting = false;
                    global_state *g = w->g;
                    CILK_EXIT_WORKER_TIMING(g);
                    signal_uncilkified(g);
                    return;
                }

                t = NULL;
                if (l->returning) {
                    l->returning = false;
                    t = w->return_closure;
                    w->return_closure = NULL;
                    // Attempt to get a closure from the bottom of our deque.
                    // We should already have the lock on the deque at this
                    // point, as we jumped here from Cilk_exception_handler.
                    // t = fetch_and_update_closure(w, cur_head, (Closure *)NULL);
                    CILK_ASSERT(w, t->status == CLOSURE_RETURNING);
                    //printf("return extracting closure  %p      worker      %d\n", t, self);
                }
            }

            break; // ?

        case CLOSURE_RETURNING:
            cilkrts_alert(SCHED, w, "(do_what_it_says) CLOSURE_RETURNING");
            // The return protocol requires t to not be locked, so that it can
            // acquire locks on t and t's parent in the correct order.
            t = return_value(w, self, t);

            break; // ?

        default:
            cilkrts_bug(w, "do_what_it_says() invalid closure status: %s",
                        Closure_status_to_str(t->status));
            break;
        }
        if (t) {
            WHEN_SCHED_STATS(l->stats.repos++);
        }
    } while (t);
}

// Thin wrapper around do_what_it_says to allow the boss thread to execute the
// Cilk computation until it would enter the work-stealing loop.
void do_what_it_says_boss(__cilkrts_worker *w, Closure *t) {
    //printf("do what it says boss    w   %d      closure     %p\n", w->self, t);
    CILK_ASSERT(w, t == w->g->root_closure);
    w->fiber = w->g->root_fiber;
    w->closure_stack_head = t;
    atomic_store_explicit(&w->closure_stack_tail, t + 1, memory_order_release);
    w->closure_stack_tail->spawn_parent = t;

    Closure_clean_root(t);

    setup_for_execution(w, t);

    worker_id self = w->self;
    do_what_it_says(w, self, t);

    // At this point, the boss has run out of work to do.  Rather than become a
    // thief itself, the boss wakes up the root worker to become a thief.

    CILK_STOP_TIMING(w, INTERVAL_SCHED);
    worker_change_state(w, WORKER_IDLE);
#if BOSS_THIEF
    worker_scheduler(w);
#else
    __builtin_longjmp(w->g->boss_ctx, 1);
#endif
}

void worker_scheduler(__cilkrts_worker *w) {
    //printf("scheduler   worker   %d\n", w->self);
    Closure *t = NULL;
    CILK_ASSERT(w, w == __cilkrts_get_tls_worker());

    CILK_START_TIMING(w, INTERVAL_SCHED);
    worker_change_state(w, WORKER_SCHED);
    global_state *rts = w->g;
    worker_id self = w->self;
    const bool is_boss = (0 == self);

    // Get this worker's local_state pointer, to avoid rereading it
    // unnecessarily during the work-stealing loop.  This optimization helps
    // reduce sharing on the worker structure.
    local_state *l = w->l;
    unsigned int rand_state = l->rand_next;

    // Get the number of workers.  We don't currently support changing the
    // number of workers dynamically during execution of a Cilkified region.
    unsigned int nworkers = rts->nworkers;

    // Initialize count of consecutive failed steal attempts.
    unsigned int fails = init_fails(l->wake_val, rts);
    unsigned int sample_threshold = SENTINEL_THRESHOLD;
    // Local history information of the state of the system, for sentinel
    // workers to use to determine when to disengage and how many workers to
    // reengage.
    history_t inefficient_history = 0;
    history_t efficient_history = 0;
    unsigned int sentinel_count_history[SENTINEL_COUNT_HISTORY] = { 1 };
    unsigned int sentinel_count_history_tail = 0;
    unsigned int recent_sentinel_count = SENTINEL_COUNT_HISTORY;

    // Get pointers to the local and global copies of the index-to-worker map.
    worker_id *index_to_worker = rts->index_to_worker;
    __cilkrts_worker **workers = rts->workers;

    while (!atomic_load_explicit(&rts->done, memory_order_acquire)) {
        /* A worker entering the steal loop must have saved its reducer map into
           the frame to which it belongs. */
        CILK_ASSERT(w, !w->hyper_table ||
                           (is_boss && atomic_load_explicit(
                                           &rts->done, memory_order_acquire)));

        CILK_STOP_TIMING(w, INTERVAL_SCHED);

        while (!t && !atomic_load_explicit(&rts->done, memory_order_acquire)) {
            CILK_START_TIMING(w, INTERVAL_SCHED);
            CILK_START_TIMING(w, INTERVAL_IDLE);
#if ENABLE_THIEF_SLEEP
            // Get the set of workers we can steal from and a local copy of the
            // index-to-worker map.  We'll attempt a few steals using these
            // local copies to minimize memory traffic.
            uint64_t disengaged_sentinel = atomic_load_explicit(
                &rts->disengaged_sentinel, memory_order_relaxed);
            uint32_t disengaged = GET_DISENGAGED(disengaged_sentinel);
            uint32_t stealable = nworkers - disengaged;
            __attribute__((unused))
            uint32_t sentinel = recent_sentinel_count / SENTINEL_COUNT_HISTORY;

            if (__builtin_expect(stealable == 1, false)) {
                // If this worker detects only 1 stealable worker, then its the
                // only worker in the work-stealing loop.
                CILK_DROP_TIMING(w, INTERVAL_SCHED);
                CILK_DROP_TIMING(w, INTERVAL_IDLE);
                continue;
            }

#else // ENABLE_THIEF_SLEEP
            uint32_t stealable = nworkers;
            __attribute__((unused))
            uint32_t sentinel = nworkers / 2;
#endif // ENABLE_THIEF_SLEEP
#ifndef __APPLE__
            uint32_t lg_sentinel = sentinel == 0 ? 1
                                                 : (8 * sizeof(sentinel)) -
                                                       __builtin_clz(sentinel);
            uint32_t sentinel_div_lg_sentinel =
                sentinel == 0 ? 1
                              : (sentinel >> (8 * sizeof(lg_sentinel) -
                                              __builtin_clz(lg_sentinel)));
#endif
            const unsigned int NAP_THRESHOLD = SENTINEL_THRESHOLD * 64;

#if !defined(__aarch64__) && !defined(__APPLE__)
            uint64_t start = __builtin_readcyclecounter();
#endif // !defined(__aarch64__) && !defined(__APPLE__)
            int attempt = ATTEMPTS;
            do {
                // Choose a random victim not equal to self.
                worker_id victim =
                        index_to_worker[get_rand(rand_state) % stealable];
                rand_state = update_rand_state(rand_state);
                while (victim == self) {
                    victim = index_to_worker[get_rand(rand_state) % stealable];
                    rand_state = update_rand_state(rand_state);
                }
                // Attempt to steal from that victim.
                t = Closure_steal(workers, w, self, victim);
                if (!t) {
                    // Pause inside this busy loop.
                    busy_loop_pause();
                }
            } while (!t && --attempt > 0);

#if SCHED_STATS
            if (t) { // steal successful
                WHEN_SCHED_STATS(w->l->stats.steals++);
                CILK_STOP_TIMING(w, INTERVAL_SCHED);
                CILK_DROP_TIMING(w, INTERVAL_IDLE);
            } else { // steal unsuccessful
                CILK_STOP_TIMING(w, INTERVAL_IDLE);
                CILK_DROP_TIMING(w, INTERVAL_SCHED);
            }
#endif

            fails = go_to_sleep_maybe(
                rts, self, nworkers, NAP_THRESHOLD, w, t, fails,
                &sample_threshold, &inefficient_history, &efficient_history,
                sentinel_count_history, &sentinel_count_history_tail,
                &recent_sentinel_count);

            if (!t) {
                // Add some delay to the time a worker takes between steal
                // attempts.  On a variety of systems, this delay seems to
                // improve parallel performance of Cilk computations where
                // workers spend a signficant amount of time stealing.
                //
                // The computation for the delay is heuristic, based on the
                // following:
                // - Incorporate some delay for each steal attempt.
                // - Increase the delay for workers who fail a lot of steal
                //   attempts, and allow successful thieves to steal more
                //   frequently.
                // - Increase the delay based on the number of thieves failing
                //   lots of steal attempts.  In this case, we use the number S
                //   of sentinels and increase the delay by approximately S/lg
                //   S, which seems to work better than a linear increase in
                //   practice.
                CILK_START_TIMING(w, INTERVAL_IDLE);
#ifndef __APPLE__
#ifndef __aarch64__
                uint64_t stop = 450 * ATTEMPTS;
                if (fails > stealable)
                    stop += 650 * ATTEMPTS;
                stop *= sentinel_div_lg_sentinel;
                // On x86-64, the latency of a pause instruction varies between
                // microarchitectures.  We use the cycle counter to delay by a
                // certain amount of time, regardless of the latency of pause.
                while ((__builtin_readcyclecounter() - start) < stop) {
#if SCHED_YIELD_BETWIXT_STEALS
                   sched_yield();
#else
                   busy_pause();
#endif                   
                   //usleep(0);
                }
#else
                int pause_count = 200 * ATTEMPTS;
                if (fails > stealable)
                    pause_count += 50 * ATTEMPTS;
                pause_count *= sentinel_div_lg_sentinel;
                // On arm64, we can't necessarily read the cycle counter without
                // a kernel patch.  Instead, we just perform some number of
                // pause instructions.
                for (int i = 0; i < pause_count; ++i)
                    busy_pause();
#endif // __aarch64__
#endif // __APPLE__
                CILK_STOP_TIMING(w, INTERVAL_IDLE);
            }
        }
        CILK_START_TIMING(w, INTERVAL_SCHED);
        // If one Cilkified region stops and another one starts, then a worker
        // can reach this point with t == NULL and w->g->done == false.  Check
        // that t is not NULL before calling do_what_it_says.
        if (t) {
#if ENABLE_THIEF_SLEEP
            const unsigned int MIN_FAILS = 2 * ATTEMPTS;
            uint64_t start, end;
            // Executing do_what_it_says involves some minimum amount of work,
            // which can be used to amortize the cost of some failed steal
            // attempts.  Therefore, avoid measuring the elapsed cycles if we
            // haven't failed many steal attempts.
            if (fails > MIN_FAILS) {
                start = gettime_fast();
            }
#endif // ENABLE_THIEF_SLEEP
            do_what_it_says(w, self, t);
#if ENABLE_THIEF_SLEEP
            if (fails > MIN_FAILS) {
                end = gettime_fast();
                uint64_t elapsed = end - start;
                // Decrement the count of failed steal attempts based on the
                // amount of work done.
                fails = decrease_fails_by_work(rts, w, fails, elapsed,
                                               &sample_threshold);
                if (fails < SENTINEL_THRESHOLD) {
                    inefficient_history = 0;
                    efficient_history = 0;
                }
            } else {
                fails = 0;
                sample_threshold = SENTINEL_THRESHOLD;
            }
#endif // ENABLE_THIEF_SLEEP
            t = NULL;
        } else if (!is_boss &&
                   atomic_load_explicit(&rts->done, memory_order_relaxed)) {
            // If it appears the computation is done, busy-wait for a while
            // before exiting the work-stealing loop, in case another cilkified
            // region is started soon.
            unsigned int busy_fail = 0;
            while (busy_fail++ < BUSY_LOOP_SPIN &&
                   atomic_load_explicit(&rts->done, memory_order_relaxed)) {
                busy_pause();
            }
            if (thief_should_wait(rts)) {
                break;
            }
        }
    }

    // Reset the fail count.
#if ENABLE_THIEF_SLEEP
    reset_fails(rts, fails);
#endif
    l->rand_next = rand_state;

    CILK_STOP_TIMING(w, INTERVAL_SCHED);
    worker_change_state(w, WORKER_IDLE);
#if BOSS_THIEF
    if (is_boss) {
        __builtin_longjmp(rts->boss_ctx, 1);
    }
#endif
}

void *scheduler_thread_proc(void *arg) {
    struct worker_args *w_arg = (struct worker_args *)arg;
    __cilkrts_worker *w = __cilkrts_init_tls_worker(w_arg->id, w_arg->g);

    cilkrts_alert(BOOT, w, "scheduler_thread_proc");
    __cilkrts_set_tls_worker(w);

#if BOSS_THIEF
    CILK_ASSERT(w, w->self != 0);
#endif
    // Initialize the worker's fiber pool.  We have each worker do this itself
    // to improve the locality of the initial fibers.
    cilk_fiber_pool_per_worker_init(w);

    // Avoid redundant lookups of these commonly accessed worker fields.
    const worker_id self = w->self;
    global_state *rts = w->g;
    local_state *l = w->l;
    const unsigned int nworkers = rts->nworkers;

    // Initialize worker's random-number generator.
    rts_srand(w, (self + 1) * 162347);

    CILK_START_TIMING(w, INTERVAL_SLEEP_UNCILK);
    do {
        l->wake_val = nworkers;
        // Wait for g->start == 1 to start executing the work-stealing loop.  We
        // use a condition variable to wait on g->start, because this approach
        // seems to result in better performance.
#if !BOSS_THIEF
        if (self == rts->exiting_worker) {
            root_worker_wait(rts, self);
        } else {
#endif
            if (thief_should_wait(rts)) {
                disengage_worker(rts, nworkers, self);
                l->wake_val = thief_wait(rts);
                reengage_worker(rts, nworkers, self);
            }
#if !BOSS_THIEF
        }
#endif
        CILK_STOP_TIMING(w, INTERVAL_SLEEP_UNCILK);

        // Check if we should exit this scheduling function.
        if (rts->terminate) {
            return NULL;
        }

        // Start the new Cilkified region using the last worker that finished a
        // Cilkified region.  This approach ensures that the new Cilkified
        // region starts on an available worker with the worker state that was
        // updated by any operations that occurred outside of Cilkified regions.
        // Such operations, for example might have updated the left-most view of
        // a reducer.
        if (!atomic_load_explicit(&rts->done, memory_order_acquire)) {
            worker_scheduler(w);
        }

        // At this point, some worker will have finished the Cilkified region,
        // meaning it recorded its ID in g->exiting_worker and set g->done = 1.
        // That worker's state accurately reflects the execution of the
        // Cilkified region, including all updates to reducers.  Wait for that
        // worker to exit the work-stealing loop, and use it to wake-up the
        // original Cilkifying thread.
        CILK_START_TIMING(w, INTERVAL_SLEEP_UNCILK);
    } while (true);
}

