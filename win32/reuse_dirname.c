/* -*- mode:c -*- */
/* $Id: reuse_dirname.c 6528 2011-11-25 18:08:26Z cher $ */

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

#include "reuse_xalloc.h"
#include "reuse_osdeps.h"

#include <string.h>

/**
 * NAME:    os_DirName
 * PURPOSE: return dir name from file path
 * ARGS:    argpath - path to the file
 * RETURN:  the directory component of the path
 */
char *
os_DirName(char const *str)
{
  int         len;
  char const *s;

  if (!str) return xstrdup("");
  len = strlen(str);
  for (s = str + len - 1; s >= str; s--) {
    if (*s == '/' || *s == '\\' || *s == ':') {
      break;
    }
  }
  if (s < str) {
    return xstrdup(".");
  }
  if (*s == ':') s++;

  return xmemdup(str, s - str);
}

/*
 * Local variables:
 *  compile-command: "make -C .."
 * End:
 */
