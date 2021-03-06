/* -*- mode:c -*- */
/* $Id: reuse_geterrorstring.c 6166 2011-03-27 10:27:54Z cher $ */

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
#include <errno.h>

static char errbuf[256];

/**
 * NAME:    os_GetErrorString
 * PURPOSE: get error string by error code
 * ARGS:    e - error code
 * RETURN:  the corresponding error string
 */
char *
os_GetErrorString(int e)
{
  char *s = strerror(e);
  if (s == NULL)
    {
      sprintf(errbuf, "unknown error (%d)", e);
      return errbuf;
    }
  return s;
}

/*
 * Local variables:
 *  compile-command: "make -C .."
 * End:
 */
