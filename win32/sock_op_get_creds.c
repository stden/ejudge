/* -*- mode: c -*- */
/* $Id: sock_op_get_creds.c 5513 2008-12-27 19:36:41Z cher $ */

/* Copyright (C) 2008 Alexander Chernov <cher@ejudge.ru> */

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

#include "sock_op.h"

#include <stdlib.h>

int
sock_op_get_creds(
        int sock_fd,
        int conn_id,
        int *p_pid,
        int *p_uid,
        int *p_gid)
{
  abort();
}

/*
 * Local variables:
 *  compile-command: "make -C .."
 *  c-font-lock-extra-types: ("\\sw+_t" "FILE")
 * End:
 */
