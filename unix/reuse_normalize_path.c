/* -*- mode:c -*- */
/* $Id: reuse_normalize_path.c 6166 2011-03-27 10:27:54Z cher $ */

/* Copyright (C) 2002-2011 Alexander Chernov <cher@ejudge.ru> */

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

#include "reuse_osdeps.h"

#include <stdio.h>
#include <string.h>

/*
 * remove recurring /
 * remove / from the end of the path
 */
void
os_normalize_path(char *path)
{
  char *d, *s;

  if (!path || !*path) return;

  s = path;
  while (*s) {
    s++;
  }

  s--;
  while (s >= path && *s == '/') *s-- = 0;
  if (!*path) *path = '/';

  s = d = path;
  while (*s) {
    if (*s == '/') {
      *d++ = *s++;
      while (*s == '/') s++;
    } else {
      *d++ = *s++;
    }
  }
  *d = *s;
}

/*
 * Local variables:
 *  compile-command: "make -C .."
 * End:
 */
