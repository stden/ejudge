/* -*- mode: c -*- */
/* $Id: vars.c 7460 2013-10-21 21:35:57Z cher $ */

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

#ifndef _MSC_VER
#include <dirent.h>
#endif

#include "l10n_impl.h"

FILE *f_in;
FILE *f_out;
FILE *f_corr;
FILE *f_arr[3];

// backward compatibility
FILE *f_team;

#ifndef _MSC_VER
DIR *dir_in;
DIR *dir_out;

char *dir_in_path;
char *dir_out_path;
#endif

const char * const f_arr_names[3] =
{
  __("test input data"),
  __("user program output"),
  __("test correct output")
};

struct testinfo_struct;
int (*testinfo_parse_func)() = 0;
const char *(*testinfo_strerror_func)() = 0;
