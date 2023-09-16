#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <signal.h>

#include <criterion/criterion.h>
#include <criterion/new/assert.h>
#include <criterion/parameterized.h>

// TODO: Use a mocking library
// Dummy implementation of __cilkrts_get_worker_number.
unsigned __cilkrts_get_worker_number(void) { return 0; }

#define CHEETAH_INTERNAL
#include "runtime/local-hypertable.h"

#define TRACE 0

// Print additional trace information if TRACE == 1.
static inline void PRINT_TRACE(const char *fmt, ...) {
#if TRACE
    va_list l;
    va_start(l, fmt);
    vfprintf(stderr, fmt, l);
    va_end(l);
#endif
}

// Structures for specifying simple table commands for tests.
enum table_command_type {
    TABLE_INSERT,
    TABLE_LOOKUP,
    TABLE_DELETE
};
typedef struct table_command {
    enum table_command_type type;
    uintptr_t key;
} table_command;

/*static hyper_table *local_table = NULL;

void init_hypertable_tests(void) {
    local_table = __cilkrts_local_hyper_table_alloc();
}

void fini_hypertable_tests(void) {
    local_hyper_table_free(local_table);
    local_table = NULL;
}

TestSuite(local_hypertable, .init = init_hypertable_tests, .fini = fini_hypertable_tests);*/

// Check the entries of a hyper_table to verify that key appears exactly
// expected_count times.  Optionally print the entries of hyper_table.
void check_hypertable(hyper_table *table, uintptr_t key, int32_t expected_count) {
    int32_t key_count = 0;
    int32_t capacity = table->capacity;
    struct bucket *buckets = table->buckets;
    PRINT_TRACE("table(%p): cap %d, occ %d, ins_rm %d\n", buckets, capacity,
                table->occupancy, table->ins_rm_count);
    if (capacity < MIN_HT_CAPACITY) {
        int32_t occupancy = table->occupancy;
        for (int32_t i = 0; i < occupancy; ++i) {
            PRINT_TRACE("table(%p)[%d] = { 0x%lx, %p }\n", buckets, i,
                        buckets[i].key, buckets[i].value.view);
            if (is_valid(key) && buckets[i].key == key)
                key_count++;
        }

        cr_assert(eq(sz, key_count, expected_count),
                "ERROR: Unexpected count for key 0x%lx!\n", key);

        return;
    }

    for (int32_t i = 0; i < capacity; ++i) {
        PRINT_TRACE("table(%p)[%d] = { 0x%lx, %p }\n", buckets, i,
                    buckets[i].key,
                    is_valid(buckets[i].key) ? buckets[i].value.view : NULL);
        if (is_valid(key) && buckets[i].key == key)
            key_count++;
    }

    cr_assert(eq(sz, key_count, expected_count),
            "ERROR: Unexpected count for key 0x%lx!\n", key);
}

// Parse and execute a table_command on a hyper_table.
void do_table_command(hyper_table *table, table_command cmd) {
    switch (cmd.type) {
    case TABLE_INSERT: {
        PRINT_TRACE("INSERT 0x%lx\n", cmd.key);
        bool success = insert_hyperobject(
            table, (struct bucket){
                       .key = cmd.key,
                       .value = {.view = (void *)cmd.key, .reduce_fn = NULL}});
        cr_assert(success, "insert_hyperobject failed");
        check_hypertable(table, cmd.key, 1);
        break;
    }
    case TABLE_LOOKUP: {
        // TODO: Implement this.
        break;
    }
    case TABLE_DELETE: {
        PRINT_TRACE("DELETE 0x%lx\n", cmd.key);
        bool success = remove_hyperobject(table, cmd.key);
        cr_assert(success, "remove_hyperobject failed");
        check_hypertable(table, cmd.key, 0);
        break;
    }
    }
}

// Simple test routine to insert and remove elements from a hyper_table,
// according to the given list of table_commands.
void test_insert_remove(const table_command *commands, int num_commands) {
    hyper_table *table = __cilkrts_local_hyper_table_alloc();
    for (int i = 0; i < num_commands; ++i) {
        do_table_command(table, commands[i]);
    }
    local_hyper_table_free(table);
}

ParameterizedTestParameters(local_hypertable, single_insert) {
    static int params[] = {
        0x42, 0xdeadbeef, 0x1, 0x2a    
    };

    size_t nb_params = sizeof (params) / sizeof (params[0]);
    return cr_make_param_array(int, params, nb_params);
}

ParameterizedTest(int *value_to_insert, local_hypertable, single_insert) {
    table_command cmd_sequence[] = {
        {TABLE_INSERT, *value_to_insert},
    };

    test_insert_remove(cmd_sequence, sizeof(cmd_sequence)/sizeof(table_command));
}

ParameterizedTestParameters(local_hypertable, cannot_insert_reserved_vals) {
    static uintptr_t params[] = {
        KEY_EMPTY, KEY_DELETED
    };

    size_t nb_params = sizeof (params) / sizeof (params[0]);
    return cr_make_param_array(uintptr_t, params, nb_params);
}

ParameterizedTest(uintptr_t *value_to_insert, local_hypertable, cannot_insert_reserved_vals, .signal = SIGABRT) {
    table_command cmd_sequence[] = {
        {TABLE_INSERT, *value_to_insert},
    };

    test_insert_remove(cmd_sequence, sizeof(cmd_sequence)/sizeof(table_command));
}

ParameterizedTestParameters(local_hypertable, insert_then_remove) {
    static int params[] = {
        0xdeadbeef, 0xcafef00d, 0xfeed3e, 0x1
    };

    size_t nb_params = sizeof (params) / sizeof (params[0]);
    return cr_make_param_array(int, params, nb_params);
}

ParameterizedTest(int *value_to_insert, local_hypertable, insert_then_remove) {
    table_command cmd_sequence[] = {
        {TABLE_INSERT, *value_to_insert},
        {TABLE_DELETE, *value_to_insert},
    };

    test_insert_remove(cmd_sequence, sizeof(cmd_sequence)/sizeof(table_command));
}

typedef struct int_array {
    size_t size;
    int *arr;
} int_array;

void cleanup_int_array(struct criterion_test_params *ctp) {
    cr_free(((struct int_array *) ctp->params)->arr);
}

ParameterizedTestParameters(local_hypertable, insert_remove_with_inserts_between) {
    static int_array params[] = {
        { .size = 2, .arr = NULL },
        { .size = 3, .arr = NULL }, 
        { .size = 6, .arr = NULL },
        { .size = 9, .arr = NULL },
    };

    size_t nb_params = sizeof (params) / sizeof (params[0]);

    for (int i = 0; i < (int) nb_params; ++i) {
        params[i].arr = (int*) cr_malloc(params[i].size * sizeof(int));
    }

    int array_0[] = {0x123456, 0x864210};
    memcpy(params[0].arr, array_0, params[0].size * sizeof(int));

    int array_1[] = {0xa991e5, 0x56713295, 0x1};
    memcpy(params[1].arr, array_1, params[1].size * sizeof(int));

    int array_2[] = {0x56713295, 0x1, 0x123456, 0xa991e5, 0xf00dcafe, 0x93};
    memcpy(params[2].arr, array_2, params[2].size * sizeof(int));

    int array_3[] = {0x9857, 0x134809, 0x137, 0x10984, 0x923498, 0x823098, 0x56, 0x78, 0x10000};
    memcpy(params[3].arr, array_3, params[3].size * sizeof(int));

    return cr_make_param_array(int_array, params, nb_params, cleanup_int_array);
}

ParameterizedTest(int_array *arr, local_hypertable, insert_remove_with_inserts_between) {
    // Number to be inserted from the array, plus 1 deletion
    int sequence_size = arr->size + 1;

    table_command *cmd_sequence = (table_command*) malloc(sequence_size * sizeof(table_command));

    // Fill in all the values to be inserted
    for (int i = 0; i < (int) arr->size; ++i) {
        cmd_sequence[i].type = TABLE_INSERT;
        cmd_sequence[i].key = arr->arr[i];
    }

    // Delete only the first item
    cmd_sequence[arr->size].type = TABLE_DELETE;
    cmd_sequence[arr->size].key = arr->arr[0];

    test_insert_remove(cmd_sequence, sequence_size);
}

Test(local_hypertable, multiple_insert) {
    {
        table_command cmd_sequence[] = {
            {TABLE_INSERT, 0x12345},
            {TABLE_INSERT, 0x12345},
            {TABLE_INSERT, 0x12345},
            {TABLE_INSERT, 0x12345},
        };

        test_insert_remove(cmd_sequence, sizeof(cmd_sequence)/sizeof(table_command));
    }
}

Test(local_hypertable, multiple_delete_fails) {
    {
        table_command cmd_sequence[] = {
            {TABLE_INSERT, 0x12345},
            {TABLE_INSERT, 0x12345},
            {TABLE_INSERT, 0x12345},
            {TABLE_INSERT, 0x12345},
        };

        test_insert_remove(cmd_sequence, sizeof(cmd_sequence)/sizeof(table_command));
    }
}

Test(local_hypertable, simple_trace) {
    // Simple test case
    table_command cmd_sequence[] = {
        {TABLE_INSERT, 0x1},
        {TABLE_INSERT, 0x2},
        {TABLE_INSERT, 0x3},
        {TABLE_INSERT, 0x4},
        {TABLE_INSERT, 0x5},
        {TABLE_INSERT, 0x6},
        {TABLE_INSERT, 0x7},
        {TABLE_INSERT, 0x8},
        {TABLE_INSERT, 0x9},
        {TABLE_INSERT, 0xa},
        {TABLE_INSERT, 0xb},
        {TABLE_INSERT, 0xc},
        {TABLE_INSERT, 0xd},
        {TABLE_INSERT, 0xe},
        {TABLE_INSERT, 0xf},

        {TABLE_DELETE, 0x1},
        {TABLE_INSERT, 0x1},

        {TABLE_DELETE, 0x1},
        {TABLE_DELETE, 0x2},
        {TABLE_DELETE, 0x3},
        {TABLE_DELETE, 0x4},
        {TABLE_DELETE, 0x5},
        {TABLE_DELETE, 0x6},
        {TABLE_DELETE, 0x7},
        {TABLE_DELETE, 0x8},
        {TABLE_DELETE, 0x9},
        {TABLE_DELETE, 0xa},
        {TABLE_DELETE, 0xb},
        {TABLE_DELETE, 0xc},
        {TABLE_DELETE, 0xd},
        {TABLE_DELETE, 0xe},
        {TABLE_DELETE, 0xf},
    };
    test_insert_remove(cmd_sequence, sizeof(cmd_sequence)/sizeof(table_command));
}

// Test case derived from trace that led to errors.
Test(local_hypertable, tricky_trace) {
    table_command cmd_sequence[] = {
        {TABLE_INSERT, 0x7f2a10bfe050},
        {TABLE_INSERT, 0x7f2a10bff968},
        {TABLE_INSERT, 0x7f2a10bfe8a8},
        {TABLE_INSERT, 0x7f2a10bfece0},
        {TABLE_INSERT, 0x7f2a10bff538},
        {TABLE_INSERT, 0x7f2a10bff108},
        {TABLE_INSERT, 0x7f2a10bff540},
        {TABLE_INSERT, 0x7f2a10bff970},
        {TABLE_INSERT, 0x7f2a10bfe8b0},
        {TABLE_INSERT, 0x7f2a10bfe478},
        {TABLE_INSERT, 0x7f2a10bfe480},
        {TABLE_INSERT, 0x7f2a10bff110},
        {TABLE_INSERT, 0x7f2a10bffda0},
        {TABLE_INSERT, 0x562edc97d0c0},
        {TABLE_INSERT, 0x7f2a10bfe048},

        {TABLE_INSERT, 0x7f2a10bfe478},
        {TABLE_INSERT, 0x7f2a10bff110},

        {TABLE_DELETE, 0x7f2a10bfe048},
        {TABLE_DELETE, 0x7f2a10bfe050},
        {TABLE_DELETE, 0x7f2a10bfe478},
        {TABLE_DELETE, 0x7f2a10bfe480},
        {TABLE_DELETE, 0x7f2a10bfe8a8},
        {TABLE_DELETE, 0x7f2a10bfe8b0},

        {TABLE_INSERT, 0x7f2a10bfe8b0},
        {TABLE_INSERT, 0x7f2a10bfe8a8},
        {TABLE_INSERT, 0x7f2a10bfe480},
        {TABLE_INSERT, 0x7f2a10bfe478},
        {TABLE_INSERT, 0x7f2a10bfe050},
        {TABLE_INSERT, 0x7f2a10bfe048},

        {TABLE_DELETE, 0x7f2a10bfe048},
        {TABLE_DELETE, 0x7f2a10bfe050},
        {TABLE_DELETE, 0x7f2a10bfe478},
        {TABLE_DELETE, 0x7f2a10bfe480},

        {TABLE_INSERT, 0x7f2a10bfe480},
        {TABLE_INSERT, 0x7f2a10bfe478},

        {TABLE_INSERT, 0x7f2a10bfe480},
    };
    test_insert_remove(cmd_sequence, sizeof(cmd_sequence)/sizeof(table_command));
}
