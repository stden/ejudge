/* -*- mode: c -*- */
/* $Id: read_long_double.c 7462 2013-10-22 05:39:39Z cher $ */

/* Copyright (C) 2003-2013 Alexander Chernov <cher@ejudge.ru> */

/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include "checker_internal.h"

#include <errno.h>

#include "l10n_impl.h"

int
checker_read_long_double(
        int ind,
        const char *name,
        int eof_error_flag,
        long double *p_val)
{
  long double x;
  char sb[128], *db = 0, *vb = 0, *ep = 0;
  size_t ds = 0;

  if (!name) name = "";
  vb = checker_read_buf_2(ind, name, eof_error_flag, sb, sizeof(sb), &db, &ds);
  if (!vb) return -1;
  if (!*vb) {
    fatal_read(ind, _("%s: no long double value"), name);
  }
  errno = 0;
  x = strtold(vb, &ep);
  if (*ep) {
    fatal_read(ind, _("%s: cannot parse long double value"), name);
  }
  if (errno) {
    fatal_read(ind, _("%s: long double value is out of range"), name);
  }
  *p_val = x;
  return 1;
}
