#ifndef MP_GETOPT_H
#define MP_GETOPT_H


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
 * Option parsing.  Provides a common interface for the mpatrol tools to parse
 * command line options.
 */


#include "config.h"


#ifdef __cplusplus
extern "C"
{
#endif /* __cplusplus */


MP_EXPORT unsigned long __mp_optindex;
MP_EXPORT char *__mp_optarg;


MP_EXPORT int __mp_getopt(unsigned long, char **, char *);


#ifdef __cplusplus
}
#endif /* __cplusplus */


#endif /* MP_GETOPT_H */