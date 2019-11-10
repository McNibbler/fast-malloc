#ifndef IVEC_H
#define IVEC_H

#include <assert.h>

#include "xmalloc.h"

typedef struct ivec {
    long  cap;
    long  size;
    long* data;
} ivec;

static
ivec*
make_ivec(int cap0)
{
    assert(cap0 > 0);

    ivec* xs = xmalloc(sizeof(ivec));
    xs->cap  = cap0;
    xs->size = 0;
    xs->data = xmalloc(xs->cap * sizeof(long));
    return xs;
}

static
void
free_ivec(ivec* xs)
{
    xfree(xs->data);
    xfree(xs);
}

static
void
ivec_push(ivec* xs, long item)
{
    if (xs->size >= xs->cap) {
        xs->cap *= 2;
        xs->data = xrealloc(xs->data, xs->cap * sizeof(long));
    }

    xs->data[xs->size] = item;
    xs->size += 1;
}

static
long
ivec_last(ivec* xs)
{
    return xs->data[xs->size - 1];
}

static
ivec*
ivec_copy(ivec* xs)
{
    ivec* ys = make_ivec(xs->cap);
    for (long ii = 0; ii < xs->size; ++ii) {
        ivec_push(ys, xs->data[ii]);
    }
    return ys;
}

#endif
