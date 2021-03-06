/* -*- mode:c -*- */
/* $Id: reuse_getlastname.c 6166 2011-03-27 10:27:54Z cher $ */

/* Copyright (C) 2004-2011 Alexander Chernov <cher@ejudge.ru> */

/*
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 */

#include "reuse_xalloc.h"
#include "reuse_osdeps.h"

#include <string.h>

char *
os_GetLastname(char const *path)
{
  char const *sp, *dp;
  int   l;

  if (!path) return xstrdup("");

  l  = strlen(path);
  sp = strrchr(path, '/');

  if (!sp)
    sp = path;
  else
    sp++;
  dp = path + l;

  return xmemdup(sp, dp - sp);
}

/*
 * Local variables:
 *  compile-command: "make -C .."
 * End:
 */
