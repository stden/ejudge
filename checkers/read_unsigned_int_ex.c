/* -*- mode: c -*- */
/* $Id: read_unsigned_int_ex.c 7462 2013-10-22 05:39:39Z cher $ */

/* Copyright (C) 2010-2013 Alexander Chernov <cher@ejudge.ru> */

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
checker_read_unsigned_int_ex(
        FILE *f,
        checker_error_func_t error_func,
        const char *name,
        int eof_error_flag,
        unsigned int *p_val)
{
  char sbuf[128] = { 0 };
  char *dbuf = 0;
  size_t dsize = 0;
  char *vbuf = 0;
  char *eptr = 0;
  unsigned int val;

  if (!name) name = "";

  vbuf = checker_read_buf_ex(f, error_func, name, eof_error_flag,
                             sbuf, sizeof(sbuf), &dbuf, &dsize);
  if (!vbuf) return -1;
  if (!*vbuf) error_func(_("%s: no uint32 value"), name);
  if (vbuf[0] == '-') error_func(_("%s: `-' before uint32 value"), name);
  errno = 0;
  val = strtoul(vbuf, &eptr, 10);
  if (*eptr) error_func(_("%s: cannot parse uint32 value"), name);
  if (errno) error_func(_("%s: uint32 value is out of range"), name);
  free(dbuf); dbuf = 0;
  *p_val = val;
  return 1;
}
