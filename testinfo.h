/* -*- c -*- */
/* $Id: testinfo.h 6584 2011-12-21 08:39:11Z cher $ */
#ifndef __TESTINFO_H__
#define __TESTINFO_H__

/* Copyright (C) 2003-2011 Alexander Chernov <cher@ejudge.ru> */

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

#ifdef __cplusplus
extern "C" {
#else
#endif /* __cplusplus */

/* error codes, actual error values are negative */
enum
{
  TINF_E_OK = 0,
  TINF_E_EOF,
  TINF_E_IO_ERROR,
  TINF_E_NO_MEMORY,
  TINF_E_UNCLOSED_QUOTE,
  TINF_E_STRAY_CONTROL_CHAR,
  TINF_E_INVALID_ESCAPE,
  TINF_E_IDENT_EXPECTED,
  TINF_E_EQUAL_EXPECTED,
  TINF_E_CANNOT_OPEN,
  TINF_E_INVALID_VAR_NAME,
  TINF_E_VAR_REDEFINED,
  TINF_E_EMPTY_VALUE,
  TINF_E_MULTIPLE_VALUE,
  TINF_E_INVALID_VALUE,

  TINF_E_LAST,
};

struct testinfo_struct
{
  int exit_code;
  int check_stderr;
  int disable_stderr;
  int cmd_argc;
  char **cmd_argv;
  char *comment;
  char *team_comment;
  int env_u;
  char **env_v;
};
typedef struct testinfo_struct testinfo_t;

int testinfo_parse(const char *path, testinfo_t *pt);
void testinfo_free(testinfo_t *pt);
const char *testinfo_strerror(int errcode);
unsigned char *testinfo_unparse_cmdline(const testinfo_t *pt);
unsigned char *testinfo_unparse_environ(const struct testinfo_struct *ti);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* __TESTINFO_H__ */

/*
 * Local variables:
 *  compile-command: "make"
 * End:
 */
