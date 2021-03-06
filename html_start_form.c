/* -*- mode: c -*- */
/* $Id: html_start_form.c 6147 2011-03-26 10:49:24Z cher $ */

/* Copyright (C) 2005-2011 Alexander Chernov <cher@ejudge.ru> */

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

#include "mischtml.h"

#include "reuse_logger.h"

#include <stdio.h>

static const unsigned char * const form_methods[] =
{
  "form method=\"get\" action=",
  "form method=\"post\" enctype=\"application/x-www-form-urlencoded\" action=",
  "form method=\"post\" enctype=\"multipart/form-data\" action=",
};

void
html_start_form_id(
        FILE *f, int mode,
        unsigned char const *self_url,
        unsigned char const *id,
        unsigned char const *hidden_vars)
{
  ASSERT(mode >= 0 && mode <= 2);
  fprintf(f, "<%s\"%s\" id=\"%s\">%s",
          form_methods[mode], self_url, id, hidden_vars);
}

void
html_start_form(FILE *f, int mode,
                unsigned char const *self_url,
                unsigned char const *hidden_vars)
{
  ASSERT(mode >= 0 && mode <= 2);
  fprintf(f, "<%s\"%s\">%s", form_methods[mode], self_url, hidden_vars);
}

/*
 * Local variables:
 *  compile-command: "make"
 *  c-font-lock-extra-types: ("\\sw+_t" "FILE" "va_list" "fd_set" "DIR")
 * End:
 */
