#include <stdlib.h>
#include <stdio.h>
#include <stdlib.h>

#include "../runtime/cilk2c.h"
#include "../runtime/cilk2c_inlined.c"
#include "ktiming.h"


#ifndef TIMING_COUNT 
#define TIMING_COUNT 1 
#endif

/* 
 * fib 39: 63245986
 * fib 40: 102334155
 * fib 41: 165580141 
 * fib 42: 267914296
   
int fib(int n) {
    int x, y, _tmp;

    if(n < 2) {
        return n;
    }
    
    x = cilk_spawn fib(n - 1);
    y = fib(n - 2);
    cilk_sync;

    return x+y;
}
*/

extern size_t ZERO;
void __attribute__((weak)) dummy(void *p) { return; }

void zero_i(void *v) { *(int *)v = 0; }
void plus_i(void *l, void *r) { *(int *)l += *(int *)r; }

int result;

void deinit_global_reducer(void) {
    __cilkrts_reducer_unregister(&result);
}

__attribute__((constructor))
void init_global_reducer(void) {
    atexit(deinit_global_reducer);
    __cilkrts_reducer_register(&result, sizeof(result), zero_i, plus_i);
}

static void __attribute__ ((noinline)) fib_spawn_helper(int n,
                                                        __cilkrts_stack_frame *parent); 

void fib(int n) {

    int my_n = n;
    
    dummy(alloca(ZERO));
    __cilkrts_stack_frame sf;
    __cilkrts_enter_frame(&sf);

start:
    if(my_n < 2) {
        int *v = (int *)__cilkrts_reducer_lookup(&result,sizeof(result), zero_i, plus_i);
        *v += my_n;
        
        /* cilk_sync */
        __cilk_sync_nothrow(&sf);

        __cilk_parent_epilogue(&sf);
        return;
    }

    /* x = spawn fib(n-1) */
    if (!__cilk_prepare_spawn(&sf)) {
      fib_spawn_helper(my_n-1, &sf);
    }

    // fib(n - 2);
    my_n -= 2;
    goto start;
}

static void __attribute__ ((noinline)) fib_spawn_helper(int n,
                                                        __cilkrts_stack_frame *parent) {

    __cilkrts_stack_frame sf;
    __cilkrts_enter_frame_helper(&sf, parent, false);
    __cilkrts_detach(&sf, parent);
    fib(n);
    __cilk_helper_epilogue(&sf, parent, false);
}

int main(int argc, char * args[]) {
    int i;
    int n, res;
    clockmark_t begin, end; 
    uint64_t running_time[TIMING_COUNT];

    if(argc != 2) {
        fprintf(stderr, "Usage: fib [<cilk-options>] <n>\n");
        exit(1);
    }
    
    n = atoi(args[1]);

    for(i = 0; i < TIMING_COUNT; i++) {
        result = 0;
        begin = ktiming_getmark();
        fib(n);
        res = *(int *)__cilkrts_reducer_lookup(&result, sizeof(result), zero_i, plus_i);
        end = ktiming_getmark();
        running_time[i] = ktiming_diff_nsec(&begin, &end);
    }
    printf("Result: %d\n", res);
    print_runtime(running_time, TIMING_COUNT); 

    return 0;
}