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
 * Memory allocation profiling.  The call graphs for every memory allocation
 * and deallocation are recorded here along with their memory usage statistics
 * and are written to a file for later processing by a profiling tool.
 */


#include "profile.h"
#include "info.h"
#include "diag.h"
#include <stdio.h>
#include <string.h>


#if MP_IDENT_SUPPORT
#ident "$Id: profile.c,v 1.11 2000-04-20 00:55:23 graeme Exp $"
#endif /* MP_IDENT_SUPPORT */


#ifdef __cplusplus
extern "C"
{
#endif /* __cplusplus */


/* Initialise the fields of a profhead so that the mpatrol library
 * is ready to profile memory allocations.
 */

MP_GLOBAL void __mp_newprofile(profhead *p)
{
    size_t i;

    for (i = 0; i < MP_BIN_SIZE; i++)
        p->acounts[i] = p->dcounts[i] = 0;
    p->acountl = p->dcountl = 0;
    p->acountt = p->dcountt = 0;
    p->acount = p->dcount = 0;
    p->sbound = MP_SMALLBOUND;
    p->mbound = MP_MEDIUMBOUND;
    p->lbound = MP_LARGEBOUND;
    p->autosave = p->autocount = 0;
    p->file = MP_PROFFILE;
    p->profiling = 0;
}


/* Forget all existing profiling information.
 */

MP_GLOBAL void __mp_deleteprofile(profhead *p)
{
    size_t i;

    for (i = 0; i < MP_BIN_SIZE; i++)
        p->acounts[i] = p->dcounts[i] = 0;
    p->acountl = p->dcountl = 0;
    p->acountt = p->dcountt = 0;
    p->acount = p->dcount = 0;
    p->autocount = 0;
    p->file = NULL;
    p->profiling = 0;
}


/* Record a memory allocation for profiling.
 */

MP_GLOBAL int __mp_profilealloc(profhead *p, size_t l, void *d)
{
    /* Note the size of the allocation in one of the allocation bins.
     * The highest allocation bin stores a count of all the allocations
     * that are larger than the largest bin.
     */
    if (l < MP_BIN_SIZE)
        p->acounts[l - 1]++;
    else
    {
        p->acounts[MP_BIN_SIZE - 1]++;
        p->acountl += l;
    }
    p->acountt += l;
    p->acount++;
    /* If the autosave feature is enabled then we may need to write out
     * all of the current profiling information to the output file before
     * we can return.
     */
    p->autocount++;
    if ((p->autosave != 0) && (p->autocount % p->autosave == 0))
        __mp_writeprofile(p);
    return 1;
}


/* Record a memory deallocation for profiling.
 */

MP_GLOBAL int __mp_profilefree(profhead *p, size_t l, void *d)
{
    /* Note the size of the deallocation in one of the deallocation bins.
     * The highest deallocation bin stores a count of all the deallocations
     * that are larger than the largest bin.
     */
    if (l < MP_BIN_SIZE)
        p->dcounts[l - 1]++;
    else
    {
        p->dcounts[MP_BIN_SIZE - 1]++;
        p->dcountl += l;
    }
    p->dcountt += l;
    p->dcount++;
    /* If the autosave feature is enabled then we may need to write out
     * all of the current profiling information to the output file before
     * we can return.
     */
    p->autocount++;
    if ((p->autosave != 0) && (p->autocount % p->autosave == 0))
        __mp_writeprofile(p);
    return 1;
}


/* Write the profiling information to the output file.
 */

MP_GLOBAL int __mp_writeprofile(profhead *p)
{
    FILE *f;
    size_t n;

    p->autocount = 0;
    /* The profiling file name can also be named as stderr and stdout which
     * will go to the standard error and standard output streams respectively.
     */
    if (p->file == NULL)
        return 0;
    else if (strcmp(p->file, "stderr") == 0)
        f = stderr;
    else if (strcmp(p->file, "stdout") == 0)
        f = stdout;
    else if ((f = fopen(p->file, "wb")) == NULL)
    {
        __mp_error(AT_MAX, "%s: cannot open file\n", p->file);
        p->file = NULL;
        return 0;
    }
    /* Technically, we should check the return values from each of the calls
     * to fwrite().  However, that would increase the complexity of this
     * function and would make the code extremely hard to follow.  Instead,
     * we just assume that each write to the output file succeeds and hope
     * that if an error does occur then it will not be too drastic if we
     * continue writing the rest of the file.
     */
    fwrite(&p->acount, sizeof(size_t), 1, f);
    fwrite(&p->acountt, sizeof(size_t), 1, f);
    fwrite(&p->dcount, sizeof(size_t), 1, f);
    fwrite(&p->dcountt, sizeof(size_t), 1, f);
    /* Write out the contents of the allocation and deallocation bins.
     */
    n = MP_BIN_SIZE;
    fwrite(&n, sizeof(size_t), 1, f);
    fwrite(p->acounts, sizeof(size_t), MP_BIN_SIZE, f);
    fwrite(&p->acountl, sizeof(size_t), 1, f);
    fwrite(p->dcounts, sizeof(size_t), MP_BIN_SIZE, f);
    fwrite(&p->dcountl, sizeof(size_t), 1, f);
    if ((f != stderr) && (f != stdout) && fclose(f))
        return 0;
    return 1;
}


#ifdef __cplusplus
}
#endif /* __cplusplus */
