/* -*- mode: c -*- */
/* $Id: file_perms.c 6003 2010-10-05 07:57:27Z cher $ */

/* Copyright (C) 2009-2010 Alexander Chernov <cher@ejudge.ru> */

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

#include "file_perms.h"
#include "misctext.h"

int
file_perms_parse_mode(const unsigned char *mode)
{
  return -1;
}

int
file_perms_parse_group(const unsigned char *group)
{
  return -1;
}

int
file_perms_set(
        FILE *flog,
        const unsigned char *path,
        int group,
        int mode,
        int old_group,
        int old_mode)
{
  return -1;
}

void
file_perms_get(
        const unsigned char *path,
        int *p_group,
        int *p_mode)
{
}

/*
 * Local variables:
 *  compile-command: "make -C .."
 *  c-font-lock-extra-types: ("\\sw+_t" "FILE")
 * End:
 */
