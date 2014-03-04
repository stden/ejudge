/* -*- mode:c -*- */
/* $Id: reuse_rgetworkingdir.c 6165 2011-03-27 10:26:54Z cher $ */

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

#include "reuse_logger.h"
#include "reuse_osdeps.h"

#include <windows.h>

/**
 * NAME:    os_rGetWorkingDir
 * PURPOSE: Get the current working directory
 *          path       - the buffer to store the path
 *          maxlen     - the size of the buffer
 *          abort_flag - if 1, failure of getcwd function will
 *                       lead to runtime error (abort)
 * RETURN:  strlen of the current working directory path
 */
int
os_rGetWorkingDir(char *path, unsigned int maxlen, int flag)
{
  GetCurrentDirectory(maxlen, path);
  path[maxlen - 1] = 0;
  return strlen(path);
}

/*
 * Local variables:
 *  compile-command: "make -C ../.."
 * End:
 */
