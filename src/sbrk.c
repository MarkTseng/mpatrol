/*
 * mpatrol
 * A library for controlling and tracing dynamic memory allocations.
 * Copyright (C) 1997-2000 Graeme S. Roy <graeme@epc.co.uk>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the Free
 * Software Foundation, Inc., 59 Temple Place, Suite 330, Boston,
 * MA 02111-1307, USA.
 */


/*
 * Simulated UNIX heap.  This implements the brk() and sbrk() functions on
 * non-UNIX platforms by allocating a single block of memory for a simulated
 * heap from the low-level memory allocator.  These functions are not normally
 * used by the mpatrol library and are only defined to resolve any references
 * to them from user code.
 */


#include "sbrk.h"
#include "memory.h"
#include <stdlib.h>


/* The brkinfo structure contains information about the simulated heap.
 */

typedef struct brkinfo
{
    meminfo memory; /* memory details */
    void *block;    /* pointer to block of memory */
    size_t size;    /* size of block of memory */
    size_t len;     /* current break length */
}
brkinfo;


#if MP_IDENT_SUPPORT
#ident "$Id: sbrk.c,v 1.1 2000-03-23 19:10:26 graeme Exp $"
#endif /* MP_IDENT_SUPPORT */


#ifdef __cplusplus
extern "C"
{
#endif /* __cplusplus */


/* The break header structure used to contain information about the heap.
 */

static brkinfo brkhead;


/* Finalise the break header structure so that the heap becomes empty.
 */

static void finibrk(void)
{
    if (brkhead.block != NULL)
        __mp_memfree(&brkhead.memory, brkhead.block, brkhead.size);
    __mp_endmemory(&brkhead.memory);
    brkhead.block = NULL;
    brkhead.size = 0;
    brkhead.len = 0;
}


/* Initialise the fields of the break header structure so that the heap
 * becomes empty.
 */

static int initbrk(void)
{
    size_t l;

    l = MP_BREAK_SIZE;
    __mp_newmemory(&brkhead.memory);
    if ((brkhead.block = __mp_memalloc(&brkhead.memory, &l,
          brkhead.memory.align)) == NULL)
        return 0;
    brkhead.size = l;
    brkhead.len = 0;
#if MP_DELETE
    atexit(finibrk);
#endif /* MP_DELETE */
    return 1;
}


/* Set the address of the first byte beyond the end of the heap.
 */

int brk(void *p)
{
    if (((brkhead.block != NULL) || initbrk()) && (p >= brkhead.block) &&
        ((char *) p <= (char *) brkhead.block + brkhead.size))
    {
        brkhead.len = (char *) p - (char *) brkhead.block;
        return 0;
    }
    return -1;
}


/* Increase or decrease the amount of available heap space.
 */

void *sbrk(int l)
{
    void *p;

    p = (void *) -1;
    if ((brkhead.block != NULL) || initbrk())
        if (l > 0)
        {
            if (brkhead.len + l <= brkhead.size)
            {
                p = (char *) brkhead.block + brkhead.len;
                brkhead.len += l;
            }
        }
        else if (l < 0)
        {
            if (brkhead.len >= -l)
            {
                p = (char *) brkhead.block + brkhead.len;
                brkhead.len += l;
            }
        }
        else
            p = (char *) brkhead.block + brkhead.len;
    return p;
}


#ifdef __cplusplus
}
#endif /* __cplusplus */
