/*
 * mpatrol
 * A library for controlling and tracing dynamic memory allocations.
 * Copyright (C) 1997-2001 Graeme S. Roy <graeme@epc.co.uk>
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
 * Library interface.  This module defines the visible interface for the
 * mpatrol library.  It also defines replacement functions for the GNU
 * Checker tool, which adds code to check all reads from and writes to
 * memory when the user's code is compiled with the -fcheck-memory-usage
 * option in the GNU compiler.
 */


#include "inter.h"
#include "diag.h"
#include "machine.h"
#if MP_THREADS_SUPPORT
#include "mutex.h"
#endif /* MP_THREADS_SUPPORT */
#if (TARGET == TARGET_AMIGA && defined(__GNUC__)) || \
    (TARGET == TARGET_WINDOWS && !defined(__GNUC__))
#include "sbrk.h"
#endif /* TARGET && __GNUC__ */
#include "option.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>


#if MP_IDENT_SUPPORT
#ident "$Id: inter.c,v 1.95 2001-03-01 00:54:16 graeme Exp $"
#else /* MP_IDENT_SUPPORT */
static MP_CONST MP_VOLATILE char *inter_id = "$Id: inter.c,v 1.95 2001-03-01 00:54:16 graeme Exp $";
#endif /* MP_IDENT_SUPPORT */


#ifdef __cplusplus
extern "C"
{
#endif /* __cplusplus */


#if MP_INUSE_SUPPORT
int _Inuse_init(unsigned long, int);
void _Inuse_close(void);
#endif /* MP_INUSE_SUPPORT */


/* The memory header structure used by the library to hold all of its data
 * and settings.
 */

static infohead memhead;


#if TARGET == TARGET_WINDOWS && !defined(__GNUC__)
/* These are global variables used by the Microsoft C run-time library to
 * indicate initialisation of the environment variables, the exit function
 * table and the streams buffers respectively.  The run-time library calls
 * malloc() and related functions to set these up so we cannot initialise
 * the mpatrol library until all of these have been set up.  Instead, we
 * will call sbrk() to allocate enough memory for these as they appear, but
 * we cannot record anything about these allocations.
 */

extern int __env_initialized;
extern void *__onexitbegin;
extern void **__piob;


/* Determine if the Microsoft C run-time library is initialised.
 */

static
int
msvcrt_initialised(void)
{
    if (__env_initialized && __onexitbegin && __piob)
        return 1;
    return 0;
}
#endif /* TARGET && __GNUC__ */


#if MP_INIT_SUPPORT
/* On systems with .init and .fini sections we can plant calls to initialise
 * the mpatrol mutexes and data structures before main() is called and also
 * to terminate the mpatrol library after exit() is called.  However, we need
 * to refer to a symbol within the machine module so that we can drag in the
 * contents of the .init and .fini sections when the mpatrol library is built
 * as an archive library.
 */

static int *initsection = &__mp_initsection;
#elif defined(__GNUC__)
/* The GNU C compiler allows us to indicate that a function is a constructor
 * which should be called before main() or that a function is a destructor
 * that should be called at or after exit().  However, these get entered into
 * the .ctors and .dtors sections which means that the final link must also
 * be performed by the GNU C compiler.
 */

static void initlibrary(void) __attribute__((__constructor__));

static
void
initlibrary(void)
{
#if MP_THREADS_SUPPORT
    __mp_initmutexes();
#endif /* MP_THREADS_SUPPORT */
    __mp_init();
}

#if !MP_USE_ATEXIT
static void finilibrary(void) __attribute__((__destructor__));

static
void
finilibrary(void)
{
    __mp_fini();
}
#endif /* MP_USE_ATEXIT */
#elif defined(__cplusplus)
/* C++ provides a way to initialise the mpatrol library before main() is
 * called and terminate it after exit() is called.  Note that if the C++
 * compiler uses a special section for calling functions before main() that
 * is not recognised by the system tools then the C++ compiler must also be
 * used to perform the final link.
 */

static struct initlibrary
{
    initlibrary()
    {
#if MP_THREADS_SUPPORT
        __mp_initmutexes();
#endif /* MP_THREADS_SUPPORT */
        __mp_init();
    }
#if !MP_USE_ATEXIT
    ~initlibrary()
    {
        __mp_fini();
    }
#endif /* MP_USE_ATEXIT */
}
meminit;
#endif /* __cplusplus */


/* Save the current signal handlers and set them to ignore.  Also lock the
 * library data structures if we are thread-safe.
 */

static
void
savesignals(void)
{
#if MP_THREADS_SUPPORT
    __mp_lockmutex(MT_MAIN);
#endif /* MP_THREADS_SUPPORT */
    /* Only perform this step if we are not doing a recursive call.
     */
    if (memhead.recur++ == 0)
    {
        if (!memhead.init)
            __mp_initsignals(&memhead.signals);
        if (memhead.flags & FLG_SAFESIGNALS)
            __mp_savesignals(&memhead.signals);
    }
}


/* Restore the previous signal handlers.  Also unlock the library data
 * structures if we are thread-safe.
 */

static
void
restoresignals(void)
{
    /* Only perform this step if we are not doing a recursive call.
     */
    if (--memhead.recur == 0)
        __mp_restoresignals(&memhead.signals);
#if MP_THREADS_SUPPORT
    __mp_unlockmutex(MT_MAIN);
#endif /* MP_THREADS_SUPPORT */
}


/* Check the validity of all memory blocks, but only if the allocation count
 * is within range and the event count is a multiple of the heap checking
 * frequency.
 */

static
void
checkheap(loginfo *v, unsigned long n)
{
    unsigned long l;

    /* If the lower and upper bounds are zero then we never need to check
     * the heap.
     */
    if ((l = memhead.lrange) || (memhead.urange != 0))
    {
        if (l == (unsigned long) -1)
            l = 0;
        if ((l <= n) && (n <= memhead.urange) &&
            ((memhead.check == 1) || (memhead.event % memhead.check == 0)))
            __mp_checkinfo(&memhead, v);
    }
}


/* Check the alloca allocation stack for any allocations that should be freed.
 */

static
void
checkalloca(loginfo *i, int f)
{
    allocanode *n, *p;
#if MP_FULLSTACK
    addrnode *a, *b;
    stackcompare r;
#endif /* MP_FULLSTACK */
    int c;

    if (memhead.fini || (memhead.astack.size == 0))
        return;
#if MP_FULLSTACK
    /* Create the address nodes for the current call.  This is not necessarily
     * an efficient way of doing call stack comparisons if the alloca allocation
     * stack does not contain many outstanding entries, but on some platforms
     * call stack traversal is an expensive business.
     */
    if (!(memhead.flags & FLG_NOPROTECT))
        __mp_protectaddrs(&memhead.addr, MA_READWRITE);
    a = __mp_getaddrs(&memhead.addr, i->stack);
    if (!(memhead.flags & FLG_NOPROTECT))
        __mp_protectaddrs(&memhead.addr, MA_READONLY);
#endif /* MP_FULLSTACK */
    for (n = (allocanode *) memhead.astack.head;
         p = (allocanode *) n->node.next; n = p)
    {
        c = 0;
        if (f == 1)
            c = 1;
#if MP_FULLSTACK
        /* We need to compare the call stacks of the two calling functions in
         * order to see if we can free the allocation.  Currently, this just
         * involves ensuring that the call stacks are different or that the
         * current call stack is shallower than that which made the original
         * allocation.  In theory we could also free the allocation if the
         * current call stack was deeper and the initial addresses differ, but
         * that would involve a lot more calculations.
         */
        else
        {
            b = (addrnode *) n->data.frame;
            if ((b->data.next != NULL) && (a->data.next != NULL) &&
                (((r = __mp_compareaddrs(b->data.next, a->data.next)) ==
                  SC_DIFFERENT) || (r == SC_SHALLOWER)))
                c = 1;
        }
#else /* MP_FULLSTACK */
        /* We need to add an arbitrary number of bytes to the address of the
         * local variable in order to be completely safe, since the current
         * call may differ by one stack frame if the entry to the mpatrol
         * library was via a macro defined in mpatrol.h rather than a function
         * defined in malloc.c, or vice versa.  Unfortunately, this means that
         * we are going to be unable to free the allocation at the first
         * available point when it goes out of scope.
         */
        else if (memhead.alloc.heap.memory.stackdir < 0)
        {
            /* The stack grows down so we must ensure that the current local
             * variable pointer occupies a higher address than that which
             * made the original allocation if we are to free the allocation.
             */
            if ((char *) n->data.frame + 256 < (char *) &i->stack->frame)
                c = 1;
        }
        else
        {
            /* The stack grows up so we must ensure that the current local
             * variable pointer occupies a lower address than that which
             * made the original allocation if we are to free the allocation.
             */
            if ((char *) n->data.frame > (char *) &i->stack->frame + 256)
                c = 1;
        }
#endif /* MP_FULLSTACK */
        if (c == 1)
        {
            if (memhead.prologue && (memhead.recur == 1))
                memhead.prologue(n->block, (size_t) -1, i->stack->addr);
            memhead.event++;
            __mp_freememory(&memhead, n->block, AT_ALLOCA, i);
            if (memhead.epilogue && (memhead.recur == 1))
                memhead.epilogue((void *) -1, i->stack->addr);
        }
    }
#if MP_FULLSTACK
    /* Free the address nodes for the current call.  They may well have to be
     * generated again but that can't be guaranteed.
     */
    if (a != NULL)
    {
        if (!(memhead.flags & FLG_NOPROTECT))
            __mp_protectaddrs(&memhead.addr, MA_READWRITE);
        __mp_freeaddrs(&memhead.addr, a);
        if (!(memhead.flags & FLG_NOPROTECT))
            __mp_protectaddrs(&memhead.addr, MA_READONLY);
    }
#endif /* MP_FULLSTACK */
}


/* Initialise the mpatrol library.
 */

void
__mp_init(void)
{
    size_t i;

    savesignals();
    if (memhead.fini)
        /* We currently don't allow the library to be reinitialised.
         */
        __mp_abort();
    if (!memhead.init)
    {
        __mp_newinfo(&memhead);
#if (TARGET == TARGET_AMIGA && defined(__GNUC__))
        /* Initialise the simulated UNIX heap.  This gets done anyway, but it
         * forces a reference to the sbrk module which is necessary since
         * we need it for libiberty, which gets linked after libmpatrol.
         */
        sbrk(0);
#endif /* TARGET && __GNUC__ */
#if MP_INUSE_SUPPORT
        _Inuse_init(0, 0);
#endif /* MP_INUSE_SUPPORT */
#if MP_USE_ATEXIT
        /* We will have to terminate the library using atexit() since either
         * this has been explicitly specified or there is no support for
         * .init/.fini functions, constructor/destructor functions or C++.
         * This is usually OK to do but there may be problems if the mpatrol
         * library is terminated before all memory allocations are freed.
         */
        atexit(__mp_fini);
#endif /* MP_USE_ATEXIT */
        /* Read any options from the specified environment variable.
         */
        __mp_parseoptions(&memhead);
        /* Set up the random number generator for the FAILFREQ option.
         */
        if (memhead.fseed == 0)
            memhead.fseed = (unsigned long) time(NULL);
        srand((unsigned int) memhead.fseed);
        /* Attempt to open the log file.
         */
        if (!__mp_openlogfile(memhead.log))
            memhead.log = NULL;
        /* Output the header and version information.
         */
        __mp_printversion();
        /* Attempt to read symbols from any shared libraries and possibly
         * determine the program name if it has not already been set.
         */
        __mp_addextsymbols(&memhead.syms, &memhead.alloc.heap.memory);
        /* Obtain the program filename and attempt to read any symbols from
         * that file.
         */
        if (memhead.alloc.heap.memory.prog != NULL)
            __mp_addsymbols(&memhead.syms, memhead.alloc.heap.memory.prog, NULL,
                            0);
        __mp_fixsymbols(&memhead.syms);
        __mp_diag("\n");
        if (!(memhead.flags & FLG_NOPROTECT))
        {
            __mp_protectstrtab(&memhead.syms.strings, MA_READONLY);
            __mp_protectsymbols(&memhead.syms, MA_READONLY);
            __mp_protectinfo(&memhead, MA_READONLY);
        }
        /* Finally, call any initialisation functions in the order in which
         * they were registered.
         */
        for (i = 0; i < memhead.initcount; i++)
        {
            memhead.inits[i]();
            memhead.inits[i] = NULL;
        }
        memhead.initcount = 0;
    }
    restoresignals();
}


/* Finalise the mpatrol library.
 */

void
__mp_fini(void)
{
    stackinfo i;
    loginfo v;

    savesignals();
    if (memhead.init)
    {
        if (!memhead.fini)
        {
            /* First, determine the call stack details in case we need to
             * free any remaining allocations that were made by alloca().
             */
            __mp_newframe(&i, NULL);
            if (__mp_getframe(&i))
                __mp_getframe(&i);
            v.func = NULL;
            v.file = NULL;
            v.line = 0;
            v.stack = &i;
            v.typestr = NULL;
            v.typesize = 0;
            __mp_checkinfo(&memhead, &v);
            checkalloca(&v, 1);
            /* Then call any finalisation functions in the reverse order in
             * which they were registered.
             */
            while (memhead.finicount > 0)
            {
                memhead.finis[--memhead.finicount]();
                memhead.finis[memhead.finicount] = NULL;
            }
            /* Then close any access library handles that might still be open.
             */
            __mp_closesymbols(&memhead.syms);
            /* Then print a summary of library statistics and settings.
             */
            __mp_printsummary(&memhead);
            /* Then deal with any SHOW options that may have been requested.
             */
            if ((memhead.flags & FLG_SHOWMAP) && (memhead.alloc.list.size > 0))
                __mp_printmap(&memhead);
            if ((memhead.flags & FLG_SHOWSYMBOLS) &&
                (memhead.syms.dtree.size > 0))
                __mp_printsymbols(&memhead.syms);
            if ((memhead.flags & FLG_SHOWFREE) && (memhead.alloc.fsize > 0))
                __mp_printfree(&memhead);
            if ((memhead.flags & FLG_SHOWFREED) && (memhead.alloc.gsize > 0))
                __mp_printfreed(&memhead);
            if (memhead.alloc.asize > 0)
            {
                if (memhead.flags & FLG_SHOWUNFREED)
                    __mp_printallocs(&memhead, 0);
                if ((memhead.uabort > 0) &&
                    (memhead.alloc.asize >= memhead.uabort))
                    __mp_printallocs(&memhead, 1);
            }
            /* Next, close the tracing output file.  This will do nothing if
             * tracing has not been enabled.
             */
            __mp_endtrace(&memhead.trace);
            /* Finally, write out any profiling information to the profiling
             * output file.
             */
            if (memhead.prof.autocount > 0)
                __mp_writeprofile(&memhead.prof,
                                  !(memhead.flags & FLG_NOPROTECT));
            memhead.fini = 1;
#if MP_INUSE_SUPPORT
            _Inuse_close();
#endif /* MP_INUSE_SUPPORT */
        }
#if MP_DELETEHEAP
        /* We only need to perform this step if the operating system does not
         * reclaim memory from a terminated process.  We must not perform this
         * step if the operating system needs to deal with dynamically
         * allocated memory after the library has terminated.
         */
        if (!(memhead.flags & FLG_NOPROTECT))
        {
            __mp_protectinfo(&memhead, MA_READWRITE);
            __mp_protectsymbols(&memhead.syms, MA_READWRITE);
            __mp_protectstrtab(&memhead.syms.strings, MA_READWRITE);
        }
        __mp_deleteinfo(&memhead);
#if MP_THREADS_SUPPORT
        __mp_finimutexes();
#endif /* MP_THREADS_SUPPORT */
#endif /* MP_DELETEHEAP */
        memhead.init = 0;
    }
    restoresignals();
}


/* Register a finalisation function to be called when __mp_fini() is called.
 */

int
__mp_atexit(void (*f)(void))
{
    int r;

    savesignals();
    if (!memhead.init)
        __mp_init();
    if (memhead.finicount == MP_MAXFINIS)
        r = 0;
    else
    {
        memhead.finis[memhead.finicount++] = f;
        r = 1;
    }
    restoresignals();
    return r;
}


/* Set an mpatrol option after the library has been initialised.
 */

unsigned long
__mp_setoption(long o, unsigned long v)
{
    unsigned long r;

    savesignals();
    if (!memhead.init)
        __mp_init();
    if (o > 0)
        r = 1;
    else
    {
        o = -o;
        if (!(memhead.flags & FLG_NOPROTECT))
            __mp_protectinfo(&memhead, MA_READWRITE);
        if ((r = __mp_set(&memhead, (unsigned long) o, v)) &&
            (o != OPT_SETFLAGS) && (o != OPT_UNSETFLAGS))
            r = 1;
        if ((memhead.recur == 1) && !(memhead.flags & FLG_NOPROTECT))
            __mp_protectinfo(&memhead, MA_READONLY);
    }
    restoresignals();
    return r;
}


/* Get an mpatrol option after the library has been initialised.
 */

int
__mp_getoption(long o, unsigned long *v)
{
    int r;

    savesignals();
    if (!memhead.init)
        __mp_init();
    if (o > 0)
        r = 0;
    else
    {
        o = -o;
        r = __mp_get(&memhead, (unsigned long) o, v);
    }
    restoresignals();
    return r;
}


/* Return the memory header structure.
 */

infohead *
__mp_memhead(void)
{
    return &memhead;
}


/* Allocate a new block of memory of a specified size and alignment.
 */

void *
__mp_alloc(size_t l, size_t a, alloctype f, char *s, char *t, unsigned long u,
           char *g, size_t h, size_t k)
{
    void *p;
    stackinfo i;
    loginfo v;
    int j, z;

#if TARGET == TARGET_WINDOWS && !defined(__GNUC__)
    /* If the C run-time library has not finished initialising then we must
     * allocate new memory with sbrk().  We don't even attempt to do anything
     * with calls to memalign(), valloc() and pvalloc() but these shouldn't
     * be coming through anyway.
     */
    if (!msvcrt_initialised())
    {
        if (l == 0)
            l = 1;
        if ((p = sbrk(l)) == (void *) -1)
            if ((f == AT_ALLOCA) || (f == AT_XMALLOC) || (f == AT_XCALLOC))
                abort();
            else
                p = NULL;
        else if ((f == AT_CALLOC) || (f == AT_XCALLOC))
            __mp_memset(p, 0, l);
        return p;
    }
#endif /* TARGET && __GNUC__ */
    savesignals();
    if (!memhead.init)
        __mp_init();
    /* Determine the call stack details.
     */
    __mp_newframe(&i, NULL);
    if (__mp_getframe(&i))
    {
        j = __mp_getframe(&i);
        while ((k > 0) && (j != 0))
        {
            j = __mp_getframe(&i);
            k--;
        }
    }
    if (memhead.prologue && (memhead.recur == 1))
        memhead.prologue((void *) -1, l, i.addr);
    /* If no filename was passed through then attempt to read any debugging
     * information to determine the source location of the call.
     */
    if ((memhead.recur == 1) && (t == NULL) && (i.addr != NULL) &&
        __mp_findsource(&memhead.syms, (char *) i.addr - 1, &s, &t, &u))
    {
        if (!(memhead.flags & FLG_NOPROTECT))
            __mp_protectstrtab(&memhead.syms.strings, MA_READWRITE);
        if (s != NULL)
            s = __mp_addstring(&memhead.syms.strings, s);
        if (t != NULL)
            t = __mp_addstring(&memhead.syms.strings, t);
        if (!(memhead.flags & FLG_NOPROTECT))
            __mp_protectstrtab(&memhead.syms.strings, MA_READONLY);
    }
    v.func = s;
    v.file = t;
    v.line = u;
    v.stack = &i;
    v.typestr = g;
    v.typesize = h;
    checkheap(&v, memhead.count + 1);
    checkalloca(&v, 0);
    memhead.event++;
    if (((f == AT_XMALLOC) || (f == AT_XCALLOC)) && (l == 0) && (h != 0))
        l = h;
    z = 0;
  retry:
    p = __mp_getmemory(&memhead, l, a, f, &v);
    if (memhead.epilogue && (memhead.recur == 1))
        memhead.epilogue(p, i.addr);
    if (p == NULL)
    {
        if ((z == 0) && (memhead.nomemory))
        {
            /* Call the low-memory handler if no memory block was allocated.
             */
            memhead.nomemory();
            if (memhead.prologue && (memhead.recur == 1))
                memhead.prologue((void *) -1, l, i.addr);
            if ((f != AT_NEW) && (f != AT_NEWVEC))
                z = 1;
            goto retry;
        }
        if ((f == AT_ALLOCA) || (f == AT_XMALLOC) || (f == AT_XCALLOC))
        {
            /* The alloca(), xmalloc() and xcalloc() functions should always
             * return non-NULL pointers, which means we must abort here.
             */
            __mp_printsummary(&memhead);
            __mp_diag("\n");
            __mp_error(ET_OUTMEM, f, t, u, "out of memory");
            memhead.fini = 1;
            __mp_abort();
        }
    }
    restoresignals();
    return p;
}


/* Allocate a new block of memory to duplicate a string.
 */

char *
__mp_strdup(char *p, size_t l, alloctype f, char *s, char *t, unsigned long u,
            size_t k)
{
    char *o;
    stackinfo i;
    loginfo v;
    size_t n;
    int j, z;

#if TARGET == TARGET_WINDOWS && !defined(__GNUC__)
    /* If the C run-time library has not finished initialising then we must
     * allocate new memory with sbrk() and copy the string to the new
     * allocation.
     */
    if (!msvcrt_initialised())
    {
        if (p == NULL)
            o = NULL;
        else
        {
            n = strlen(p);
            if (((f == AT_STRNDUP) || (f == AT_STRNSAVE) ||
                 (f == AT_STRNDUPA)) && (n > l))
                n = l;
            if ((o = (char *) sbrk(n + 1)) == (void *) -1)
                o = NULL;
            else
            {
                __mp_memcopy(o, p, n);
                o[n] = '\0';
            }
        }
        if ((o == NULL) && ((f == AT_STRDUPA) || (f == AT_STRNDUPA) ||
             (f == AT_XSTRDUP)))
            abort();
        return o;
    }
#endif /* TARGET && __GNUC__ */
    savesignals();
    if (!memhead.init)
        __mp_init();
    /* Determine the call stack details.
     */
    __mp_newframe(&i, NULL);
    if (__mp_getframe(&i))
    {
        j = __mp_getframe(&i);
        while ((k > 0) && (j != 0))
        {
            j = __mp_getframe(&i);
            k--;
        }
    }
    if (memhead.prologue && (memhead.recur == 1))
        memhead.prologue(p, (size_t) -2, i.addr);
    /* If no filename was passed through then attempt to read any debugging
     * information to determine the source location of the call.
     */
    if ((memhead.recur == 1) && (t == NULL) && (i.addr != NULL) &&
        __mp_findsource(&memhead.syms, (char *) i.addr - 1, &s, &t, &u))
    {
        if (!(memhead.flags & FLG_NOPROTECT))
            __mp_protectstrtab(&memhead.syms.strings, MA_READWRITE);
        if (s != NULL)
            s = __mp_addstring(&memhead.syms.strings, s);
        if (t != NULL)
            t = __mp_addstring(&memhead.syms.strings, t);
        if (!(memhead.flags & FLG_NOPROTECT))
            __mp_protectstrtab(&memhead.syms.strings, MA_READONLY);
    }
    v.func = s;
    v.file = t;
    v.line = u;
    v.stack = &i;
    v.typestr = "char";
    v.typesize = sizeof(char);
    checkheap(&v, memhead.count + 1);
    checkalloca(&v, 0);
    memhead.event++;
    o = p;
    z = 0;
  retry:
    if ((f == AT_STRNDUP) || (f == AT_STRNSAVE) || (f == AT_STRNDUPA))
        j = 1;
    else
        j = 0;
    n = l;
    /* If the string is not NULL and does not overflow any memory blocks then
     * allocate the memory and copy the string to the new allocation.
     */
    if (__mp_checkstring(&memhead, o, &n, f, &v, j))
    {
        if (p = (char *) __mp_getmemory(&memhead, n + 1, 1, f, &v))
        {
            __mp_memcopy(p, o, n);
            p[n] = '\0';
        }
    }
    else
        p = NULL;
    if (memhead.epilogue && (memhead.recur == 1))
        memhead.epilogue(p, i.addr);
    if (p == NULL)
    {
        if ((z == 0) && memhead.nomemory)
        {
            /* Call the low-memory handler if no memory block was allocated.
             */
            memhead.nomemory();
            if (memhead.prologue && (memhead.recur == 1))
                memhead.prologue(o, (size_t) -2, i.addr);
            z = 1;
            goto retry;
        }
        if ((f == AT_STRDUPA) || (f == AT_STRNDUPA) || (f == AT_XSTRDUP))
        {
            /* The strdupa(), strndupa() and xstrdup() functions should always
             * return a non-NULL pointer, which means we must abort here.
             */
            __mp_printsummary(&memhead);
            __mp_diag("\n");
            __mp_error(ET_OUTMEM, f, t, u, "out of memory");
            memhead.fini = 1;
            __mp_abort();
        }
    }
    restoresignals();
    return p;
}


/* Resize an existing block of memory to a new size and alignment.
 */

void *
__mp_realloc(void *p, size_t l, size_t a, alloctype f, char *s, char *t,
             unsigned long u, char *g, size_t h, size_t k)
{
    void *q;
    stackinfo i;
    loginfo v;
    int j, z;

#if TARGET == TARGET_WINDOWS && !defined(__GNUC__)
    /* If the C run-time library has not finished initialising then we must
     * allocate new memory with sbrk() and copy the old contents to the new
     * allocation.  We can't free the old allocation as we know nothing
     * about it.
     */
    if (!msvcrt_initialised())
    {
        if (p == NULL)
        {
            if (l == 0)
                l = 1;
            if ((q = sbrk(l)) == (void *) -1)
                q = NULL;
            else if (f == AT_RECALLOC)
                __mp_memset(q, 0, l);
        }
        else if ((l == 0) || (f == AT_EXPAND) || ((q = sbrk(l)) == (void *) -1))
            q = NULL;
        else
            __mp_memcopy(q, p, l);
        if (q == NULL)
            if (f == AT_REALLOCF)
                free(p);
            else if (f == AT_XREALLOC)
                abort();
        return q;
    }
#endif /* TARGET && __GNUC__ */
    savesignals();
    if (!memhead.init)
        __mp_init();
    /* Determine the call stack details.
     */
    __mp_newframe(&i, NULL);
    if (__mp_getframe(&i))
    {
        j = __mp_getframe(&i);
        while ((k > 0) && (j != 0))
        {
            j = __mp_getframe(&i);
            k--;
        }
    }
    if (memhead.prologue && (memhead.recur == 1))
        memhead.prologue(p, l, i.addr);
    /* If no filename was passed through then attempt to read any debugging
     * information to determine the source location of the call.
     */
    if ((memhead.recur == 1) && (t == NULL) && (i.addr != NULL) &&
        __mp_findsource(&memhead.syms, (char *) i.addr - 1, &s, &t, &u))
    {
        if (!(memhead.flags & FLG_NOPROTECT))
            __mp_protectstrtab(&memhead.syms.strings, MA_READWRITE);
        if (s != NULL)
            s = __mp_addstring(&memhead.syms.strings, s);
        if (t != NULL)
            t = __mp_addstring(&memhead.syms.strings, t);
        if (!(memhead.flags & FLG_NOPROTECT))
            __mp_protectstrtab(&memhead.syms.strings, MA_READONLY);
    }
    v.func = s;
    v.file = t;
    v.line = u;
    v.stack = &i;
    v.typestr = g;
    v.typesize = h;
    checkheap(&v, memhead.count);
    checkalloca(&v, 0);
    memhead.event++;
    if ((f == AT_XREALLOC) && (l == 0) && (h != 0))
        l = h;
    q = p;
    z = 0;
  retry:
    p = __mp_resizememory(&memhead, q, l, a, f, &v);
    if (memhead.epilogue && (memhead.recur == 1))
        memhead.epilogue(p, i.addr);
    if (p == NULL)
    {
        if ((z == 0) && memhead.nomemory)
        {
            /* Call the low-memory handler if no memory block was allocated.
             */
            memhead.nomemory();
            if (memhead.prologue && (memhead.recur == 1))
                memhead.prologue(q, l, i.addr);
            z = 1;
            goto retry;
        }
        if (f == AT_XREALLOC)
        {
            /* The xrealloc() function should always return a non-NULL pointer,
             * which means we must abort here.
             */
            __mp_printsummary(&memhead);
            __mp_diag("\n");
            __mp_error(ET_OUTMEM, f, t, u, "out of memory");
            memhead.fini = 1;
            __mp_abort();
        }
    }
    restoresignals();
    return p;
}


/* Free an existing block of memory.
 */

void
__mp_free(void *p, alloctype f, char *s, char *t, unsigned long u, size_t k)
{
    stackinfo i;
    loginfo v;
    int j;

#if TARGET == TARGET_WINDOWS && !defined(__GNUC__)
    /* If the C run-time library has not finished initialising then just
     * return since we know nothing about any of the prior allocations anyway.
     */
    if (!msvcrt_initialised() || memhead.fini)
#else /* TARGET && __GNUC__ */
    if (memhead.fini)
#endif /* TARGET && __GNUC__ */
        return;
    savesignals();
    if (!memhead.init)
        __mp_init();
    /* Determine the call stack details.
     */
    __mp_newframe(&i, NULL);
    if (__mp_getframe(&i))
    {
        j = __mp_getframe(&i);
        while ((k > 0) && (j != 0))
        {
            j = __mp_getframe(&i);
            k--;
        }
    }
    if (memhead.prologue && (memhead.recur == 1))
        memhead.prologue(p, (size_t) -1, i.addr);
    /* If no filename was passed through then attempt to read any debugging
     * information to determine the source location of the call.
     */
    if ((memhead.recur == 1) && (t == NULL) && (i.addr != NULL) &&
        __mp_findsource(&memhead.syms, (char *) i.addr - 1, &s, &t, &u))
    {
        if (!(memhead.flags & FLG_NOPROTECT))
            __mp_protectstrtab(&memhead.syms.strings, MA_READWRITE);
        if (s != NULL)
            s = __mp_addstring(&memhead.syms.strings, s);
        if (t != NULL)
            t = __mp_addstring(&memhead.syms.strings, t);
        if (!(memhead.flags & FLG_NOPROTECT))
            __mp_protectstrtab(&memhead.syms.strings, MA_READONLY);
    }
    v.func = s;
    v.file = t;
    v.line = u;
    v.stack = &i;
    v.typestr = NULL;
    v.typesize = 0;
    checkheap(&v, memhead.count);
    checkalloca(&v, 0);
    memhead.event++;
    __mp_freememory(&memhead, p, f, &v);
    if (memhead.epilogue && (memhead.recur == 1))
        memhead.epilogue((void *) -1, i.addr);
    restoresignals();
}


/* Set a block of memory to contain a specific byte.
 */

void *
__mp_setmem(void *p, size_t l, unsigned char c, alloctype f, char *s, char *t,
            unsigned long u, size_t k)
{
    stackinfo i;
    loginfo v;
    int j;

    if (!memhead.init || memhead.fini)
    {
        __mp_memset(p, c, l);
        return p;
    }
    savesignals();
    /* Determine the call stack details.
     */
    __mp_newframe(&i, NULL);
    if (__mp_getframe(&i))
    {
        j = __mp_getframe(&i);
        while ((k > 0) && (j != 0))
        {
            j = __mp_getframe(&i);
            k--;
        }
    }
    /* If no filename was passed through then attempt to read any debugging
     * information to determine the source location of the call.
     */
    if ((memhead.recur == 1) && (t == NULL) && (i.addr != NULL) &&
        __mp_findsource(&memhead.syms, (char *) i.addr - 1, &s, &t, &u))
    {
        if (!(memhead.flags & FLG_NOPROTECT))
            __mp_protectstrtab(&memhead.syms.strings, MA_READWRITE);
        if (s != NULL)
            s = __mp_addstring(&memhead.syms.strings, s);
        if (t != NULL)
            t = __mp_addstring(&memhead.syms.strings, t);
        if (!(memhead.flags & FLG_NOPROTECT))
            __mp_protectstrtab(&memhead.syms.strings, MA_READONLY);
    }
    v.func = s;
    v.file = t;
    v.line = u;
    v.stack = &i;
    v.typestr = NULL;
    v.typesize = 0;
    checkalloca(&v, 0);
    __mp_setmemory(&memhead, p, l, c, f, &v);
    restoresignals();
    return p;
}


/* Copy a block of memory from one address to another.
 */

void *
__mp_copymem(void *p, void *q, size_t l, unsigned char c, alloctype f, char *s,
             char *t, unsigned long u, size_t k)
{
    void *r;
    stackinfo i;
    loginfo v;
    int j;

    if (!memhead.init || memhead.fini)
        if (f == AT_MEMCCPY)
        {
            if (r = __mp_memfind(p, l, &c, 1))
                l = (size_t) ((char *) r - (char *) p) + 1;
            __mp_memcopy(q, p, l);
            if (r != NULL)
                return (char *) q + l;
            else
                return NULL;
        }
        else
        {
            __mp_memcopy(q, p, l);
            return q;
        }
    savesignals();
    /* Determine the call stack details.
     */
    __mp_newframe(&i, NULL);
    if (__mp_getframe(&i))
    {
        j = __mp_getframe(&i);
        while ((k > 0) && (j != 0))
        {
            j = __mp_getframe(&i);
            k--;
        }
    }
    /* If no filename was passed through then attempt to read any debugging
     * information to determine the source location of the call.
     */
    if ((memhead.recur == 1) && (t == NULL) && (i.addr != NULL) &&
        __mp_findsource(&memhead.syms, (char *) i.addr - 1, &s, &t, &u))
    {
        if (!(memhead.flags & FLG_NOPROTECT))
            __mp_protectstrtab(&memhead.syms.strings, MA_READWRITE);
        if (s != NULL)
            s = __mp_addstring(&memhead.syms.strings, s);
        if (t != NULL)
            t = __mp_addstring(&memhead.syms.strings, t);
        if (!(memhead.flags & FLG_NOPROTECT))
            __mp_protectstrtab(&memhead.syms.strings, MA_READONLY);
    }
    v.func = s;
    v.file = t;
    v.line = u;
    v.stack = &i;
    v.typestr = NULL;
    v.typesize = 0;
    checkalloca(&v, 0);
    q = __mp_copymemory(&memhead, p, q, l, c, f, &v);
    restoresignals();
    return q;
}


/* Attempt to locate the position of one block of memory in another block.
 */

void *
__mp_locatemem(void *p, size_t l, void *q, size_t m, alloctype f, char *s,
               char *t, unsigned long u, size_t k)
{
    void *r;
    stackinfo i;
    loginfo v;
    int j;
    unsigned char b;

    if (f == AT_MEMCHR)
    {
        /* If the function that called us was memchr() then we must convert the
         * second length to a character and set up the new pointer and length.
         */
        b = (unsigned char) (m & 0xFF);
        q = (void *) &b;
        m = 1;
    }
    if (!memhead.init || memhead.fini)
        return __mp_memfind(p, l, q, m);
    savesignals();
    /* Determine the call stack details.
     */
    __mp_newframe(&i, NULL);
    if (__mp_getframe(&i))
    {
        j = __mp_getframe(&i);
        while ((k > 0) && (j != 0))
        {
            j = __mp_getframe(&i);
            k--;
        }
    }
    /* If no filename was passed through then attempt to read any debugging
     * information to determine the source location of the call.
     */
    if ((memhead.recur == 1) && (t == NULL) && (i.addr != NULL) &&
        __mp_findsource(&memhead.syms, (char *) i.addr - 1, &s, &t, &u))
    {
        if (!(memhead.flags & FLG_NOPROTECT))
            __mp_protectstrtab(&memhead.syms.strings, MA_READWRITE);
        if (s != NULL)
            s = __mp_addstring(&memhead.syms.strings, s);
        if (t != NULL)
            t = __mp_addstring(&memhead.syms.strings, t);
        if (!(memhead.flags & FLG_NOPROTECT))
            __mp_protectstrtab(&memhead.syms.strings, MA_READONLY);
    }
    v.func = s;
    v.file = t;
    v.line = u;
    v.stack = &i;
    v.typestr = NULL;
    v.typesize = 0;
    checkalloca(&v, 0);
    r = __mp_locatememory(&memhead, p, l, q, m, f, &v);
    restoresignals();
    return r;
}


/* Compare two blocks of memory.
 */

int
__mp_comparemem(void *p, void *q, size_t l, alloctype f, char *s, char *t,
                unsigned long u, size_t k)
{
    void *m;
    stackinfo i;
    loginfo v;
    int j, r;

    if (!memhead.init || memhead.fini)
        if (m = __mp_memcompare(p, q, l))
        {
            l = (char *) m - (char *) p;
            return (int) ((unsigned char *) p)[l] -
                   (int) ((unsigned char *) q)[l];
        }
        else
            return 0;
    savesignals();
    /* Determine the call stack details.
     */
    __mp_newframe(&i, NULL);
    if (__mp_getframe(&i))
    {
        j = __mp_getframe(&i);
        while ((k > 0) && (j != 0))
        {
            j = __mp_getframe(&i);
            k--;
        }
    }
    /* If no filename was passed through then attempt to read any debugging
     * information to determine the source location of the call.
     */
    if ((memhead.recur == 1) && (t == NULL) && (i.addr != NULL) &&
        __mp_findsource(&memhead.syms, (char *) i.addr - 1, &s, &t, &u))
    {
        if (!(memhead.flags & FLG_NOPROTECT))
            __mp_protectstrtab(&memhead.syms.strings, MA_READWRITE);
        if (s != NULL)
            s = __mp_addstring(&memhead.syms.strings, s);
        if (t != NULL)
            t = __mp_addstring(&memhead.syms.strings, t);
        if (!(memhead.flags & FLG_NOPROTECT))
            __mp_protectstrtab(&memhead.syms.strings, MA_READONLY);
    }
    v.func = s;
    v.file = t;
    v.line = u;
    v.stack = &i;
    v.typestr = NULL;
    v.typesize = 0;
    checkalloca(&v, 0);
    r = __mp_comparememory(&memhead, p, q, l, f, &v);
    restoresignals();
    return r;
}


/* Return the name of the function corresponding to a given allocation type.
 */

char *
__mp_function(alloctype f)
{
    if ((f >= (alloctype) 0) && (f < AT_MAX))
        return __mp_functionnames[f];
    return NULL;
}


/* Set the user data for a given memory allocation.
 */

int
__mp_setuser(void *p, void *d)
{
    allocnode *n;
    infonode *m;
    int r;

    savesignals();
    if (!memhead.init)
        __mp_init();
    /* Check that we know something about the address that was supplied.
     */
    if (((n = __mp_findalloc(&memhead.alloc, p)) == NULL) ||
        ((m = (infonode *) n->info) == NULL))
        r = 0;
    else
    {
        if (!(memhead.flags & FLG_NOPROTECT))
            __mp_protectinfo(&memhead, MA_READWRITE);
        m->data.userdata = d;
        if ((memhead.recur == 1) && !(memhead.flags & FLG_NOPROTECT))
            __mp_protectinfo(&memhead, MA_READONLY);
        r = 1;
    }
    restoresignals();
    return r;
}


/* Set the marked flag for a given memory allocation.
 */

int
__mp_setmark(void *p)
{
    allocnode *n;
    infonode *m;
    int r;

    savesignals();
    if (!memhead.init)
        __mp_init();
    /* Check that we know something about the address that was supplied.
     */
    if (((n = __mp_findalloc(&memhead.alloc, p)) == NULL) ||
        ((m = (infonode *) n->info) == NULL))
        r = 0;
    else
    {
        if (!(memhead.flags & FLG_NOPROTECT))
            __mp_protectinfo(&memhead, MA_READWRITE);
        m->data.flags |= FLG_MARKED;
        if ((memhead.recur == 1) && !(memhead.flags & FLG_NOPROTECT))
            __mp_protectinfo(&memhead, MA_READONLY);
        r = 1;
    }
    restoresignals();
    return r;
}


/* Obtain any details about the memory block that contains a given address.
 */

int
__mp_info(void *p, allocinfo *d)
{
    addrnode *a;
    symnode *s;
    allocnode *n;
    infonode *m;

    savesignals();
    if (!memhead.init)
        __mp_init();
    /* Check that we know something about the address that was supplied.
     */
    if (((n = __mp_findnode(&memhead.alloc, p, 1)) == NULL) ||
        ((m = (infonode *) n->info) == NULL))
    {
        restoresignals();
        return 0;
    }
    /* We now fill in the details for the supplied structure.
     */
    d->block = n->block;
    d->size = n->size;
    d->type = m->data.type;
    d->alloc = m->data.alloc;
    d->realloc = m->data.realloc;
#if MP_THREADS_SUPPORT
    d->thread = m->data.thread;
#else /* MP_THREADS_SUPPORT */
    d->thread = 0;
#endif /* MP_THREADS_SUPPORT */
    d->event = m->data.event;
    d->func = m->data.func;
    d->file = m->data.file;
    d->line = m->data.line;
    d->stack = m->data.stack;
    d->typestr = m->data.typestr;
    d->typesize = m->data.typesize;
    d->userdata = m->data.userdata;
    d->freed = ((m->data.flags & FLG_FREED) != 0);
    d->marked = ((m->data.flags & FLG_MARKED) != 0);
    d->profiled = ((m->data.flags & FLG_PROFILED) != 0);
    d->traced = ((m->data.flags & FLG_TRACED) != 0);
    d->internal = ((m->data.flags & FLG_INTERNAL) != 0);
    if (!(memhead.flags & FLG_NOPROTECT))
        __mp_protectinfo(&memhead, MA_READWRITE);
    /* The names of the symbols in the call stack may not have been determined
     * yet, so we traverse the stack, filling in any known symbol names as we
     * go.
     */
    for (a = m->data.stack; a != NULL; a = a->data.next)
        if ((a->data.name == NULL) &&
            (s = __mp_findsymbol(&memhead.syms, a->data.addr)))
            a->data.name = s->data.name;
    if ((memhead.recur == 1) && !(memhead.flags & FLG_NOPROTECT))
        __mp_protectinfo(&memhead, MA_READONLY);
    restoresignals();
    return 1;
}


/* Obtain any details about the function symbol that contains a given address.
 */

int
__mp_syminfo(void *p, symbolinfo *d)
{
    symnode *n;
    char *s, *t;
    unsigned long u;
    int r;

    savesignals();
    if (!memhead.init)
        __mp_init();
    n = __mp_findsymbol(&memhead.syms, p);
    r = __mp_findsource(&memhead.syms, p, &s, &t, &u);
    if (((n == NULL) && (s != NULL)) || (t != NULL))
    {
        if (!(memhead.flags & FLG_NOPROTECT))
            __mp_protectstrtab(&memhead.syms.strings, MA_READWRITE);
        if ((n == NULL) && (s != NULL))
            s = __mp_addstring(&memhead.syms.strings, s);
        if (t != NULL)
            t = __mp_addstring(&memhead.syms.strings, t);
        if (!(memhead.flags & FLG_NOPROTECT))
            __mp_protectstrtab(&memhead.syms.strings, MA_READONLY);
    }
    if ((n != NULL) || (r != 0))
    {
        if (n != NULL)
        {
            d->name = n->data.name;
            d->object = n->data.file;
            d->addr = n->data.addr;
            d->size = n->data.size;
        }
        else
        {
            d->name = s;
            d->object = NULL;
            d->addr = NULL;
            d->size = 0;
        }
        d->file = t;
        d->line = u;
        r = 1;
    }
    restoresignals();
    return r;
}


/* Display any details about the memory block that contains a given address.
 * This is for calling within a symbolic debugger and will result in output to
 * the standard error file stream instead of the log file.
 */

int
__mp_printinfo(void *p)
{
    addrnode *a;
    symnode *s;
    allocnode *n;
    infonode *m;

    savesignals();
    /* Check that we know something about the address that was supplied.
     */
    n = NULL;
    if (!memhead.init || memhead.fini ||
        ((n = __mp_findnode(&memhead.alloc, p, 1)) == NULL) ||
        ((m = (infonode *) n->info) == NULL))
    {
        fprintf(stderr, "address " MP_POINTER, p);
        if (n == NULL)
            fputs(" not in heap\n", stderr);
        else
            fputs(" is in free memory\n", stderr);
        restoresignals();
        return 0;
    }
    /* We now display the details of the associated memory block.
     */
    fprintf(stderr, "address " MP_POINTER " located in %s block:\n", p,
            (m->data.flags & FLG_FREED) ? "freed" : "allocated");
    fprintf(stderr, "    start of block:     " MP_POINTER "\n", n->block);
    fprintf(stderr, "    size of block:      %lu byte%s\n", n->size,
            (n->size == 1) ? "" : "s");
    fprintf(stderr, "    stored type:        %s\n",
            m->data.typestr ? m->data.typestr : "<unknown>");
    fputs("    stored type size:   ", stderr);
    if (m->data.typesize)
        fprintf(stderr, "%lu byte%s\n", m->data.typesize,
                (m->data.typesize == 1) ? "" : "s");
    else
        fputs("<unknown>\n", stderr);
    fprintf(stderr, "    user data:          " MP_POINTER "\n",
            m->data.userdata);
    if (m->data.flags & FLG_FREED)
        fputs("    freed by:           ", stderr);
    else
        fputs("    allocated by:       ", stderr);
    fprintf(stderr, "%s\n", __mp_functionnames[m->data.type]);
    fprintf(stderr, "    allocation index:   %lu\n", m->data.alloc);
    fprintf(stderr, "    reallocation index: %lu\n", m->data.realloc);
#if MP_THREADS_SUPPORT
    fprintf(stderr, "    thread identifier:  %lu\n", m->data.thread);
#endif /* MP_THREADS_SUPPORT */
    fprintf(stderr, "    modification event: %lu\n", m->data.event);
    fputs("    flags:             ", stderr);
    if (m->data.flags == 0)
        fputs(" none\n", stderr);
    else
    {
        if (m->data.flags & FLG_FREED)
            fputs(" freed", stderr);
        if (m->data.flags & FLG_MARKED)
            fputs(" marked", stderr);
        if (m->data.flags & FLG_PROFILED)
            fputs(" profiled", stderr);
        if (m->data.flags & FLG_TRACED)
            fputs(" traced", stderr);
        if (m->data.flags & FLG_INTERNAL)
            fputs(" internal", stderr);
        fputc('\n', stderr);
    }
    fprintf(stderr, "    calling function:   %s\n",
            m->data.func ? m->data.func : "<unknown>");
    fprintf(stderr, "    called from file:   %s\n",
            m->data.file ? m->data.file : "<unknown>");
    fputs("    called at line:     ", stderr);
    if (m->data.line)
        fprintf(stderr, "%lu\n", m->data.line);
    else
        fputs("<unknown>\n", stderr);
    /* Traverse the function call stack, displaying as much information as
     * possible.
     */
    if (a = m->data.stack)
    {
        fputs("    function call stack:\n", stderr);
        do
        {
            fprintf(stderr, "\t" MP_POINTER " ", a->data.addr);
            if (a->data.name)
                fputs(a->data.name, stderr);
            else if (s = __mp_findsymbol(&memhead.syms, a->data.addr))
                fputs(s->data.name, stderr);
            else
                fputs("???", stderr);
            fputc('\n', stderr);
            a = a->data.next;
        }
        while (a != NULL);
    }
    restoresignals();
    return 1;
}


/* Return the current allocation event count for later use when examining
 * the difference in the list of allocations between now and a future point.
 */

unsigned long
__mp_snapshot(void)
{
    unsigned long i;

    savesignals();
    if (!memhead.init)
        __mp_init();
    i = memhead.event;
    restoresignals();
    return i;
}


/* Iterate over all of the allocated and freed memory blocks, calling a
 * user-supplied function for each one encountered, selecting only those
 * memory blocks that have been modified since a given allocation event.
 */

size_t
__mp_iterate(int (*f)(void *, void *), void *d, unsigned long s)
{
    allocnode *n, *p;
    infonode *m;
    size_t i;
    int r;

    savesignals();
    if (!memhead.init)
        __mp_init();
    i = 0;
    for (n = (allocnode *) memhead.alloc.list.head;
         p = (allocnode *) n->lnode.next; n = p)
        if ((m = (infonode *) n->info) && !(m->data.flags & FLG_INTERNAL) &&
            (m->data.event > s))
        {
            if (f == NULL)
                r = __mp_printinfo(n->block);
            else
                r = f(n->block, d);
            if (r > 0)
                i++;
            else if (r < 0)
                break;
        }
    restoresignals();
    return i;
}


/* Display a complete memory map of the heap and (optionally) a summary of
 * all mpatrol library settings and statistics.
 */

void
__mp_memorymap(int s)
{
    savesignals();
    if (!memhead.init)
        __mp_init();
    if (s != 0)
        __mp_printsummary(&memhead);
    if (memhead.alloc.list.size > 0)
        __mp_printmap(&memhead);
    restoresignals();
}


/* Display a summary of all mpatrol library settings and statistics.
 */

void
__mp_summary(void)
{
    savesignals();
    if (!memhead.init)
        __mp_init();
    __mp_printsummary(&memhead);
    restoresignals();
}


/* Return statistics about the current state of the heap.
 */

int
__mp_stats(heapinfo *d)
{
    savesignals();
    if (!memhead.init)
        __mp_init();
    d->acount = memhead.alloc.atree.size;
    d->atotal = memhead.alloc.asize;
    d->fcount = memhead.alloc.ftree.size;
    d->ftotal = memhead.alloc.fsize;
    d->gcount = memhead.alloc.gtree.size;
    d->gtotal = memhead.alloc.gsize;
    d->icount = memhead.alloc.heap.itree.size + memhead.alloc.itree.size +
                memhead.addr.list.size + memhead.syms.strings.list.size +
                memhead.syms.strings.tree.size + memhead.syms.itree.size +
                memhead.prof.ilist.size + memhead.list.size +
                memhead.alist.size;
    d->itotal = memhead.alloc.heap.isize + memhead.alloc.isize +
                memhead.addr.size + memhead.syms.strings.size +
                memhead.syms.size + memhead.prof.size + memhead.size;
    restoresignals();
    return 1;
}


/* Check the validity of all memory blocks that have been filled with
 * a predefined pattern.
 */

void
__mp_checkheap(char *s, char *t, unsigned long u)
{
    stackinfo i;
    loginfo v;

    savesignals();
    if (!memhead.init)
        __mp_init();
    /* Determine the call stack details in case we need to free any
     * allocations that were made by alloca().
     */
    __mp_newframe(&i, NULL);
    if (__mp_getframe(&i))
        __mp_getframe(&i);
    /* If no filename was passed through then attempt to read any debugging
     * information to determine the source location of the call.
     */
    if ((memhead.recur == 1) && (t == NULL) && (i.addr != NULL) &&
        __mp_findsource(&memhead.syms, (char *) i.addr - 1, &s, &t, &u))
    {
        if (!(memhead.flags & FLG_NOPROTECT))
            __mp_protectstrtab(&memhead.syms.strings, MA_READWRITE);
        if (s != NULL)
            s = __mp_addstring(&memhead.syms.strings, s);
        if (t != NULL)
            t = __mp_addstring(&memhead.syms.strings, t);
        if (!(memhead.flags & FLG_NOPROTECT))
            __mp_protectstrtab(&memhead.syms.strings, MA_READONLY);
    }
    v.func = s;
    v.file = t;
    v.line = u;
    v.stack = &i;
    v.typestr = NULL;
    v.typesize = 0;
    __mp_checkinfo(&memhead, &v);
    checkalloca(&v, 0);
    restoresignals();
}


/* Check the validity of all memory blocks that have been filled with
 * a predefined pattern.
 */

void
__mp_check(void)
{
    __mp_checkheap(NULL, NULL, 0);
}


/* Set the prologue function and return the previous setting.
 */

void
(*__mp_prologue(void (*h)(void *, size_t, void *)))(void *, size_t, void *)
{
    void (*p)(void *, size_t, void *);

    savesignals();
    if (!memhead.init)
        __mp_init();
    p = memhead.prologue;
    memhead.prologue = h;
    restoresignals();
    return p;
}


/* Set the epilogue function and return the previous setting.
 */

void
(*__mp_epilogue(void (*h)(void *, void *)))(void *, void *)
{
    void (*p)(void *, void *);

    savesignals();
    if (!memhead.init)
        __mp_init();
    p = memhead.epilogue;
    memhead.epilogue = h;
    restoresignals();
    return p;
}


/* Set the low-memory handler and return the previous setting.
 */

void
(*__mp_nomemory(void (*h)(void)))(void)
{
    void (*p)(void);

    savesignals();
    if (!memhead.init)
        __mp_init();
    p = memhead.nomemory;
    memhead.nomemory = h;
    restoresignals();
    return p;
}


/* Push source level information onto the top of the delete stack.
 */

void
__mp_pushdelstack(char *s, char *t, unsigned long u)
{
    if (!memhead.init)
        __mp_init();
    if ((memhead.delpos >= 0) && (memhead.delpos < MP_MAXDELSTACK))
    {
        memhead.dels[memhead.delpos].func = s;
        memhead.dels[memhead.delpos].file = t;
        memhead.dels[memhead.delpos].line = u;
    }
    memhead.delpos++;
}


/* Pop source level information off the top of the delete stack.
 */

void
__mp_popdelstack(char **s, char **t, unsigned long *u)
{
    if (!memhead.init)
        __mp_init();
    if ((--memhead.delpos >= 0) && (memhead.delpos < MP_MAXDELSTACK))
    {
        *s = memhead.dels[memhead.delpos].func;
        *t = memhead.dels[memhead.delpos].file;
        *u = memhead.dels[memhead.delpos].line;
    }
    else
    {
        *s = *t = NULL;
        *u = 0;
    }
}


/* Write user data to the mpatrol log file.
 */

int
__mp_printf(char *s, ...)
{
    char b[1024];
    char *p, *t;
    va_list v;
    size_t l;
    int r;

    savesignals();
    if (!memhead.init)
        __mp_init();
    va_start(v, s);
    r = vsprintf(b, s, v);
    va_end(v);
    if (r >= 0)
    {
        l = strlen(MP_PRINTPREFIX);
        for (t = b; p = strchr(t, '\n'); t = p + 1)
        {
            *p = '\0';
            if (*t != '\0')
            {
                __mp_diag("%s%s", MP_PRINTPREFIX, t);
                r += l;
            }
            __mp_diag("\n");
        }
        if (*t != '\0')
        {
            __mp_diag("%s%s\n", MP_PRINTPREFIX, t);
            r += l + 1;
        }
    }
    restoresignals();
    return r;
}


/* Write user data to the mpatrol log file.
 */

int
__mp_vprintf(char *s, va_list v)
{
    char b[1024];
    char *p, *t;
    size_t l;
    int r;

    savesignals();
    if (!memhead.init)
        __mp_init();
    r = vsprintf(b, s, v);
    if (r >= 0)
    {
        l = strlen(MP_PRINTPREFIX);
        for (t = b; p = strchr(t, '\n'); t = p + 1)
        {
            *p = '\0';
            if (*t != '\0')
            {
                __mp_diag("%s%s", MP_PRINTPREFIX, t);
                r += l;
            }
            __mp_diag("\n");
        }
        if (*t != '\0')
        {
            __mp_diag("%s%s\n", MP_PRINTPREFIX, t);
            r += l + 1;
        }
    }
    restoresignals();
    return r;
}


/* Write a hex dump for a specified memory location to the mpatrol log file.
 */

void
__mp_logmemory(void *p, size_t l)
{
    savesignals();
    if (!memhead.init)
        __mp_init();
    __mp_printmemory(p, l);
    __mp_diag("\n");
    restoresignals();
}


/* Write the current call stack to the mpatrol log file.
 */

int
__mp_logstack(size_t k)
{
    stackinfo i;
    int r;

    savesignals();
    if (!memhead.init)
        __mp_init();
    __mp_newframe(&i, NULL);
    if (r = __mp_getframe(&i))
    {
        r = __mp_getframe(&i);
        while ((k > 0) && (r != 0))
        {
            r = __mp_getframe(&i);
            k--;
        }
    }
    if (r != 0)
    {
        __mp_printstack(&memhead.syms, &i);
        __mp_diag("\n");
    }
    restoresignals();
    return r;
}


/* Write the details about the memory block that contains a given address to
 * the mpatrol log file.
 */

int
__mp_logaddr(void *p)
{
    allocnode *n;
    int r;

    savesignals();
    if (!memhead.init)
        __mp_init();
    if (((n = __mp_findnode(&memhead.alloc, p, 1)) == NULL) ||
        (n->info == NULL))
        r = 0;
    else
    {
        __mp_printalloc(&memhead.syms, n);
        __mp_diag("\n");
        r = 1;
    }
    restoresignals();
    return r;
}


/* Invoke a text editor on a given source file at a specific line.
 */

int
__mp_edit(char *f, unsigned long l)
{
    int r;

    savesignals();
    if (!memhead.init)
        __mp_init();
    r = __mp_editfile(f, l, 0);
    restoresignals();
    return r;
}


/* List a given source file at a specific line.
 */

int
__mp_list(char *f, unsigned long l)
{
    int r;

    savesignals();
    if (!memhead.init)
        __mp_init();
    r = __mp_editfile(f, l, 1);
    restoresignals();
    return r;
}


/* Edit or list a given source file at a specific line, but only if the EDIT
 * or LIST options are in effect.
 */

int
__mp_view(char *f, unsigned long l)
{
    int r;

    savesignals();
    if (!memhead.init)
        __mp_init();
    if (__mp_diagflags & FLG_EDIT)
        r = __mp_editfile(f, l, 0);
    else if (__mp_diagflags & FLG_LIST)
        r = __mp_editfile(f, l, 1);
    else
        r = 0;
    restoresignals();
    return r;
}


/* The following function definitions are used by GNU autoconf to determine
 * which libraries must be linked in with mpatrol on a specific system.
 * They must be globally visible and have to be function symbols rather than
 * data symbols so that AC_CHECK_LIB() works reliably.
 */

#if TARGET == TARGET_WINDOWS || SYSTEM == SYSTEM_CYGWIN
#if FORMAT == FORMAT_PE || DYNLINK == DYNLINK_WINDOWS || MP_LIBRARYSTACK_SUPPORT
void __mp_libimagehlp(void) {}
#endif /* FORMAT && DYNLINK && MP_LIBRARYSTACK_SUPPORT */
#elif SYSTEM == SYSTEM_HPUX
#if MP_LIBRARYSTACK_SUPPORT
void __mp_libcl(void) {}
#endif /* MP_LIBRARYSTACK_SUPPORT */
#elif SYSTEM == SYSTEM_IRIX
#if MP_LIBRARYSTACK_SUPPORT
void __mp_libexc(void) {}
#endif /* MP_LIBRARYSTACK_SUPPORT */
#endif /* TARGET && SYSTEM */
#if (FORMAT == FORMAT_COFF || FORMAT == FORMAT_XCOFF) && SYSTEM != SYSTEM_LYNXOS
void __mp_libld(void) {}
#elif FORMAT == FORMAT_ELF32 || FORMAT == FORMAT_ELF64
void __mp_libelf(void) {}
#elif FORMAT == FORMAT_BFD
void __mp_libbfd(void) {}
void __mp_libiberty(void) {}
#endif /* FORMAT */


/* Set the access rights for a block of memory using the checker interface.
 */

void
chkr_set_right(void *p, size_t l, unsigned char a)
{
    char *s, *t;
    stackinfo i;
    loginfo v;
    unsigned long u;

#if TARGET == TARGET_WINDOWS && !defined(__GNUC__)
    /* If the C run-time library has not finished initialising then we cannot
     * initialise the mpatrol library and so we just return.
     */
    if (!msvcrt_initialised())
        return;
#endif /* TARGET && __GNUC__ */
    savesignals();
    if (!memhead.init)
        __mp_init();
    /* Determine the call stack details in case we need to report any errors.
     */
    __mp_newframe(&i, NULL);
    if (__mp_getframe(&i))
        __mp_getframe(&i);
    /* No filename was passed through so attempt to read any debugging
     * information to determine the source location of the call.
     */
    if ((memhead.recur == 1) && (i.addr != NULL) &&
        __mp_findsource(&memhead.syms, (char *) i.addr - 1, &s, &t, &u))
    {
        if (!(memhead.flags & FLG_NOPROTECT))
            __mp_protectstrtab(&memhead.syms.strings, MA_READWRITE);
        if (s != NULL)
            s = __mp_addstring(&memhead.syms.strings, s);
        if (t != NULL)
            t = __mp_addstring(&memhead.syms.strings, t);
        if (!(memhead.flags & FLG_NOPROTECT))
            __mp_protectstrtab(&memhead.syms.strings, MA_READONLY);
    }
    v.func = s;
    v.file = t;
    v.line = u;
    v.stack = &i;
    v.typestr = NULL;
    v.typesize = 0;
    if (!__mp_checkrange(&memhead, p, l, AT_MAX, &v))
    {
        memhead.fini = 1;
        __mp_abort();
    }
    restoresignals();
}


/* Copy the access rights for a block of memory to another block using the
 * checker interface.
 */

void
chkr_copy_bitmap(void *p, void *q, size_t l)
{
    char *s, *t;
    stackinfo i;
    loginfo v;
    unsigned long u;

#if TARGET == TARGET_WINDOWS && !defined(__GNUC__)
    /* If the C run-time library has not finished initialising then we cannot
     * initialise the mpatrol library and so we just return.
     */
    if (!msvcrt_initialised())
        return;
#endif /* TARGET && __GNUC__ */
    savesignals();
    if (!memhead.init)
        __mp_init();
    /* Determine the call stack details in case we need to report any errors.
     */
    __mp_newframe(&i, NULL);
    if (__mp_getframe(&i))
        __mp_getframe(&i);
    /* No filename was passed through so attempt to read any debugging
     * information to determine the source location of the call.
     */
    if ((memhead.recur == 1) && (i.addr != NULL) &&
        __mp_findsource(&memhead.syms, (char *) i.addr - 1, &s, &t, &u))
    {
        if (!(memhead.flags & FLG_NOPROTECT))
            __mp_protectstrtab(&memhead.syms.strings, MA_READWRITE);
        if (s != NULL)
            s = __mp_addstring(&memhead.syms.strings, s);
        if (t != NULL)
            t = __mp_addstring(&memhead.syms.strings, t);
        if (!(memhead.flags & FLG_NOPROTECT))
            __mp_protectstrtab(&memhead.syms.strings, MA_READONLY);
    }
    v.func = s;
    v.file = t;
    v.line = u;
    v.stack = &i;
    v.typestr = NULL;
    v.typesize = 0;
    if (!__mp_checkrange(&memhead, p, l, AT_MAX, &v) ||
        !__mp_checkrange(&memhead, q, l, AT_MAX, &v))
    {
        memhead.fini = 1;
        __mp_abort();
    }
    restoresignals();
}


/* Check a block of memory using the checker interface.
 */

void
chkr_check_addr(void *p, size_t l, unsigned char a)
{
    char *s, *t;
    stackinfo i;
    loginfo v;
    unsigned long u;

#if TARGET == TARGET_WINDOWS && !defined(__GNUC__)
    /* If the C run-time library has not finished initialising then we cannot
     * initialise the mpatrol library and so we just return.
     */
    if (!msvcrt_initialised())
        return;
#endif /* TARGET && __GNUC__ */
    savesignals();
    if (!memhead.init)
        __mp_init();
    /* Determine the call stack details in case we need to report any errors.
     */
    __mp_newframe(&i, NULL);
    if (__mp_getframe(&i))
        __mp_getframe(&i);
    /* No filename was passed through so attempt to read any debugging
     * information to determine the source location of the call.
     */
    if ((memhead.recur == 1) && (i.addr != NULL) &&
        __mp_findsource(&memhead.syms, (char *) i.addr - 1, &s, &t, &u))
    {
        if (!(memhead.flags & FLG_NOPROTECT))
            __mp_protectstrtab(&memhead.syms.strings, MA_READWRITE);
        if (s != NULL)
            s = __mp_addstring(&memhead.syms.strings, s);
        if (t != NULL)
            t = __mp_addstring(&memhead.syms.strings, t);
        if (!(memhead.flags & FLG_NOPROTECT))
            __mp_protectstrtab(&memhead.syms.strings, MA_READONLY);
    }
    v.func = s;
    v.file = t;
    v.line = u;
    v.stack = &i;
    v.typestr = NULL;
    v.typesize = 0;
    if (!__mp_checkrange(&memhead, p, l, AT_MAX, &v))
    {
        memhead.fini = 1;
        __mp_abort();
    }
    restoresignals();
}


/* Check a string using the checker interface.
 */

void
chkr_check_str(char *p, unsigned char a)
{
    char *s, *t;
    stackinfo i;
    loginfo v;
    size_t l;
    unsigned long u;

#if TARGET == TARGET_WINDOWS && !defined(__GNUC__)
    /* If the C run-time library has not finished initialising then we cannot
     * initialise the mpatrol library and so we just return.
     */
    if (!msvcrt_initialised())
        return;
#endif /* TARGET && __GNUC__ */
    savesignals();
    if (!memhead.init)
        __mp_init();
    /* Determine the call stack details in case we need to report any errors.
     */
    __mp_newframe(&i, NULL);
    if (__mp_getframe(&i))
        __mp_getframe(&i);
    /* No filename was passed through so attempt to read any debugging
     * information to determine the source location of the call.
     */
    if ((memhead.recur == 1) && (i.addr != NULL) &&
        __mp_findsource(&memhead.syms, (char *) i.addr - 1, &s, &t, &u))
    {
        if (!(memhead.flags & FLG_NOPROTECT))
            __mp_protectstrtab(&memhead.syms.strings, MA_READWRITE);
        if (s != NULL)
            s = __mp_addstring(&memhead.syms.strings, s);
        if (t != NULL)
            t = __mp_addstring(&memhead.syms.strings, t);
        if (!(memhead.flags & FLG_NOPROTECT))
            __mp_protectstrtab(&memhead.syms.strings, MA_READONLY);
    }
    v.func = s;
    v.file = t;
    v.line = u;
    v.stack = &i;
    v.typestr = NULL;
    v.typesize = 0;
    if (!__mp_checkstring(&memhead, p, &l, AT_MAX, &v, 0))
    {
        memhead.fini = 1;
        __mp_abort();
    }
    restoresignals();
}


/* Check a function pointer using the checker interface.
 */

void
chkr_check_exec(void *p)
{
    char *s, *t;
    stackinfo i;
    loginfo v;
    unsigned long u;

#if TARGET == TARGET_WINDOWS && !defined(__GNUC__)
    /* If the C run-time library has not finished initialising then we cannot
     * initialise the mpatrol library and so we just return.
     */
    if (!msvcrt_initialised())
        return;
#endif /* TARGET && __GNUC__ */
    savesignals();
    if (!memhead.init)
        __mp_init();
    /* Determine the call stack details in case we need to report any errors.
     */
    __mp_newframe(&i, NULL);
    if (__mp_getframe(&i))
        __mp_getframe(&i);
    /* No filename was passed through so attempt to read any debugging
     * information to determine the source location of the call.
     */
    if ((memhead.recur == 1) && (i.addr != NULL) &&
        __mp_findsource(&memhead.syms, (char *) i.addr - 1, &s, &t, &u))
    {
        if (!(memhead.flags & FLG_NOPROTECT))
            __mp_protectstrtab(&memhead.syms.strings, MA_READWRITE);
        if (s != NULL)
            s = __mp_addstring(&memhead.syms.strings, s);
        if (t != NULL)
            t = __mp_addstring(&memhead.syms.strings, t);
        if (!(memhead.flags & FLG_NOPROTECT))
            __mp_protectstrtab(&memhead.syms.strings, MA_READONLY);
    }
    v.func = s;
    v.file = t;
    v.line = u;
    v.stack = &i;
    v.typestr = NULL;
    v.typesize = 0;
    restoresignals();
}


#ifdef __cplusplus
}
#endif /* __cplusplus */
