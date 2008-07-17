/*
 * mpatrol
 * A library for controlling and tracing dynamic memory allocations.
 * Copyright (C) 1997-2008 Graeme S. Roy <graemeroy@users.sourceforge.net>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or (at         * your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser
 * General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */


/*
 * DLL entry point.  Provides a way to initialise and finalise the mpalloc
 * library when it is built as a DLL.
 */


#include "config.h"
#include <windows.h>
#include <winbase.h>


#if MP_IDENT_SUPPORT
#ident "$Id: main2.c,v 1.7 2008-07-17 11:19:13 groy Exp $"
#else /* MP_IDENT_SUPPORT */
static MP_CONST MP_VOLATILE char *main2_id = "$Id: main2.c,v 1.7 2008-07-17 11:19:13 groy Exp $";
#endif /* MP_IDENT_SUPPORT */


#ifdef __cplusplus
extern "C"
{
#endif /* __cplusplus */


/* DLL entry point to initialise and finalise the library.
 */

BOOL
WINAPI
DllMain(HANDLE h, DWORD r, LPVOID d)
{
    return TRUE;
}


#ifdef __cplusplus
}
#endif /* __cplusplus */
