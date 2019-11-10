#ifndef LIST_H
#define LIST_H

#include "xmalloc.h"

// Linked list cell.
typedef struct cell {
    long         item;
    struct cell* rest;
} cell;

static
cell*
cons(long item, cell* rest)
{
    cell* xs = xmalloc(sizeof(cell));
    xs->item = item;
    xs->rest = rest;
    return xs;
}

static
long
count_list(cell* xs)
{
    long nn = 0;
    while (xs) {
        nn++;
        xs = xs->rest;
    }
    return nn;
}

static
void
free_list(cell* xs)
{
    while (xs) {
        cell* ys = xs->rest;
        xfree(xs);
        xs = ys;
    }
}

static
cell*
copy_list(cell* xs)
{
    if (xs == 0) {
        return 0;
    }

    cell* ys = copy_list(xs->rest);
    return cons(xs->item, ys);
}

#endif

