/* -*- mode: c -*- */
/* $Id: send_packet.c 6169 2011-03-27 12:34:46Z cher $ */

/* Copyright (C) 2002-2011 Alexander Chernov <cher@ejudge.ru> */

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

#include "userlist_clnt/private.h"

#include "errlog.h"

#include "reuse_integral.h"

#include <sys/uio.h>
#include <errno.h>

int
userlist_clnt_send_packet(struct userlist_clnt *clnt,
                          size_t size, void const *buf)
{
  const unsigned char *b;
  int w, n;
  rint32_t size32;
  struct iovec vv[2];

#if !defined PYTHON
  ASSERT(clnt);
  ASSERT(size > 0);
  ASSERT(clnt->fd >= 0);
#endif

  /* -1073741824 is 0xc0000000 or 0xffffffffc0000000 */
  if ((size & -1073741824L)) {
#if defined PYTHON
    PyErr_SetString(PyExc_ValueError, "packet length exceeds 1GiB");
    return -1;
#else
    err("send_packet: packet length exceeds 1GiB");
    return -ULS_ERR_WRITE_ERROR;
#endif
  }
  size32 = (ruint32_t) size;

  vv[0].iov_base = &size32;
  vv[0].iov_len = sizeof(size32); /* 4, actually */
  vv[1].iov_base = (void*) buf;
  vv[1].iov_len = size;

  n = writev(clnt->fd, vv, 2);
  if (n == size + 4) return 0;
  if (n <= 0) goto write_error;

  /* if the fast method did not work out, try the slow one */
  if (n < 4) {
    w = 4 - n;
    b = (const unsigned char*) &size32 + n;
    while (w > 0) {
      if ((n = write(clnt->fd, b, w)) <= 0) goto write_error;
      w -= n;
      b += n;
    }
    n = 4;
  }
  w = size + 4 - n;
  b = (const unsigned char*) buf + n - 4;
  while (w > 0) {
    if ((n = write(clnt->fd, b, w)) <= 0) goto write_error;
    w -= n;
    b += n;
  }

  return 0;

 write_error:
#if defined PYTHON
  PyErr_SetFromErrno(PyExc_IOError);
  close(clnt->fd);
  clnt->fd = -1;
  return -1;
#else
  n = errno;
  err("send_packet: write() failed: %s", os_ErrorMsg());
  close(clnt->fd);
  clnt->fd = -1;
  if (n == EPIPE) return -ULS_ERR_DISCONNECT;
  return -ULS_ERR_WRITE_ERROR;
#endif
}

/*
 * Local variables:
 *  compile-command: "make -C .."
 * End:
 */
