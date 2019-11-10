
// The Collatz conjecture:
//
// If we start with some number n and iterate the following:
// - If x is even, n -> n/2
// - If x is odd,  n -> 3*n + 1
// We'll eventually get to 1.

// This program searches for the largest number of steps that
// this takes for numbers from 2 to a provided TOP number.

// To calculate this:
//  - calculate the entire sequence for each starting value
//    using multiple threads.
//  - calculate the length of the sequence 
// Next

#include <stdio.h>
#include <pthread.h>
#include <assert.h>
#include <unistd.h>
#include <stdlib.h>

#include "xmalloc.h"
#include "ivec.h"

#define THREADS 4

typedef struct num_task {
    ivec* vals;
    long  steps;
    int   dibs;
    pthread_mutex_t lock;
} num_task;

num_task** tasks;
long data_top = 0;

long
collatz_step(long n)
{
    if (n % 2 == 0) {
        return n/2;
    }
    else {
        return 3*n + 1;
    }
}

ivec*
iterate(ivec* xs)
{
    long vv = 0;
    for (int jj = 0; vv != 1 && jj < 50; ++jj) {
        vv = collatz_step(ivec_last(xs));
        ivec_push(xs, vv);
    }
    return xs;
}

int
scan_and_iterate()
{
    long done_count = 0;
    long base = random() % data_top;

    for (long i0 = 1; i0 < data_top; ++i0) {
        long ii = 1 + (base + i0) % (data_top - 1);

        pthread_mutex_lock(&(tasks[ii]->lock));
        int skip = tasks[ii]->dibs;
        if (!skip) {
            tasks[ii]->dibs = 1;
        }
        pthread_mutex_unlock(&(tasks[ii]->lock));
        if (skip) {
            continue;
        }

        ivec* xs = tasks[ii]->vals;
        long vv = ivec_last(xs);

        if (vv > 1) {
            xs = ivec_copy(xs);
            xs = iterate(xs);
            free_ivec(tasks[ii]->vals);
            tasks[ii]->vals = xs;
        }
        else {
            if (tasks[ii]->steps == -1) {
                tasks[ii]->steps = tasks[ii]->vals->size - 1;
            }

            done_count += 1;
        }

        pthread_mutex_lock(&(tasks[ii]->lock));
        tasks[ii]->dibs = 0;
        pthread_mutex_unlock(&(tasks[ii]->lock));
    }

    return done_count == (data_top - 1);
}

void*
worker(void* _arg)
{
    int done = 0;
    while (!done) {
        done = scan_and_iterate();
    }
    return 0;
}

int
main(int argc, char* argv[])
{
    pthread_t threads[THREADS];
    int rv;

    if (argc != 2) {
        printf("Usage:\n");
        printf("\t%s TOP\n", argv[0]);
        return 1;
    }

    data_top  = atol(argv[1]);

    tasks = xmalloc(data_top * sizeof(num_task*));
    for (int ii = 0; ii < data_top; ++ii) {
        tasks[ii] = xmalloc(sizeof(num_task));
        ivec* xs = make_ivec(4);
        ivec_push(xs, ii);
        tasks[ii]->vals  = xs;
        tasks[ii]->steps = -1;
        tasks[ii]->dibs  = 0;
        pthread_mutex_init(&(tasks[ii]->lock), 0);
    }

    for (int ii = 0; ii < THREADS; ++ii) {
        rv = pthread_create(&(threads[ii]), 0, worker, 0);
        assert(rv == 0);
    }

    for (int ii = 0; ii < THREADS; ++ii) {
        rv = pthread_join(threads[ii], 0);
        assert(rv == 0);
    }

    long max_v = 0;
    long max_s = 0;

    for (int ii = 0; ii < data_top; ++ii) {
        if (tasks[ii]->steps > max_s) {
            max_v = ii;
            max_s = tasks[ii]->steps;
        }
    }

    printf("Max steps is at %ld: %ld steps\n", max_v, max_s);

    for (int ii = 0; ii < data_top; ++ii) {
        free_ivec(tasks[ii]->vals);
        xfree(tasks[ii]);
    }
    xfree(tasks);

    return 0;
}

